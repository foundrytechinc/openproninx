// USBDI

#include "defs.h"
#include "callout.h"
#include "malloc.h"
#include "proc.h"
#include "mutex.h"
#include "sleeplock.h"
#include "usbdi.h"
#include "x86.h"

static const struct usb_driver **drivers;
static int ndrivers;
static struct usb_bus *buses, **buses_tail = &buses;
static int nbuses;
static struct mutex usblock;
static struct callout media_timer;
static int explore_pending;
static int explore_settled;
static struct root_hold root_hold;
static int waking;
static int media_pending;
static int service_pending;
static struct sleeplock treelock;
static int sleeping;
static int letting_go; // a bus that lost its state: its devices are gone

// persist: where they were, waited for after the sleep
// every 20 ms, 2 s from port power
#define WAKE_PLACES 64
#define WAKE_CONNECT_MS 2000
#define WAKE_POLL_MS 20
static struct {
  struct usb_bus *bus;
  uint8_t rhport, depth;
  uint32_t route;
  int given_up;
} places[WAKE_PLACES];
static int nplaces;
static uint64_t woke_at;

const char *usbd_speed_name(int speed) {
  switch (speed) {
  case USB_SPEED_LOW:
    return "low";
  case USB_SPEED_FULL:
    return "full";
  case USB_SPEED_HIGH:
    return "high";
  case USB_SPEED_SUPER:
    return "super";
  default:
    return "?";
  }
}

#define MEDIA_CHECK_TICKS (2 * HZ)

static void media_tick(void *arg) {
  (void)arg;
  mtx_enter(&usblock);
  media_pending = 1;
  wakeup(&explore_pending);
  mtx_leave(&usblock);
  callout_reset(&media_timer, MEDIA_CHECK_TICKS, media_tick, 0);
}

void usbd_init(void) {
  mtx_init(&usblock, "usb");
  initsleeplock(&treelock, "usbtree");
  callout_init(&media_timer);
  callout_reset(&media_timer, MEDIA_CHECK_TICKS, media_tick, 0);
}

// SLEEP
int usbd_sleeping(void) { return sleeping; }

static void note_places(struct usb_device *dev) {
  int p;

  if (dev->depth > 0 && nplaces < WAKE_PLACES) {
    places[nplaces].bus = dev->bus;
    places[nplaces].rhport = dev->rhport;
    places[nplaces].depth = dev->depth;
    places[nplaces].route = dev->route;
    places[nplaces].given_up = 0;
    nplaces++;
  }
  for (p = 0; dev->child != 0 && p < dev->nports; p++)
    if (dev->child[p])
      note_places(dev->child[p]);
}

// a place not refilled yet, or 0
static struct usb_bus *place_missing(void) {
  struct usb_device *dev, *up;
  uint64_t since;
  int i, d, port;

  for (i = 0; i < nplaces; i++) {
    if (places[i].given_up)
      continue;
    dev = places[i].bus->root;
    up = 0;
    port = 0;
    for (d = 1; dev != 0 && d <= places[i].depth; d++) {
      port = d == 1 ? places[i].rhport
                    : (int)((places[i].route >> (4 * (d - 2))) & 0xf);
      up = dev;
      dev = dev->child != 0 && port >= 1 && port <= dev->nports
                ? dev->child[port - 1]
                : 0;
    }
    if (dev != 0)
      continue;
    since = up != 0 && up->depth > 0 && up->powered_at > woke_at ? up->powered_at : woke_at;
    if ((ticks - since) * 1000 / HZ < WAKE_CONNECT_MS)
      return places[i].bus;
    places[i].given_up = 1;
    if (d - 1 == places[i].depth)
      printf("usb: nothing came back on port %d of %s in %d ms\n", port,
             up->name, WAKE_CONNECT_MS);
  }
  return 0;
}

// devices stay, buses suspend their ports
void usbd_suspend(void) {
  struct usb_bus *b;

  acquiresleep(&treelock);
  sleeping = 1;
  nplaces = 0;
  for (b = buses; b != 0; b = b->next)
    note_places(b->root);
  for (b = buses; b != 0; b = b->next)
    if (b->methods->suspend != 0)
      b->methods->suspend(b);
}

