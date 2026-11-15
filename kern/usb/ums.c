// UMS

#include "inc/abi.h"
#include "defs.h"
#include "hid.h"
#include "malloc.h"
#include "mutex.h"
#include "usbdi.h"

#define UMS_BUTTONS 8

struct ums_softc {
  struct usb_device *dev;
  int unit;
  char name[16];
  int iface;
  struct usb_pipe intr;
  int boot;
  int wsunit; // the wsmouse it feeds
  int report_id;
  int report_len;
  struct hid_location loc_x, loc_y, loc_z, loc_w;
  struct hid_location loc_btn[UMS_BUTTONS];
  int nbuttons;
  int absx, absy, revz;
  int lastx, lasty, haslast;
};

static struct unrhdr ums_units;

static void ums_intr(struct usb_pipe *pipe, void *arg, int status, int len) {
  struct ums_softc *sc = arg;
  const uint8_t *r = pipe->buf;
  int dx, dy, dz, dw, buttons = 0, i;

  if (status != USBD_NORMAL && status != USBD_SHORT)
    return;
  if (sc->report_id >= 0) {
    if (len < 1 || r[0] != (uint8_t)sc->report_id)
      return;
    r++;
    len--;
  }
  if (len <= 0)
    return;

  dx = hid_get_data(r, len, &sc->loc_x);
  dy = hid_get_data(r, len, &sc->loc_y);
  dz = sc->loc_z.size != 0 ? hid_get_data(r, len, &sc->loc_z) : 0;
  dw = sc->loc_w.size != 0 ? hid_get_data(r, len, &sc->loc_w) : 0;
  if (sc->revz)
    dz = -dz;
  if (dz == 0 && dw != 0)
    dz = dw > 0 ? -2 : 2;
  for (i = 0; i < sc->nbuttons; i++)
    if (hid_get_data(r, len, &sc->loc_btn[i]))
      buttons |= 1 << i;

  if (sc->absx || sc->absy) {
    int ax = dx, ay = dy;

    if (!sc->haslast) {
      sc->lastx = ax;
      sc->lasty = ay;
      sc->haslast = 1;
      wsmouse_input(sc->wsunit, 0, 0, dz, buttons);
      return;
    }
    dx = sc->absx ? ax - sc->lastx : dx;
    dy = sc->absy ? ay - sc->lasty : dy;
    sc->lastx = ax;
    sc->lasty = ay;
    wsmouse_input(sc->wsunit, dx, -dy, dz, buttons);
    return;
  }
  wsmouse_input(sc->wsunit, dx, -dy, dz, buttons);
}

static void ums_boot_layout(struct ums_softc *sc) {
  int i;

  sc->report_id = -1;
  sc->boot = 1;
  for (i = 0; i < 3; i++) {
    sc->loc_btn[i].pos = (uint32_t)i;
    sc->loc_btn[i].size = 1;
    sc->loc_btn[i].count = 1;
  }
  sc->nbuttons = 3;
  sc->loc_x.pos = 8;
  sc->loc_x.size = 8;
  sc->loc_x.count = 1;
  sc->loc_y.pos = 16;
  sc->loc_y.size = 8;
  sc->loc_y.count = 1;
  sc->loc_z.pos = 24;
  sc->loc_z.size = 8;
  sc->loc_z.count = 1;
  sc->loc_w.size = 0;
  sc->absx = sc->absy = sc->revz = 0;
  sc->haslast = 0;
  sc->report_len = 4;
}

static int ums_first_id(const uint8_t *desc, int dlen) {
  struct hid_data *d = hid_start_parse(desc, dlen, hid_input);
  struct hid_item h;
  int id = 0;

  if (d == 0)
    return 0;
  while (hid_get_item(d, &h))
    if (h.report_ID != 0) {
      id = (int)h.report_ID;
      break;
    }
  hid_end_parse(d);
  return id;
}

