// UKBD
#include "inc/abi.h"
#include "defs.h"
#include "callout.h"
#include "kbdreg.h"
#include "malloc.h"
#include "mutex.h"
#include "param.h"
#include "hid.h"
#include "usbdi.h"

#define EXT 0x100
#define MAXKEYS 12
#define MAXVARS 16

struct ukbd_var {
  struct hid_location loc;
  uint8_t key;
};

struct ukbd_softc {
  struct usb_device *dev;
  int unit;
  char name[16];
  int iface;
  struct usb_pipe intr;
  struct callout repeat;

  int report_id;
  int report_len;
  struct ukbd_var var[MAXVARS];
  int nvar;
  struct hid_location keyloc;
  int nkeys;
  struct hid_location numloc;
  struct hid_location capsloc;
  struct hid_location scroloc;
  int led_id;
  int nleds;
  int boot;

  uint8_t prev[MAXKEYS];
  uint8_t prevvar[MAXVARS];
  uint8_t held;
  uint leds;
  int leds_dirty;

  keyboard_t kbd;
  struct kbdxlate x;
  struct fnu_keymap keymap;
  struct fnu_accentmap accmap;
  struct fnu_fkeytab fkeytab[FNU_KB_NFKEYS];
  struct ukbd_softc *next;
  uint32 polled[16];
  int phead, ptail;
};

// polling: keys here for cncheckc, not to the console
static struct ukbd_softc *ukbd_list;
static int ukbd_polling;

static struct unrhdr ukbd_units;