// ehci kept state: resume the ports
// lost state: reset, drop the devices, find them again
void usbd_resume(int lost) {
  struct usb_bus *b;
  int p;

  for (b = buses; b != 0; b = b->next) {
    struct usb_device *root = b->root;

    if (b->methods->resume != 0 && b->methods->resume(b, lost) == 0)
      continue;
    letting_go = 1;
    for (p = 0; root->child != 0 && p < root->nports; p++)
      if (root->child[p])
        usbd_free_device(root->child[p]);
    letting_go = 0;
    if (b->methods->restart != 0)
      b->methods->restart(b);
  }
  sleeping = 0;
  waking = 1;
  woke_at = ticks;
  releasesleep(&treelock);
  mtx_enter(&usblock);
  for (b = buses; b != 0; b = b->next)
    b->explore_wanted = 1;
  explore_pending = 1;
  wakeup(&explore_pending);
  mtx_leave(&usblock);
}

int usbd_register_driver(const struct usb_driver *drv) {
  const struct usb_driver **grown;

  if (drv == 0)
    return -1;
  grown = malloc(((size_t)ndrivers + 1) * sizeof(*grown), M_DEVBUF, M_WAITOK);
  if (ndrivers > 0)
    memmove(grown, drivers, (size_t)ndrivers * sizeof(*grown));
  if (drivers != 0)
    free(drivers, M_DEVBUF);
  drivers = grown;
  drivers[ndrivers++] = drv;
  return 0;
}

// REQUESTS

int usbd_do_request(struct usb_device *dev, uint8_t type, uint8_t request,
                    uint16_t value, uint16_t index, uint16_t len, void *data) {
  struct usb_device_request req;
  int got = len;

  if (dev == 0 || dev->bus == 0)
    return -1;
  req.bmRequestType = type;
  req.bRequest = request;
  req.wValue = value;
  req.wIndex = index;
  req.wLength = len;
  if (dev->bus->methods->control(dev, &req, data, &got) < 0)
    return -1;
  return got;
}

int usbd_get_descriptor(struct usb_device *dev, uint8_t type, uint8_t index,
                        uint16_t len, void *data) {
  return usbd_do_request(dev, UT_READ_DEVICE, UR_GET_DESCRIPTOR,
                         (uint16_t)((type << 8) | index), 0, len, data);
}

// STRINGS
#define USBD_NOLANG (-1)
#define USB_LANGUAGE_TABLE 0

static int string_desc(struct usb_device *dev, int si, int langid, uint8_t *sd) {
  int len;

  if (usbd_do_request(dev, UT_READ_DEVICE, UR_GET_DESCRIPTOR,
                      (uint16_t)((UDESC_STRING << 8) | si), (uint16_t)langid, 2,
                      sd) < 2 || sd[1] != UDESC_STRING)
    return -1;
  len = sd[0] < 2 ? 2 : sd[0];
  return usbd_do_request(dev, UT_READ_DEVICE, UR_GET_DESCRIPTOR,
                         (uint16_t)((UDESC_STRING << 8) | si), (uint16_t)langid,
                         (uint16_t)len, sd);
}

int usbd_get_string(struct usb_device *dev, int si, char *buf, int buflen) {
  uint8_t sd[256];
  int size, i, n;
  uint16_t c;

  if (si == 0 || buflen <= 0)
    return -1;
  if (dev->langid == USBD_NOLANG) {
    size = string_desc(dev, USB_LANGUAGE_TABLE, 0, sd);
    dev->langid = size >= 4 ? sd[2] | (sd[3] << 8) : 0;
  }
  if ((size = string_desc(dev, si, dev->langid, sd)) < 2)
    return -1;
  n = size / 2 - 1;
  for (i = 0; i < n && i < buflen - 1; i++) {
    c = (uint16_t)(sd[2 + 2 * i] | (sd[3 + 2 * i] << 8));
    buf[i] = (c & 0xff00) == 0 ? (char)c : '?';
  }
  buf[i] = 0;
  return 0;
}

