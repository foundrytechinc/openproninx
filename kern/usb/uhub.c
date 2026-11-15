// UHUB

#include "defs.h"
#include "malloc.h"
#include "usbdi.h"

#define RESET_TRIES 60

struct uhub_softc {
  struct usb_device *dev;
  int unit;
  int nports;
  int pwrgood_ms;
  int super;
  uint8_t *connected;
  struct usb_pipe intr;
};

static struct unrhdr uhub_units;

static void uhub_free(struct uhub_softc *sc) {
  free_unr(&uhub_units, sc->unit);
  if (sc->connected != 0)
    free(sc->connected, M_DEVBUF);
  free(sc, M_DEVBUF);
}

static int port_feature(struct usb_device *dev, int set, int feature,
                        int port) {
  return usbd_do_request(dev, UT_WRITE_CLASS_OTHER,
                         set ? UR_SET_FEATURE : UR_CLEAR_FEATURE,
                         (uint16_t)feature, (uint16_t)port, 0, 0);
}

static int port_status(struct usb_device *dev, int port,
                       struct usb_port_status *ps) {
  return usbd_do_request(dev, UT_READ_CLASS_OTHER, UR_GET_STATUS, 0,
                         (uint16_t)port, sizeof(*ps), ps);
}

static int port_speed(struct uhub_softc *sc, uint16_t status) {
  if (sc->super)
    return USB_SPEED_SUPER;
  if (status & UPS_LOW_SPEED)
    return USB_SPEED_LOW;
  if (status & UPS_HIGH_SPEED)
    return USB_SPEED_HIGH;
  return USB_SPEED_FULL;
}

static int uhub_reset_port(struct usb_device *dev, int port,
                           struct usb_port_status *ps, int super) {
  int i;

  if (port_feature(dev, 1, UHF_PORT_RESET, port) < 0)
    return -1;
  for (i = 0; i < RESET_TRIES; i++) {
    usb_pause_ms(10);
    if (port_status(dev, port, ps) < 0)
      return -1;
    if (ps->wPortChange & UPS_C_PORT_RESET)
      break;
    if ((ps->wPortStatus & UPS_CURRENT_CONNECT_STATUS) == 0)
      return -1;
  }
  port_feature(dev, 0, UHF_C_PORT_RESET, port);
  if (super)
    port_feature(dev, 0, UHF_C_BH_PORT_RESET, port);
  usb_pause_ms(10);
  if (port_status(dev, port, ps) < 0)
    return -1;
  return (ps->wPortStatus & UPS_PORT_ENABLED) ? 0 : -1;
}

int uhub_port_reset(struct usb_device *hub, int port) {
  struct uhub_softc *sc = hub->hubsoftc;
  struct usb_port_status ps;

  if (sc == 0 || port < 1 || port > sc->nports)
    return -1;
  return uhub_reset_port(hub, port, &ps, sc->super);
}

static int uhub_explore(struct usb_device *dev) {
  struct uhub_softc *sc = dev->hubsoftc;
  struct usb_port_status ps;
  int port, connected, speed;

  if (sc == 0)
    return -1;

  for (port = 1; port <= sc->nports; port++) {
    if (port_status(dev, port, &ps) < 0)
      continue;

    if (ps.wPortChange & UPS_C_CONNECT_STATUS)
      port_feature(dev, 0, UHF_C_PORT_CONNECTION, port);
    if (ps.wPortChange & UPS_C_PORT_ENABLED)
      port_feature(dev, 0, UHF_C_PORT_ENABLE, port);
    if (ps.wPortChange & UPS_C_OVERCURRENT_INDICATOR)
      port_feature(dev, 0, UHF_C_PORT_OVER_CURRENT, port);
    if (ps.wPortChange & UPS_C_PORT_LINK_STATE)
      port_feature(dev, 0, UHF_C_PORT_LINK_STATE, port);

    connected = (ps.wPortStatus & UPS_CURRENT_CONNECT_STATUS) != 0;
    if (connected == sc->connected[port - 1])
      continue;
    sc->connected[port - 1] = (uint8_t)connected;

    if (!connected) {
      if (dev->child[port - 1]) {
        printf("%s: device on port %d unplugged\n", dev->name, port);
        usbd_free_device(dev->child[port - 1]);
      }
      continue;
    }

    usb_pause_ms(100);
    if (uhub_reset_port(dev, port, &ps, sc->super) < 0) {
      printf("%s: port %d will not enable (status 0x%x change 0x%x)\n",
              dev->name, port, ps.wPortStatus, ps.wPortChange);
      sc->connected[port - 1] = 0;
      continue;
    }
    speed = port_speed(sc, ps.wPortStatus);
    usbd_new_device(dev->bus, dev, dev->depth + 1, speed, port, 0);
  }
  return 0;
}