static const uint16_t usagemap[] = {
    [0x04] = 0x1e, [0x05] = 0x30, [0x06] = 0x2e, [0x07] = 0x20,
    [0x08] = 0x12, [0x09] = 0x21, [0x0a] = 0x22, [0x0b] = 0x23,
    [0x0c] = 0x17, [0x0d] = 0x24, [0x0e] = 0x25, [0x0f] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1f, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2f, [0x1a] = 0x11, [0x1b] = 0x2d,
    [0x1c] = 0x15, [0x1d] = 0x2c, [0x1e] = 0x02, [0x1f] = 0x03,
    [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0a, [0x27] = 0x0b,
    [0x28] = 0x1c, [0x29] = 0x01, [0x2a] = 0x0e, [0x2b] = 0x0f,
    [0x2c] = 0x39, [0x2d] = 0x0c, [0x2e] = 0x0d, [0x2f] = 0x1a,
    [0x30] = 0x1b, [0x31] = 0x2b, [0x32] = 0x2b, [0x33] = 0x27,
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34,
    [0x38] = 0x35, [0x39] = 0x3a, [0x3a] = 0x3b, [0x3b] = 0x3c,
    [0x3c] = 0x3d, [0x3d] = 0x3e, [0x3e] = 0x3f, [0x3f] = 0x40,
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44,
    [0x44] = 0x57, [0x45] = 0x58, [0x47] = 0x46,
    [0x49] = EXT | 0x52, [0x4a] = EXT | 0x47, [0x4b] = EXT | 0x49,
    [0x4c] = EXT | 0x53, [0x4d] = EXT | 0x4f, [0x4e] = EXT | 0x51,
    [0x4f] = EXT | 0x4d, [0x50] = EXT | 0x4b, [0x51] = EXT | 0x50,
    [0x52] = EXT | 0x48, [0x53] = 0x45,       [0x54] = EXT | 0x35,
    [0x55] = 0x37,       [0x56] = 0x4a,       [0x57] = 0x4e,
    [0x58] = EXT | 0x1c, [0x59] = 0x4f,       [0x5a] = 0x50,
    [0x5b] = 0x51,       [0x5c] = 0x4b,       [0x5d] = 0x4c,
    [0x5e] = 0x4d,       [0x5f] = 0x47,       [0x60] = 0x48,
    [0x61] = 0x49,       [0x62] = 0x52,       [0x63] = 0x53,
    [0x64] = 0x56,       [0x65] = EXT | 0x5d,
    [0xe0] = 0x1d,       [0xe1] = 0x2a,       [0xe2] = 0x38,
    [0xe3] = EXT | 0x5b, [0xe4] = EXT | 0x1d, [0xe5] = 0x36,
    [0xe6] = EXT | 0x38, [0xe7] = EXT | 0x5c,
};

static uint16_t lookup(uint8_t usage) {
  if (usage >= sizeof(usagemap) / sizeof(usagemap[0]))
    return 0;
  return usagemap[usage];
}

static void deliver(struct ukbd_softc *sc, uint32 c) {
  if (!ukbd_polling) {
    console_key(&sc->kbd, (int)c);
    return;
  }
  if ((sc->ptail + 1) % 16 != sc->phead) {
    sc->polled[sc->ptail] = c;
    sc->ptail = (sc->ptail + 1) % 16;
  }
}

static void feed(struct ukbd_softc *sc, uint byte) {
  uint32 c;

  if ((c = genkbd_pending(&sc->x)) != FNU_NOKEY)
    deliver(sc, c);
  if ((c = genkbd_scancode(&sc->kbd, &sc->x, (int)byte)) != FNU_NOKEY)
    deliver(sc, c);
  if ((c = genkbd_pending(&sc->x)) != FNU_NOKEY)
    deliver(sc, c);
}

static void emit(struct ukbd_softc *sc, uint16_t code, int release) {
  if (code == 0)
    return;
  if (code & EXT)
    feed(sc, 0xe0);
  feed(sc, (code & 0xff) | (release ? 0x80 : 0));
}

static int ukbd_ioctl(keyboard_t *kbd, uint64_t cmd, void *arg) {
  struct ukbd_softc *sc = kbd->kb_data;
  int i;

  switch (cmd) {
  case FNU_KBSETLEDS:
    if (*(int *)arg & ~FNU_KB_LOCKS)
      return FNU_ERR_INVALID;
    i = *(int *)arg;
    if (sc->x.mode == FNU_KB_XLATE && kbd->kb_keymap->n_keys > FNU_KB_ALTGR) {
      if (i & FNU_KB_ALKED)
        i |= FNU_KB_CLKED;
      else
        i &= ~FNU_KB_CLKED;
    }
    sc->leds = ((i & FNU_KB_LEDNUM) ? 1 : 0) | ((i & FNU_KB_LEDCAP) ? 2 : 0) |
               ((i & FNU_KB_LEDSCR) ? 4 : 0);
    sc->leds_dirty = 1;
    if (!ukbd_polling)
      usbd_needs_service();
    KBD_LED_VAL(kbd) = *(int *)arg;
    return 0;
  case FNU_KBSETREPEAT:
    kbd->kb_delay1 = ((int *)arg)[0];
    kbd->kb_delay2 = ((int *)arg)[1];
    return 0;
  case FNU_KBSETRATE:
    return 0;
  default:
    return genkbd_modeioctl(kbd, &sc->x, cmd, arg);
  }
}

static void ukbd_clear_state(keyboard_t *kbd) {
  struct ukbd_softc *sc = kbd->kb_data;

  genkbd_clear_state(&sc->x);
}

static const struct keyboard_switch ukbdsw = {
    .ioctl = ukbd_ioctl,
    .clear_state = ukbd_clear_state,
};

static int in_list(const uint8_t *list, int n, uint8_t usage) {
  int i;

  for (i = 0; i < n; i++)
    if (list[i] == usage)
      return 1;
  return 0;
}

static int ukbd_led_id(const uint8_t *d, int len) {
  struct hid_data *h;
  struct hid_item it;
  int id = 0;

  h = hid_start_parse(d, len, hid_output);
  if (h == 0)
    return 0;
  while (hid_get_item(h, &it))
    if (HID_GET_USAGE_PAGE(it.usage) == HUP_LED) {
      id = (int)it.report_ID;
      break;
    }
  hid_end_parse(h);
  return id;
}

static int ukbd_first_id(const uint8_t *d, int len) {
  struct hid_data *h = hid_start_parse(d, len, hid_input);
  struct hid_item it;
  int id = -1;

  if (h == 0)
    return -1;
  while (hid_get_item(h, &it))
    if (it.kind == hid_input && !(it.flags & HIO_CONST) &&
        HID_GET_USAGE_PAGE(it.usage) == HUP_KEYBOARD) {
      id = (int)it.report_ID;
      break;
    }
  hid_end_parse(h);
  return id;
}

static int ukbd_is_keyboard(const uint8_t *d, int len) {
  int id = ukbd_first_id(d, len);

  if (id < 0)
    return 0;
  return hid_is_collection(d, len, (uint8_t)id,
                           HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_KEYBOARD)) ||
         hid_is_collection(d, len, (uint8_t)id,
                           HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_KEYPAD));
}