static void trim_spaces(char *p) {
  char *q = p, *e = p, c;

  while (*q == ' ')
    q++;
  while ((*p = *q++) != 0) {
    c = *p++;
    if (c != ' ' && c != '\n')
      e = p;
  }
  *e = 0;
}

int usbd_set_config(struct usb_device *dev, uint8_t config) {
  if (usbd_do_request(dev, UT_WRITE_DEVICE, UR_SET_CONFIG, config, 0, 0, 0) < 0)
    return -1;
  dev->config = config;
  return 0;
}

int usbd_set_interface(struct usb_device *dev, int iface, int alt) {
  return usbd_do_request(dev, UT_WRITE_INTERFACE, UR_SET_INTERFACE,
                         (uint16_t)alt, (uint16_t)iface, 0, 0);
}

// toggle goes to DATA0 with the halt. the controller's too
int usbd_clear_endpoint_stall(struct usb_pipe *pipe) {
  struct usb_device *dev = pipe->dev;

  if (dev->bus->methods->clear_toggle != 0)
    dev->bus->methods->clear_toggle(pipe);
  return usbd_do_request(dev, UT_WRITE_ENDPOINT, UR_CLEAR_FEATURE, 0,
                         pipe->edesc.bEndpointAddress, 0, 0);
}

// port reset, device back at its old address; pipes stay open at DATA0
int usbd_reset_device(struct usb_device *dev, struct usb_pipe **pipes, int n) {
  const struct usb_bus_methods *m = dev->bus->methods;
  struct usb_device *hub = dev->parent;
  int i, rc;

  // ???
  if (dev->gone || hub == 0 || m->readdress == 0)
    return -1;
  if (hub->depth == 0)
    rc = m->root_reset != 0 ? m->root_reset(dev->bus, dev->port) : -1;
  else
    rc = uhub_port_reset(hub, dev->port);
  if (rc < 0 || m->readdress(dev) < 0 || usbd_set_config(dev, dev->config) < 0) {
    printf("usb: the device on port %d would not come back\n", dev->port);
    return -1;
  }
  for (i = 0; i < n; i++)
    if (m->clear_toggle != 0)
      m->clear_toggle(pipes[i]);
  return 0;
}

// THE CONFIGURATION DESCRIPTOR

const struct usb_interface_descriptor *usbd_find_iface(struct usb_device *dev,
                                                       int index) {
  int off = 0, seen = -1;

  while (off + 2 <= dev->cdesc_len) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > dev->cdesc_len)
      break;
    if (d[1] == UDESC_INTERFACE && d[0] >= 9) {
      const struct usb_interface_descriptor *id =
          (const struct usb_interface_descriptor *)d;
      if (id->bAlternateSetting == 0 && ++seen == index)
        return id;
    }
    off += d[0];
  }
  return 0;
}

const struct usb_endpoint_descriptor *usbd_find_endpoint(struct usb_device *dev,
                                                         int iface,
                                                         int index) {
  int off = 0, cur = -1, seen = -1;

  while (off + 2 <= dev->cdesc_len) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > dev->cdesc_len)
      break;
    if (d[1] == UDESC_INTERFACE && d[0] >= 9) {
      const struct usb_interface_descriptor *id =
          (const struct usb_interface_descriptor *)d;
      if (id->bAlternateSetting == 0)
        cur++;
    } else if (d[1] == UDESC_ENDPOINT && d[0] >= 7 && cur == iface) {
      if (++seen == index)
        return (const struct usb_endpoint_descriptor *)d;
    }
    off += d[0];
  }
  return 0;
}