static int ums_is_mouse(const uint8_t *desc, int dlen) {
  uint8_t id = (uint8_t)ums_first_id(desc, dlen);

  return hid_is_collection(desc, dlen, id,
                           HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_MOUSE)) ||
         hid_is_collection(desc, dlen, id, HID_USAGE2(HUP_DIGITIZERS, HUD_PEN));
}

static int ums_parse(struct ums_softc *sc, const uint8_t *desc, int dlen) {
  uint32_t flags;
  int i, id;

  if (!ums_is_mouse(desc, dlen))
    return -1;
  id = ums_first_id(desc, dlen);
  sc->report_id = id != 0 ? id : -1;

  if (!hid_locate(desc, dlen, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X), id,
                  hid_input, &sc->loc_x, &flags))
    return -1;
  sc->absx = (flags & HIO_RELATIVE) == 0;
  if (!hid_locate(desc, dlen, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y), id,
                  hid_input, &sc->loc_y, &flags))
    return -1;
  sc->absy = (flags & HIO_RELATIVE) == 0;

  if (hid_locate(desc, dlen, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_WHEEL), id,
                 hid_input, &sc->loc_z, &flags) ||
      hid_locate(desc, dlen, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_TWHEEL), id,
                 hid_input, &sc->loc_z, &flags)) {
    sc->revz = 0;
    if (!hid_locate(desc, dlen, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Z), id,
                    hid_input, &sc->loc_w, &flags) &&
        !hid_locate(desc, dlen, HID_USAGE2(HUP_CONSUMER, HUC_AC_PAN), id,
                    hid_input, &sc->loc_w, &flags))
      sc->loc_w.size = 0;
  } else if (hid_locate(desc, dlen, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Z), id,
                        hid_input, &sc->loc_z, &flags)) {
    sc->revz = 1;
    sc->loc_w.size = 0;
  } else {
    sc->loc_z.size = 0;
    if (!hid_locate(desc, dlen, HID_USAGE2(HUP_CONSUMER, HUC_AC_PAN), id,
                    hid_input, &sc->loc_w, &flags))
      sc->loc_w.size = 0;
  }

  for (i = 1; i <= UMS_BUTTONS; i++)
    if (!hid_locate(desc, dlen, HID_USAGE2(HUP_BUTTON, i), id, hid_input,
                    &sc->loc_btn[i - 1], 0))
      break;
  sc->nbuttons = i - 1;
  sc->report_len = hid_report_size(desc, dlen, hid_input, (uint8_t)id);
  return sc->nbuttons > 0 ? 0 : -1;
}

static int ums_match(struct usb_attach_arg *uaa) {
  if (uaa->iface < 0 || uaa->class_code != UCLASS_HID)
    return USB_MATCH_NONE;
  if (uaa->subclass == USUBCLASS_BOOT && uaa->proto == UPROTO_BOOT_MOUSE)
    return USB_MATCH_IFACE_CLASS + 1;
  if (uaa->subclass != 0 || uaa->proto != 0)
    return USB_MATCH_NONE;
  {
    uint8_t *desc = (uint8_t *)page_alloc();
    int len, ok;

    if (desc == 0)
      return USB_MATCH_NONE;
    len = usbd_hid_report_desc(uaa->dev, uaa->iface, desc, USB_RDESC_MAX);
    ok = len > 0 && ums_is_mouse(desc, len);
    page_free((char *)desc);
    return ok ? USB_MATCH_IFACE_CLASS : USB_MATCH_NONE;
  }
}

static void ums_free(struct ums_softc *sc) {
  wsmouse_release(sc->wsunit);
  free_unr(&ums_units, sc->unit);
  free(sc, M_DEVBUF);
}