static void uhub_intr(struct usb_pipe *pipe __unused, void *arg, int status, int len __unused) {
  struct uhub_softc *sc = arg;

  if (status == USBD_NORMAL || status == USBD_SHORT)
    usbd_needs_explore(sc->dev->bus);
}

static int uhub_match(struct usb_attach_arg *uaa) {
  if (uaa->iface != -1)
    return USB_MATCH_NONE;
  if (uaa->class_code != UCLASS_HUB)
    return USB_MATCH_NONE;
  return USB_MATCH_DEV_CLASS;
}

static int uhub_attach(struct usb_attach_arg *uaa) {
  struct usb_device *dev = uaa->dev;
  struct uhub_softc *sc;
  struct usb_hub_descriptor hd;
  const struct usb_endpoint_descriptor *ed;
  uint16_t chars;
  int port, tt_think, type;

  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->unit = alloc_unr(&uhub_units);
  sc->dev = dev;
  sc->super = dev->speed == USB_SPEED_SUPER;
  snprintf(dev->name, sizeof(dev->name), "uhub%d", sc->unit);

  type = sc->super ? UDESC_SS_HUB : UDESC_HUB;
  memset(&hd, 0, sizeof(hd));
  if (usbd_do_request(dev, UT_READ_CLASS_DEVICE, UR_GET_DESCRIPTOR,
                      (uint16_t)(type << 8), 0, 9, &hd) < 0) {
    printf("%s: no hub descriptor\n", dev->name);
    uhub_free(sc);
    return -1;
  }

  sc->nports = hd.bNbrPorts;
  if (sc->nports == 0) {
    printf("%s: a hub of no ports\n", dev->name);
    uhub_free(sc);
    return -1;
  }
  sc->connected = malloc((size_t)sc->nports, M_DEVBUF, M_WAITOK | M_ZERO);
  chars = hd.wHubCharacteristics;
  tt_think = (chars & UHD_TT_THINK) >> 5;
  sc->pwrgood_ms = hd.bPwrOn2PwrGood * 2;
  if (sc->pwrgood_ms < 20)
    sc->pwrgood_ms = 20;

  if (usbd_hub_ports(dev, sc->nports) < 0) {
    uhub_free(sc);
    return -1;
  }
  dev->hubsoftc = sc;
  if (dev->bus->methods->set_hub(dev, sc->nports, tt_think) < 0) {
    printf("%s: the controller would not take a hub\n", dev->name);
    dev->hubsoftc = 0;
    dev->nports = 0;
    uhub_free(sc);
    return -1;
  }

  for (port = 1; port <= sc->nports; port++)
    port_feature(dev, 1, UHF_PORT_POWER, port);
  usb_pause_ms(sc->pwrgood_ms);
  dev->powered_at = ticks;

  dev->explore = uhub_explore;
  uaa->softc = sc;

  ed = usbd_find_endpoint(dev, 0, 0);
  if (ed != 0 && UE_GET_DIR(ed->bEndpointAddress) &&
      UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT) {
    if (usbd_open_pipe(dev, ed, &sc->intr) == 0)
      usbd_start_pipe(&sc->intr, (sc->nports + 8) / 8, uhub_intr, sc);
  }

  printf("%s: %d port %s speed hub at depth %d\n", dev->name, sc->nports,
          usbd_speed_name(dev->speed), dev->depth);

  usbd_needs_explore(dev->bus);
  return 0;
}

static void uhub_detach(struct usb_device *dev, void *softc) {
  struct uhub_softc *sc = softc;

  usbd_close_pipe(&sc->intr);
  dev->hubsoftc = 0;
  dev->explore = 0;
  dev->nports = 0;
  uhub_free(sc);
}

static const struct usb_driver uhub_driver = {
    .name = "uhub",
    .match = uhub_match,
    .attach = uhub_attach,
    .detach = uhub_detach,
};

void uhub_init(void) {
  init_unrhdr(&uhub_units, "uhub");
  usbd_register_driver(&uhub_driver);
}