const struct usb_interface_descriptor *
usbd_find_altiface(struct usb_device *dev, int ifaceno, int alt) {
  int off = 0;

  while (off + 2 <= dev->cdesc_len) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > dev->cdesc_len)
      break;
    if (d[1] == UDESC_INTERFACE && d[0] >= 9) {
      const struct usb_interface_descriptor *id =
          (const struct usb_interface_descriptor *)d;
      if (id->bInterfaceNumber == ifaceno && id->bAlternateSetting == alt)
        return id;
    }
    off += d[0];
  }
  return 0;
}

static int alt_body(struct usb_device *dev, int ifaceno, int alt, int *end) {
  int off = 0, in = 0, start = -1;

  while (off + 2 <= dev->cdesc_len) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > dev->cdesc_len)
      break;
    if (d[1] == UDESC_INTERFACE && d[0] >= 9) {
      const struct usb_interface_descriptor *id =
          (const struct usb_interface_descriptor *)d;
      if (in) {
        *end = off;
        return start;
      }
      if (id->bInterfaceNumber == ifaceno && id->bAlternateSetting == alt) {
        in = 1;
        start = off + d[0];
      }
    }
    off += d[0];
  }
  *end = dev->cdesc_len;
  return in ? start : -1;
}

const struct usb_endpoint_descriptor *
usbd_find_alt_endpoint(struct usb_device *dev, int ifaceno, int alt,
                       int index) {
  int end, off = alt_body(dev, ifaceno, alt, &end), seen = -1;

  if (off < 0)
    return 0;
  while (off + 2 <= end) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > end)
      break;
    if (d[1] == UDESC_ENDPOINT && d[0] >= 7 && ++seen == index)
      return (const struct usb_endpoint_descriptor *)d;
    off += d[0];
  }
  return 0;
}

const void *usbd_find_alt_class_desc(struct usb_device *dev, int ifaceno,
                                     int alt, uint8_t type, int index) {
  int end, off = alt_body(dev, ifaceno, alt, &end), seen = -1;

  if (off < 0)
    return 0;
  while (off + 2 <= end) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > end)
      break;
    if (d[1] == type && ++seen == index)
      return d;
    off += d[0];
  }
  return 0;
}

const void *usbd_find_class_desc(struct usb_device *dev, int iface,
                                 uint8_t type) {
  int off = 0, cur = -1;

  while (off + 2 <= dev->cdesc_len) {
    const uint8_t *d = dev->cdesc + off;

    if (d[0] < 2 || off + d[0] > dev->cdesc_len)
      break;
    if (d[1] == UDESC_INTERFACE && d[0] >= 9) {
      const struct usb_interface_descriptor *id =
          (const struct usb_interface_descriptor *)d;
      if (id->bAlternateSetting == 0)
        cur++;
    } else if (d[1] == type && cur == iface) {
      return d;
    }
    off += d[0];
  }
  return 0;
}

int usbd_hid_report_desc(struct usb_device *dev, int iface, uint8_t *buf,
                         int max) {
  const struct usb_hid_descriptor *hid;
  int i, ifidx = -1, len;

  for (i = 0; i < USB_MAX_INTERFACES; i++) {
    const struct usb_interface_descriptor *id = usbd_find_iface(dev, i);

    if (id == 0)
      break;
    if (id->bInterfaceNumber == iface) {
      ifidx = i;
      break;
    }
  }
  if (ifidx < 0)
    return 0;
  hid = usbd_find_class_desc(dev, ifidx, UDESC_HID);
  if (hid == 0)
    return 0;
  len = hid->wReportDescriptorLength;
  if (len <= 0 || len > max)
    return 0;
  if (usbd_do_request(dev, UT_READ_INTERFACE, UR_GET_DESCRIPTOR,
                      (uint16_t)(UDESC_REPORT << 8), (uint16_t)iface,
                      (uint16_t)len, buf) < 0)
    return 0;
  return len;
}

// PIPES