static int ukbd_parse(struct ukbd_softc *sc, const uint8_t *d, int len) {
  struct hid_data *h;
  struct hid_item it;
  int id, lid;

  sc->nvar = 0;
  sc->nkeys = 0;
  sc->nleds = 0;
  sc->report_id = -1;

  if (!ukbd_is_keyboard(d, len))
    return -1;
  id = ukbd_first_id(d, len);

  h = hid_start_parse(d, len, hid_input);
  if (h == 0)
    return -1;
  while (hid_get_item(h, &it)) {
    if (it.kind != hid_input || (it.flags & HIO_CONST) ||
        HID_GET_USAGE_PAGE(it.usage) != HUP_KEYBOARD || (int)it.report_ID != id)
      continue;
    if (it.flags & HIO_VARIABLE) {
      if (it.loc.size != 1 || sc->nvar >= MAXVARS)
        continue;
      sc->var[sc->nvar].loc = it.loc;
      sc->var[sc->nvar].key = (uint8_t)HID_GET_USAGE(it.usage);
      sc->nvar++;
    } else {
      if (it.loc.size != 8 || it.loc.pos % 8 != 0 || sc->nkeys != 0)
        continue;
      sc->keyloc = it.loc;
      sc->nkeys = it.loc.count > MAXKEYS ? MAXKEYS : (int)it.loc.count;
    }
  }
  hid_end_parse(h);
  if (sc->nkeys == 0 && sc->nvar == 0)
    return -1;

  lid = ukbd_led_id(d, len);
  hid_locate(d, len, HID_USAGE2(HUP_LED, HUL_NUM_LOCK), (uint8_t)lid,
             hid_output, &sc->numloc, 0);
  hid_locate(d, len, HID_USAGE2(HUP_LED, HUL_CAPS_LOCK), (uint8_t)lid,
             hid_output, &sc->capsloc, 0);
  hid_locate(d, len, HID_USAGE2(HUP_LED, HUL_SCROLL_LOCK), (uint8_t)lid,
             hid_output, &sc->scroloc, 0);
  sc->nleds =
      sc->numloc.size != 0 || sc->capsloc.size != 0 || sc->scroloc.size != 0;
  sc->led_id = sc->nleds != 0 ? lid : -1;

  sc->report_id = id != 0 ? id : -1;
  sc->report_len = hid_report_size(d, len, hid_input, (uint8_t)id);
  return 0;
}

static void ukbd_boot_layout(struct ukbd_softc *sc) {
  int i;

  sc->report_id = -1;
  sc->report_len = 8;
  for (i = 0; i < 8; i++) {
    sc->var[i].loc.pos = (uint32_t)i;
    sc->var[i].loc.size = 1;
    sc->var[i].loc.count = 1;
    sc->var[i].key = (uint8_t)(0xe0 + i);
  }
  sc->nvar = 8;
  sc->keyloc.pos = 16;
  sc->keyloc.size = 8;
  sc->keyloc.count = 6;
  sc->nkeys = 6;
  sc->numloc.pos = 0;
  sc->numloc.size = 1;
  sc->capsloc.pos = 1;
  sc->capsloc.size = 1;
  sc->scroloc.pos = 2;
  sc->scroloc.size = 1;
  sc->led_id = -1;
  sc->nleds = 1;
  sc->boot = 1;
}