static int ums_attach(struct usb_attach_arg *uaa) {
  struct usb_device *dev = uaa->dev;
  struct ums_softc *sc;
  const struct usb_endpoint_descriptor *ed;
  int ifidx = 0, i, len;
  int bootiface =
      uaa->subclass == USUBCLASS_BOOT && uaa->proto == UPROTO_BOOT_MOUSE;

  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->unit = alloc_unr(&ums_units);
  snprintf(sc->name, sizeof(sc->name), "ums%d", sc->unit);
  sc->boot = 1;
  sc->dev = dev;
  sc->iface = uaa->iface;
  sc->wsunit = wsmouse_new();

  for (i = 0; i < USB_MAX_INTERFACES; i++) {
    const struct usb_interface_descriptor *id = usbd_find_iface(dev, i);

    if (id == 0)
      break;
    if (id->bInterfaceNumber == uaa->iface) {
      ifidx = i;
      break;
    }
  }

  {
    uint8_t *rdesc = (uint8_t *)page_alloc();
    int rlen;

    if (rdesc == 0) {
      ums_free(sc);
      return -1;
    }
    rlen = usbd_hid_report_desc(dev, uaa->iface, rdesc, USB_RDESC_MAX);
    if (rlen > 0 && ums_parse(sc, rdesc, rlen) == 0)
      sc->boot = 0;
    page_free((char *)rdesc);
    if (sc->boot != 0 && !bootiface) {
      ums_free(sc);
      return -1;
    }
    if (sc->boot != 0) {
      ums_boot_layout(sc);
      usbd_do_request(dev, UT_WRITE_CLASS_INTERFACE, UR_SET_PROTOCOL,
                      UHID_BOOT_PROTOCOL, (uint16_t)uaa->iface, 0, 0);
    } else {
      usbd_do_request(dev, UT_WRITE_CLASS_INTERFACE, UR_SET_PROTOCOL,
                      UHID_REPORT_PROTOCOL, (uint16_t)uaa->iface, 0, 0);
    }
  }
  usbd_do_request(dev, UT_WRITE_CLASS_INTERFACE, UR_SET_IDLE, 0,
                  (uint16_t)uaa->iface, 0, 0);

  ed = usbd_find_endpoint(dev, ifidx, 0);
  if (ed == 0 || !UE_GET_DIR(ed->bEndpointAddress) ||
      UE_GET_XFERTYPE(ed->bmAttributes) != UE_INTERRUPT) {
    ums_free(sc);
    return -1;
  }
  if (usbd_open_pipe(dev, ed, &sc->intr) < 0) {
    ums_free(sc);
    return -1;
  }
  wsmouse_set_hw(sc->wsunit, FNU_WSMOUSE_TYPE_USB, 0);
  wsmouse_attach(sc->wsunit);
  len = sc->report_len + (sc->report_id > 0 ? 1 : 0);
  if (usbd_start_pipe(&sc->intr, len, ums_intr, sc) < 0) {
    usbd_close_pipe(&sc->intr);
    wsmouse_detach(sc->wsunit);
    free_unr(&ums_units, sc->unit);
    free(sc, M_DEVBUF);
    return -1;
  }

  uaa->softc = sc;
  hotplug_attach(FNU_DEVCLASS_MOUSE, wsmouse_name(sc->wsunit), FNU_HOTPLUG_REMOVABLE, 0);
  printf("%s: %s speed mouse, %d button%s%s%s, %s\n", sc->name,
          usbd_speed_name(dev->speed), sc->nbuttons,
          sc->nbuttons == 1 ? "" : "s", sc->loc_z.size != 0 ? ", wheel" : "",
          sc->boot ? ", boot protocol" : (sc->absx || sc->absy ? ", absolute" : ""),
          wsmouse_name(sc->wsunit));
  return 0;
}

static void ums_detach(struct usb_device *dev, void *softc) {
  struct ums_softc *sc = softc;

  (void)dev;
  hotplug_detach(FNU_DEVCLASS_MOUSE, wsmouse_name(sc->wsunit), FNU_HOTPLUG_REMOVABLE, 0);
  usbd_close_pipe(&sc->intr);
  wsmouse_detach(sc->wsunit);
  free_unr(&ums_units, sc->unit);
  free(sc, M_DEVBUF);
}

static const struct usb_driver ums_driver = {
    .name = "ums",
    .match = ums_match,
    .attach = ums_attach,
    .detach = ums_detach,
};

void ums_init(void) {
  init_unrhdr(&ums_units, "ums");
  usbd_register_driver(&ums_driver);
}