int usbd_open_pipe(struct usb_device *dev,
                   const struct usb_endpoint_descriptor *ed,
                   struct usb_pipe *pipe) {
  if (dev == 0 || ed == 0 || pipe == 0)
    return -1;
  memset(pipe, 0, sizeof(*pipe));
  pipe->dev = dev;
  pipe->edesc = *ed;
  pipe->maxp = ed->wMaxPacketSize & 0x7ff;
  if (pipe->maxp == 0)
    return -1;
  pipe->isoc = UE_GET_XFERTYPE(ed->bmAttributes) == UE_ISOCHRONOUS;
  if (dev->speed == USB_SPEED_HIGH &&
      (pipe->isoc || UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT))
    pipe->burst = (uint8_t)((ed->wMaxPacketSize >> 11) & 3);
  else if (dev->speed == USB_SPEED_SUPER) {
    const uint8_t *d = (const uint8_t *)ed + ed->bLength;
    const struct usb_ss_ep_comp_descriptor *cd =
        (const struct usb_ss_ep_comp_descriptor *)d;

    if (d + 6 <= dev->cdesc + dev->cdesc_len && d[0] >= 6 &&
        d[1] == UDESC_ENDPOINT_SS_COMP) {
      pipe->burst = cd->bMaxBurst;
      pipe->bpi = cd->wBytesPerInterval;
    }
  }
  if (dev->bus->methods->open_pipe(pipe) < 0)
    return -1;
  pipe->open = 1;
  return 0;
}

void usbd_close_pipe(struct usb_pipe *pipe) {
  if (pipe == 0 || !pipe->open)
    return;
  pipe->repeat = 0;
  pipe->dev->bus->methods->close_pipe(pipe);
  pipe->open = 0;
}

int usbd_start_pipe(struct usb_pipe *pipe, int len,
                    void (*done)(struct usb_pipe *, void *, int, int),
                    void *arg) {
  if (pipe == 0 || !pipe->open || len <= 0 || len > pipe->buflen)
    return -1;
  pipe->done = done;
  pipe->arg = arg;
  pipe->xfer_len = len;
  pipe->repeat = 1;
  return pipe->dev->bus->methods->start(pipe);
}

int usbd_isoc_start(struct usb_pipe *pipe, int seglen, int nseg,
                    void (*done)(struct usb_pipe *, void *, int, int),
                    void *arg) {
  if (pipe == 0 || !pipe->open || !pipe->isoc || done == 0)
    return -1;
  if (seglen <= 0 || seglen > USB_ISOC_STRIDE || nseg <= 0 || nseg > USB_ISOC_SEGS)
    return -1;
  if (USB_ISOC_STRIDE * nseg > pipe->buflen)
    return -1;
  if (pipe->dev->bus->methods->isoc_start == 0)
    return -1;
  pipe->done = done;
  pipe->arg = arg;
  pipe->seglen = seglen;
  pipe->nseg = nseg;
  pipe->seg = 0;
  pipe->seg_next = 0;
  pipe->xfer_len = seglen;
  pipe->repeat = 1;
  return pipe->dev->bus->methods->isoc_start(pipe);
}

// a one shot transfer
int usbd_bulk_transfer(struct usb_pipe *pipe, void *data, int len, int ms) {
  int in;

  if (pipe == 0 || !pipe->open || len < 0 || len > pipe->buflen)
    return -1;
  if (pipe->dev->bus->methods->transfer == 0)
    return -1;
  in = UE_GET_DIR(pipe->edesc.bEndpointAddress) != 0;
  if (!in && data != 0 && len > 0)
    memmove(pipe->buf, data, len);
  if (pipe->dev->bus->methods->transfer(pipe, len, ms) < 0)
    return -1;
  if (in && data != 0 && pipe->sync_len > 0)
    memmove(data, pipe->buf, pipe->sync_len);
  return pipe->sync_len;
}

// DEVICES

static struct usb_device *device_alloc(void) {
  struct usb_device *dev = malloc(sizeof(*dev), M_DEVBUF, M_WAITOK | M_ZERO);

  dev->in_use = 1;
  return dev;
}

static int default_maxp0(int speed) {
  switch (speed) {
  case USB_SPEED_LOW:
    return 8;
  case USB_SPEED_FULL:
    return 8;
  case USB_SPEED_HIGH:
    return 64;
  default:
    return 512;
  }
}