// THE LAMPS

static void ukbd_set_leds(struct ukbd_softc *sc, uint leds) {
  uint8_t buf[2];
  int n = 1;

  if (sc->nleds == 0)
    return;
  buf[0] = 0;
  if ((leds & 1) && sc->numloc.size == 1)
    buf[0] |= (uint8_t)(1 << sc->numloc.pos);
  if ((leds & 2) && sc->capsloc.size == 1)
    buf[0] |= (uint8_t)(1 << sc->capsloc.pos);
  if ((leds & 4) && sc->scroloc.size == 1)
    buf[0] |= (uint8_t)(1 << sc->scroloc.pos);
  if (sc->led_id > 0) {
    buf[1] = buf[0];
    buf[0] = (uint8_t)sc->led_id;
    n = 2;
  }
  usbd_do_request(sc->dev, UT_WRITE_CLASS_INTERFACE, UR_SET_REPORT,
                  (uint16_t)((UHID_OUTPUT_REPORT << 8) |
                             (sc->led_id > 0 ? sc->led_id : 0)),
                  (uint16_t)sc->iface, (uint16_t)n, buf);
}

static void ukbd_repeat(void *arg);

static void ukbd_report(struct ukbd_softc *sc, const uint8_t *rep, int len) {
  uint8_t keys[MAXKEYS], vars[MAXVARS];
  int i, n;

  if (sc->report_id > 0) {
    if (len < 1 || rep[0] != sc->report_id)
      return;
    rep++;
    len--;
  }
  if (len <= 0)
    return;

  for (i = 0; i < sc->nvar; i++)
    vars[i] = hid_get_data(rep, len, &sc->var[i].loc) != 0;
  for (i = 0; i < sc->nvar; i++)
    if (vars[i] != sc->prevvar[i])
      emit(sc, lookup(sc->var[i].key), vars[i] == 0);
  memmove(sc->prevvar, vars, (uint)sc->nvar);

  n = sc->nkeys;
  if ((int)(sc->keyloc.pos / 8) + n > len)
    n = len - (int)(sc->keyloc.pos / 8);
  if (n <= 0)
    return;
  memmove(keys, rep + sc->keyloc.pos / 8, (uint)n);

  // all ones is rollover
  if (keys[0] == 0x01)
    return;

  for (i = 0; i < MAXKEYS; i++)
    if (sc->prev[i] > 3 && !in_list(keys, n, sc->prev[i]))
      emit(sc, lookup(sc->prev[i]), 1);

  for (i = 0; i < n; i++) {
    uint8_t usage = keys[i];

    if (usage <= 3 || in_list(sc->prev, MAXKEYS, usage))
      continue;
    emit(sc, lookup(usage), 0);
    sc->held = usage;
    if (!ukbd_polling)
      callout_reset(&sc->repeat, mstohz(sc->kbd.kb_delay1), ukbd_repeat, sc);
  }

  if (sc->held != 0 && !in_list(keys, n, sc->held)) {
    sc->held = 0;
    if (!ukbd_polling)
      callout_stop_async(&sc->repeat);
  }
  memset(sc->prev, 0, sizeof(sc->prev));
  for (i = 0; i < n && i < MAXKEYS; i++)
    sc->prev[i] = keys[i];
}

// REPORTS

static void ukbd_intr(struct usb_pipe *pipe, void *arg, int status, int len) {
  struct ukbd_softc *sc = arg;

  if (status != USBD_NORMAL && status != USBD_SHORT)
    return;
  if (ukbd_polling) {
    ukbd_report(sc, pipe->buf, len);
    return;
  }
  mtx_enter(&kbd_lock);
  ukbd_report(sc, pipe->buf, len);
  mtx_leave(&kbd_lock);
}