static int try_drivers(struct usb_attach_arg *uaa) {
  const struct usb_driver *best = 0;
  struct usb_attachment *a;
  int i, score, bestscore = 0;

  for (i = 0; i < ndrivers; i++) {
    if (drivers[i]->match == 0)
      continue;
    score = drivers[i]->match(uaa);
    if (score > bestscore) {
      bestscore = score;
      best = drivers[i];
    }
  }
  if (best == 0)
    return 0;
  uaa->softc = 0;
  if (best->attach(uaa) < 0)
    return 0;
  a = malloc(sizeof(*a), M_DEVBUF, M_WAITOK | M_ZERO);
  a->driver = best;
  a->softc = uaa->softc;
  a->iface = uaa->iface;
  a->next = uaa->dev->attached;
  uaa->dev->attached = a;
  return 1;
}

static void usbd_probe(struct usb_device *dev) {
  struct usb_attach_arg uaa;
  const struct usb_interface_descriptor *id;
  int i, claimed = 0;

  memset(&uaa, 0, sizeof(uaa));
  uaa.dev = dev;
  uaa.iface = -1;
  uaa.class_code = dev->ddesc.bDeviceClass;
  uaa.subclass = dev->ddesc.bDeviceSubClass;
  uaa.proto = dev->ddesc.bDeviceProtocol;
  uaa.vendor = dev->ddesc.idVendor;
  uaa.product = dev->ddesc.idProduct;
  if (try_drivers(&uaa))
    return;

  for (i = 0; i < USB_MAX_INTERFACES; i++) {
    id = usbd_find_iface(dev, i);
    if (id == 0)
      break;
    memset(&uaa, 0, sizeof(uaa));
    uaa.dev = dev;
    uaa.iface = id->bInterfaceNumber;
    uaa.idesc = id;
    uaa.class_code = id->bInterfaceClass;
    uaa.subclass = id->bInterfaceSubClass;
    uaa.proto = id->bInterfaceProtocol;
    uaa.vendor = dev->ddesc.idVendor;
    uaa.product = dev->ddesc.idProduct;
    if (try_drivers(&uaa))
      claimed++;
  }

  if (claimed == 0 && !waking)
    printf("usb: %s speed device %04x:%04x class %d has no driver\n",
            usbd_speed_name(dev->speed), dev->ddesc.idVendor,
            dev->ddesc.idProduct, dev->ddesc.bDeviceClass);
}

int usbd_new_device(struct usb_bus *bus, struct usb_device *parent, int depth,
                    int speed, int port, struct usb_device **out) {
  struct usb_device *dev;
  struct usb_device_descriptor dd;
  struct usb_config_descriptor cd;
  int len, maxp0;

  if (bus == 0 || depth >= USB_MAX_DEPTH)
    return -1;
  dev = device_alloc();

  dev->bus = bus;
  dev->parent = parent;
  dev->depth = (uint8_t)depth;
  dev->speed = (uint8_t)speed;
  dev->port = (uint8_t)port;
  dev->maxp0 = (uint16_t)default_maxp0(speed);

  if (parent == 0 || parent->depth == 0) {
    dev->rhport = (uint8_t)port;
    dev->route = 0;
  } else {
    dev->rhport = parent->rhport;
    dev->route = parent->route;
    if (depth >= 2)
      dev->route |= (uint32_t)(port & 0xf) << (4 * (depth - 2));
    if (speed < USB_SPEED_HIGH) {
      if (parent->speed == USB_SPEED_HIGH) {
        dev->hs_hub = parent;
        dev->hs_port = (uint8_t)port;
      } else {
        dev->hs_hub = parent->hs_hub;
        dev->hs_port = parent->hs_port;
      }
    }
  }

  if (bus->methods->new_device(dev) < 0) {
    printf("usb: could not address the device on port %d\n", port);
    goto fail;
  }

  memset(&dd, 0, sizeof(dd));
  if (usbd_get_descriptor(dev, UDESC_DEVICE, 0, 8, &dd) < 0) {
    printf("usb: no device descriptor on port %d\n", port);
    goto fail;
  }
  maxp0 = dd.bMaxPacketSize0;
  if (speed == USB_SPEED_SUPER)
    maxp0 = 1 << dd.bMaxPacketSize0;
  if (maxp0 >= 8 && maxp0 != dev->maxp0) {
    dev->maxp0 = (uint16_t)maxp0;
    if (bus->methods->set_maxp0(dev) < 0)
      goto fail;
  }

  if (usbd_get_descriptor(dev, UDESC_DEVICE, 0, sizeof(dev->ddesc),
                          &dev->ddesc) < 0)
    goto fail;
  dev->langid = USBD_NOLANG;
  if (usbd_get_string(dev, dev->ddesc.iSerialNumber, dev->serial,
                      sizeof(dev->serial)) == 0)
    trim_spaces(dev->serial);
  else
    dev->serial[0] = 0;

  memset(&cd, 0, sizeof(cd));
  if (usbd_get_descriptor(dev, UDESC_CONFIG, 0, sizeof(cd), &cd) <
      (int)sizeof(cd))
    goto fail;
  len = cd.wTotalLength;
  if (len < 9 || len > USB_CDESC_MAX)
    goto fail;
  if (usbd_get_descriptor(dev, UDESC_CONFIG, 0, (uint16_t)len, dev->cdesc) < 0)
    goto fail;
  dev->cdesc_len = len;

  if (usbd_set_config(dev, cd.bConfigurationValue) < 0)
    goto fail;

  if (parent != 0 && parent->child != 0 && port >= 1 && port <= parent->nports)
    parent->child[port - 1] = dev;
  if (out)
    *out = dev;

  usbd_probe(dev);
  return 0;

fail:
  usbd_free_device(dev);
  return -1;
}

int usbd_hub_ports(struct usb_device *dev, int nports) {
  if (nports <= 0 || nports > USB_MAX_PORTS)
    return -1;
  if (dev->child != 0)
    free(dev->child, M_DEVBUF);
  dev->child = malloc((size_t)nports * sizeof(*dev->child), M_DEVBUF,
                      M_WAITOK | M_ZERO);
  dev->nports = nports;
  return 0;
}

void usbd_free_device(struct usb_device *dev) {
  struct usb_attachment *a;
  int i;

  if (dev == 0 || !dev->in_use)
    return;
  for (i = 0; dev->child != 0 && i < dev->nports; i++)
    if (dev->child[i]) {
      usbd_free_device(dev->child[i]);
      dev->child[i] = 0;
    }
  if (!sleeping || letting_go) {
    dev->gone = 1;
    if (dev->hcpriv && dev->bus->methods->abort_device)
      dev->bus->methods->abort_device(dev);
  }
  while ((a = dev->attached) != 0) {
    dev->attached = a->next;
    if (a->driver->detach)
      a->driver->detach(dev, a->softc);
    free(a, M_DEVBUF);
  }
  if (dev->parent && dev->parent->child != 0 && dev->port >= 1 &&
      dev->port <= dev->parent->nports)
    dev->parent->child[dev->port - 1] = 0;
  if (dev->child) {
    free(dev->child, M_DEVBUF);
    dev->child = 0;
  }
  dev->nports = 0;
  if (dev->hcpriv)
    dev->bus->methods->free_device(dev);
  dev->in_use = 0;
  // a root is its controller's
  if (dev->depth > 0)
    free(dev, M_DEVBUF);
}

// a thread sleeps; cold or spinlocked, it spins
static int nowake;

void usb_pause_ms(uint ms) {
  if (curproc == 0 || !(readeflags() & FL_IF)) {
    delay_ms(ms);
    return;
  }
  tsleep_nsec(&nowake, PWAIT, "usbpause", (uint64_t)ms * 1000000);
}

// at panic: each controller's completions, unlocked, pray
int usbd_polling;