// polled: the other cpus are stopped
int ukbd_cncheckc(void) {
  struct ukbd_softc *sc;
  uint32 c;

  ukbd_polling = 1;
  usbd_poll_all();
  for (sc = ukbd_list; sc != 0; sc = sc->next)
    while (sc->phead != sc->ptail) {
      c = sc->polled[sc->phead];
      sc->phead = (sc->phead + 1) % 16;
      if (!(c & (FNU_RELKEY | FNU_ERRKEY)))
        return (int)c;
    }
  return -1;
}

static void ukbd_repeat(void *arg) {
  struct ukbd_softc *sc = arg;

  mtx_enter(&kbd_lock);
  if (sc->held != 0) {
    emit(sc, lookup(sc->held), 0);
    callout_reset(&sc->repeat, mstohz(sc->kbd.kb_delay2), ukbd_repeat, sc);
  }
  mtx_leave(&kbd_lock);
}

// ATTACH

static int ukbd_match(struct usb_attach_arg *uaa) {
  if (uaa->iface < 0 || uaa->class_code != UCLASS_HID)
    return USB_MATCH_NONE;
  if (uaa->subclass == USUBCLASS_BOOT && uaa->proto == UPROTO_BOOT_KEYBOARD)
    return USB_MATCH_IFACE_CLASS + 1;
  if (uaa->subclass != 0 || uaa->proto != 0)
    return USB_MATCH_NONE;
  {
    uint8_t *desc = (uint8_t *)page_alloc();
    int len, ok;

    if (desc == 0)
      return USB_MATCH_NONE;
    len = usbd_hid_report_desc(uaa->dev, uaa->iface, desc, USB_RDESC_MAX);
    ok = len > 0 && ukbd_is_keyboard(desc, len);
    page_free((char *)desc);
    return ok ? USB_MATCH_IFACE_CLASS : USB_MATCH_NONE;
  }
}

static void ukbd_free(struct ukbd_softc *sc) {
  free_unr(&ukbd_units, sc->unit);
  free(sc, M_DEVBUF);
}

static int ukbd_attach(struct usb_attach_arg *uaa) {
  struct usb_device *dev = uaa->dev;
  struct ukbd_softc *sc;
  const struct usb_endpoint_descriptor *ed;
  uint8_t *rdesc;
  int rlen = 0, ifidx = 0, i;
  int bootiface =
      uaa->subclass == USUBCLASS_BOOT && uaa->proto == UPROTO_BOOT_KEYBOARD;

  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->unit = alloc_unr(&ukbd_units);
  snprintf(sc->name, sizeof(sc->name), "ukbd%d", sc->unit);
  callout_init(&sc->repeat);
  sc->dev = dev;
  sc->iface = uaa->iface;

  for (i = 0; i < USB_MAX_INTERFACES; i++) {
    const struct usb_interface_descriptor *id = usbd_find_iface(dev, i);

    if (id == 0)
      break;
    if (id->bInterfaceNumber == uaa->iface) {
      ifidx = i;
      break;
    }
  }

  rdesc = (uint8_t *)page_alloc();
  if (rdesc == 0) {
    ukbd_free(sc);
    return -1;
  }
  rlen = usbd_hid_report_desc(dev, uaa->iface, rdesc, USB_RDESC_MAX);
  if (rlen > 0 && ukbd_parse(sc, rdesc, rlen) == 0)
    usbd_do_request(dev, UT_WRITE_CLASS_INTERFACE, UR_SET_PROTOCOL,
                    UHID_REPORT_PROTOCOL, (uint16_t)uaa->iface, 0, 0);
  else
    rlen = 0;
  page_free((char *)rdesc);

  if (rlen == 0) {
    if (!bootiface) {
      ukbd_free(sc);
      return -1;
    }
    ukbd_boot_layout(sc);
    usbd_do_request(dev, UT_WRITE_CLASS_INTERFACE, UR_SET_PROTOCOL,
                    UHID_BOOT_PROTOCOL, (uint16_t)uaa->iface, 0, 0);
  }

  usbd_do_request(dev, UT_WRITE_CLASS_INTERFACE, UR_SET_IDLE, 0,
                  (uint16_t)uaa->iface, 0, 0);

  ed = usbd_find_endpoint(dev, ifidx, 0);
  if (ed == 0 || !UE_GET_DIR(ed->bEndpointAddress) ||
      UE_GET_XFERTYPE(ed->bmAttributes) != UE_INTERRUPT) {
    printf("%s: no interrupt in endpoint\n", sc->name);
    ukbd_free(sc);
    return -1;
  }
  if (usbd_open_pipe(dev, ed, &sc->intr) < 0) {
    printf("%s: cannot open the interrupt endpoint\n", sc->name);
    ukbd_free(sc);
    return -1;
  }

  i = sc->report_len + (sc->report_id > 0 ? 1 : 0);
  if (i < 8)
    i = 8;
  kbd_init_struct(&sc->kbd, "ukbd", FNU_KB_101, sc->unit, 0);
  kbd_default_maps(&sc->keymap, &sc->accmap, sc->fkeytab);
  kbd_set_maps(&sc->kbd, &sc->keymap, &sc->accmap, sc->fkeytab, FNU_KB_NFKEYS);
  sc->kbd.kb_data = sc;
  sc->kbd.kb_sw = &ukbdsw;
  KBD_FOUND_DEVICE(&sc->kbd);
  KBD_PROBE_DONE(&sc->kbd);
  KBD_INIT_DONE(&sc->kbd);
  sc->x.mode = FNU_KB_XLATE;
  sc->leds = 0;
  ukbd_set_leds(sc, 0);
  kbd_register(&sc->kbd);
  KBD_CONFIG_DONE(&sc->kbd);
  sc->intr.polled = 1;
  if (usbd_start_pipe(&sc->intr, i, ukbd_intr, sc) < 0) {
    kbd_unregister(&sc->kbd);
    usbd_close_pipe(&sc->intr);
    ukbd_free(sc);
    return -1;
  }
  uaa->softc = sc;
  mtx_enter(&kbd_lock);
  sc->next = ukbd_list;
  ukbd_list = sc;
  mtx_leave(&kbd_lock);
  hotplug_attach(FNU_DEVCLASS_KEYBOARD, sc->name, FNU_HOTPLUG_REMOVABLE, 0);
  printf("%s: %s speed keyboard on %s, %s protocol, %d keys, %d bit keys\n",
          sc->name, usbd_speed_name(dev->speed),
          dev->parent && dev->parent->depth ? "a hub" : "a root port",
          sc->boot ? "boot" : "report", sc->nkeys, sc->nvar);
  return 0;
}

static void ukbd_detach(struct usb_device *dev, void *softc) {
  struct ukbd_softc *sc = softc;

  (void)dev;
  hotplug_detach(FNU_DEVCLASS_KEYBOARD, sc->name, FNU_HOTPLUG_REMOVABLE, 0);
  usbd_close_pipe(&sc->intr);
  mtx_enter(&kbd_lock);
  sc->held = 0;
  {
    struct ukbd_softc **pp;

    for (pp = &ukbd_list; *pp != 0; pp = &(*pp)->next)
      if (*pp == sc) {
        *pp = sc->next;
        break;
      }
  }
  mtx_leave(&kbd_lock);
  callout_stop(&sc->repeat);
  kbd_unregister(&sc->kbd);
  ukbd_free(sc);
}

static void ukbd_service(struct usb_device *dev, void *softc) {
  struct ukbd_softc *sc = softc;
  uint leds;
  int dirty;

  (void)dev;
  mtx_enter(&kbd_lock);
  dirty = sc->leds_dirty;
  sc->leds_dirty = 0;
  leds = sc->leds;
  mtx_leave(&kbd_lock);
  if (dirty)
    ukbd_set_leds(sc, leds);
}

static const struct usb_driver ukbd_driver = {
    .name = "ukbd",
    .match = ukbd_match,
    .attach = ukbd_attach,
    .detach = ukbd_detach,
    .service = ukbd_service,
};

void ukbd_init(void) {
  init_unrhdr(&ukbd_units, "ukbd");
  usbd_register_driver(&ukbd_driver);
}