void usbd_poll_all(void) {
  struct usb_bus *b;

  usbd_polling = 1;
  for (b = buses; b != 0; b = b->next)
    if (b->methods->poll != 0)
      b->methods->poll(b);
}

// EXPLORATION

void usbd_attach_bus(struct usb_bus *bus) {
  mtx_enter(&usblock);
  bus->next = 0;
  *buses_tail = bus;
  buses_tail = &bus->next;
  nbuses++;
  if (!explore_settled)
    root_mount_hold(&root_hold, "usb");
  mtx_leave(&usblock);
  usbd_needs_explore(bus);
}

void usbd_needs_explore(struct usb_bus *bus) {
  mtx_enter(&usblock);
  if (bus)
    bus->explore_wanted = 1;
  explore_pending = 1;
  wakeup(&explore_pending);
  mtx_leave(&usblock);
}

void usbd_needs_service(void) {
  mtx_enter(&usblock);
  service_pending = 1;
  wakeup(&explore_pending);
  mtx_leave(&usblock);
}

static void service_tree(struct usb_device *dev) {
  struct usb_attachment *a;
  int i;

  if (dev == 0)
    return;
  for (a = dev->attached; a != 0; a = a->next)
    if (a->driver->service)
      a->driver->service(dev, a->softc);
  for (i = 0; dev->child != 0 && i < dev->nports; i++)
    if (dev->child[i])
      service_tree(dev->child[i]);
}

static void media_tree(struct usb_device *dev) {
  struct usb_attachment *a;
  int i;

  if (dev == 0)
    return;
  for (a = dev->attached; a != 0; a = a->next)
    if (a->driver->media)
      a->driver->media(dev, a->softc);
  for (i = 0; dev->child != 0 && i < dev->nports; i++)
    if (dev->child[i])
      media_tree(dev->child[i]);
}

static void explore_tree(struct usb_device *dev) {
  int i;

  if (dev == 0)
    return;
  if (dev->explore)
    dev->explore(dev);
  for (i = 0; dev->child != 0 && i < dev->nports; i++)
    if (dev->child[i])
      explore_tree(dev->child[i]);
}

void usbd_explore_thread(void *arg) {
  struct usb_bus *b, *missing;
  int i, service, media, poll = 0;

  (void)arg;
  mtx_enter(&usblock);
  for (;;) {
    while (!explore_pending && !media_pending && !service_pending)
      msleep(&explore_pending, &usblock, PRIBIO, "usbwait", 0);
    explore_pending = 0;
    service = service_pending;
    service_pending = 0;
    media = media_pending;
    media_pending = 0;
    mtx_leave(&usblock);

    acquiresleep(&treelock);
    if (service)
      for (b = buses; b != 0; b = b->next)
        service_tree(b->root);

    for (b = buses; b != 0; b = b->next) {
      mtx_enter(&usblock);
      i = b->explore_wanted;
      b->explore_wanted = 0;
      mtx_leave(&usblock);
      if (i)
        explore_tree(b->root);
    }

    if (media)
      for (b = buses; b != 0; b = b->next)
        media_tree(b->root);

    mtx_enter(&usblock);
    if ((!explore_settled || waking) && !explore_pending) {
      for (b = buses; b != 0 && !b->explore_wanted; b = b->next)
        ;
      missing = b == 0 && waking ? place_missing() : 0;
      if (missing != 0) {
        missing->explore_wanted = 1;
        explore_pending = 1;
        poll = 1;
      } else if (b == 0 && nbuses > 0) {
        explore_settled = 1;
        root_mount_rel(&root_hold);
        if (waking) {
          waking = 0;
          mtx_leave(&usblock);
          for (i = 0; i < ndrivers; i++)
            if (drivers[i]->woke != 0)
              drivers[i]->woke();
          mtx_enter(&usblock);
        }
      }
    }
    mtx_leave(&usblock);
    releasesleep(&treelock);
    if (poll) {
      poll = 0;
      usb_pause_ms(WAKE_POLL_MS);
    }

    mtx_enter(&usblock);
  }
}
