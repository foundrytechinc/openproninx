// USB QUIRKS

#include "defs.h"
#include "usbdi.h"

#define TUNABLES 16

struct usb_quirk {
  uint16_t vid, pid;
  uint16_t lo, hi; // bcdDevice
  uint32_t quirks;
};

// the rows for what umass sends
static const struct usb_quirk table[] = {
    {0x058f, 0x9361, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // alcor umcr_9361
    {0x1005, 0xb113, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE}, // apacer ht202
    {0x14af, 0xaf01, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // atp eusb
    {0x07f7, 0x011e, 0x0000, 0xffff, UQ_NO_START_STOP}, // century ex35quat
    {0x04b4, 0x6830, 0x0000, 0xffff, UQ_NO_GETMAXLUN | UQ_NO_SYNC_CACHE}, // cypress xx6830xx
    {0x13fe, 0x1e20, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // emtec daneelec4gb
    {0x0c0b, 0xa109, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // dmi cfsm_rw
    {0x13fe, 0x2240, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // emtec ruf2ps
    {0x04b8, 0x0602, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // epson stylus_895
    {0x090c, 0x6200, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // feiya elango
    {0x04cb, 0x0100, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // fujiphoto mass0100
    {0x05e3, 0x0702, 0x0000, 0xffff, UQ_NO_START_STOP | UQ_NO_SYNC_CACHE}, // genesys gl641usb2ide
    {0x05e3, 0x0701, 0x0000, 0xffff, UQ_NO_START_STOP}, // genesys gl641usb2ide_2
    {0x05e3, 0x0700, 0x0000, 0xffff, UQ_NO_START_STOP}, // genesys gl641usb
    {0x03f0, 0x0207, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_START_STOP}, // hp cdw8200
    {0x12d1, 0x1f01, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // huawei e3372_init
    {0x05ab, 0x081a, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_START_STOP}, // insystem usbcable
    {0x059b, 0x0001, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY}, // iomega zip100
    {0x152d, 0x3569, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // jmicron jms566
    {0x152d, 0x0567, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // jmicron jms567
    {0x152d, 0x2338, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // jmicron jm20337
    {0x0951, 0x1666, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // kingston datatraveler3_0
    {0x0492, 0x0140, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // meizu m6_sl
    {0x0411, 0x001c, 0x0000, 0xffff, UQ_NO_START_STOP}, // melco dubpxxg
    {0x07af, 0x0006, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_START_STOP}, // microtech dpcm
    {0x0634, 0x0655, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // micron realssd
    {0x22b8, 0x4810, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // motorola2 e398
    {0x1cae, 0x0000, 0x0000, 0xffff, UQ_NO_SYNC_CACHE | UQ_VENDOR_ONLY}, // mpman, any product
    {0x08ec, 0x0010, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // msystems diskonkey
    {0x04cf, 0x8818, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // myson heden
    {0x0525, 0xa4a5, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // netchip pocketbook
    {0x07b4, 0x0105, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // olympus c700
    {0x07b4, 0x0102, 0x0000, 0xffff, UQ_NO_GETMAXLUN | UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE}, // olympus e_1
    {0x07b4, 0x0118, 0x0000, 0xffff, UQ_NO_GETMAXLUN | UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE | UQ_NO_START_STOP}, // olympus e_330
    {0x07b4, 0x012c, 0x0000, 0xffff, UQ_NO_GETMAXLUN | UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE | UQ_NO_START_STOP}, // olympus e_pm1
    {0x07b4, 0x012e, 0x0000, 0xffff, UQ_NO_GETMAXLUN | UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE | UQ_NO_START_STOP}, // olympus e_m1
    {0x07c4, 0x0400, 0x0000, 0xffff, UQ_NO_GETMAXLUN | UQ_NO_SYNC_CACHE}, // onspec sds_hotfind_d
    {0x07c4, 0xa400, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // onspec ucf100
    {0x55aa, 0xa103, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // onspec2 imagemate_sddr55
    {0x04da, 0x0d01, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // panasonic kxl840an
    {0x093b, 0x0011, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY}, // plextor 40_12_40u
    {0x154b, 0x0010, 0x0000, 0xffff, UQ_NO_START_STOP}, // pny attache2
    {0x067b, 0x2506, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // prolific pl2506
    {0x19d2, 0x1420, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // qualcomminc zte_mf730m
    {0x0781, 0x0200, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // sandisk sddr09
    {0x0781, 0x7100, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // sandisk sdcz2_128
    {0x0781, 0x5581, 0x0000, 0xffff, UQ_NO_SYNC_CACHE | UQ_NO_TEST_UNIT_READY}, // sandisk sdcz48_32
    {0x0781, 0xb6ba, 0x0000, 0xffff, UQ_NO_SYNC_CACHE | UQ_NO_GETMAXLUN}, // sandisk imagemate_sddr289
    {0x04e6, 0x0001, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_START_STOP}, // shuttle eusb
    {0x04e6, 0x0003, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // shuttle sddr09
    {0x054c, 0x0025, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // sony ms_nw_ms7
    {0x054c, 0x002d, 0x0000, 0xffff, UQ_NO_GETMAXLUN}, // sony msacus1
    {0x14cd, 0x6600, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // supertop ide
    {0x14cd, 0x121c, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE}, // supertop flashdrive
    {0x071b, 0x3203, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY | UQ_NO_SYNC_CACHE}, // teclast tlc300
    {0x0930, 0x6545, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // toshiba transmemory
    {0x040d, 0x6204, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // via usb2idebridge
    {0x2109, 0x0700, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // vialabs usb30satabridge
    {0x1058, 0x0200, 0x0000, 0xffff, UQ_NO_START_STOP}, // western combo
    {0x1058, 0x0400, 0x0000, 0xffff, UQ_NO_START_STOP}, // western exthdd
    {0x1058, 0x0901, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mybook
    {0x1058, 0x0746, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_01
    {0x1058, 0x0748, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_02
    {0x1058, 0x074a, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_03
    {0x1058, 0x074c, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_04
    {0x1058, 0x074e, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_05
    {0x1058, 0x07a6, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_06
    {0x1058, 0x07a8, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_07
    {0x1058, 0x07aa, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_08
    {0x1058, 0x07ac, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_09
    {0x1058, 0x07ae, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_10
    {0x1058, 0x0741, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassport_11
    {0x1058, 0x070a, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_00
    {0x1058, 0x071a, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_01
    {0x1058, 0x0730, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_02
    {0x1058, 0x0732, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_03
    {0x1058, 0x0740, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_04
    {0x1058, 0x0742, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_05
    {0x1058, 0x0750, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_06
    {0x1058, 0x0752, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_07
    {0x1058, 0x07a0, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_08
    {0x1058, 0x07a2, 0x0000, 0xffff, UQ_NO_SYNC_CACHE}, // western mypassportes_09
    {0x1058, 0x0743, 0x0000, 0xffff, UQ_NO_TEST_UNIT_READY}, // western mypassportul_00
    {0x094f, 0x05fc, 0x0000, 0xffff, UQ_NO_START_STOP}, // yano fw800hd
    {0x057b, 0x0000, 0x0000, 0x007f, UQ_NO_TEST_UNIT_READY}, // yedata flashbusteru
    {0x057b, 0x0000, 0x0080, 0x0080, UQ_NO_TEST_UNIT_READY}, // yedata flashbusteru
};

static const struct {
  const char *name;
  uint32_t quirk;
} names[] = {
    {"UQ_MSC_NO_SYNC_CACHE", UQ_NO_SYNC_CACHE},
    {"UQ_MSC_NO_TEST_UNIT_READY", UQ_NO_TEST_UNIT_READY},
    {"UQ_MSC_NO_GETMAXLUN", UQ_NO_GETMAXLUN},
    {"UQ_MSC_NO_START_STOP", UQ_NO_START_STOP},
    {"UQ_MATCH_VENDOR_ONLY", UQ_VENDOR_ONLY},
};

static struct usb_quirk added[TUNABLES];
static int nadded, loaded;

static const char *word(const char *p, char *out, int size) {
  int n = 0;

  while (*p == ' ' || *p == '\t')
    p++;
  while (*p != 0 && *p != ' ' && *p != '\t') {
    if (n < size - 1)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  return p;
}

static int number(const char *s, uint16_t *v) {
  uint32_t n = 0;
  int base = 10, d;

  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
  }
  if (*s == 0)
    return -1;
  for (; *s != 0; s++) {
    if (*s >= '0' && *s <= '9')
      d = *s - '0';
    else if (base == 16 && *s >= 'a' && *s <= 'f')
      d = *s - 'a' + 10;
    else if (base == 16 && *s >= 'A' && *s <= 'F')
      d = *s - 'A' + 10;
    else
      return -1;
    if ((n = n * (uint32_t)base + (uint32_t)d) > 0xffff)
      return -1;
  }
  *v = (uint16_t)n;
  return 0;
}

// hw.usb.quirk.N="VID PID LO_REV HI_REV QUIRK [QUIRK ...]"
static void load(void) {
  char key[24], w[32];
  const char *v, *p;
  struct usb_quirk *q;
  uint32_t i;
  int n;

  loaded = 1;
  for (n = 0; n < TUNABLES; n++) {
    snprintf(key, sizeof(key), "hw.usb.quirk.%d", n);
    if ((v = kgetenv(key)) == 0)
      continue;
    q = &added[nadded];
    memset(q, 0, sizeof(*q));
    p = word(v, w, sizeof(w));
    if (number(w, &q->vid) < 0)
      goto bad;
    p = word(p, w, sizeof(w));
    if (number(w, &q->pid) < 0)
      goto bad;
    p = word(p, w, sizeof(w));
    if (number(w, &q->lo) < 0)
      goto bad;
    p = word(p, w, sizeof(w));
    if (number(w, &q->hi) < 0)
      goto bad;
    for (p = word(p, w, sizeof(w)); w[0] != 0; p = word(p, w, sizeof(w))) {
      for (i = 0; i < nitems(names) && strcmp(names[i].name, w) != 0; i++)
        ;
      if (i == nitems(names)) {
        printf("usb: %s: %s is not a quirk this system knows\n", key, w);
        continue;
      }
      q->quirks |= names[i].quirk;
    }
    if (q->quirks == 0)
      goto bad;
    nadded++;
    continue;
  bad:
    printf("usb: %s=\"%s\" is not VID PID LO_REV HI_REV QUIRK\n", key, v);
  }
}

static int matches(const struct usb_quirk *q, const struct usb_device *dev) {
  const struct usb_device_descriptor *d = &dev->ddesc;

  if (q->vid != d->idVendor)
    return 0;
  if (!(q->quirks & UQ_VENDOR_ONLY) && q->pid != d->idProduct)
    return 0;
  return d->bcdDevice >= q->lo && d->bcdDevice <= q->hi;
}

uint32_t usbd_quirks(const struct usb_device *dev) {
  uint32_t i, quirks = 0;

  if (!loaded)
    load();
  for (i = 0; i < nitems(table); i++)
    if (matches(&table[i], dev))
      quirks |= table[i].quirks;
  for (i = 0; i < (uint32_t)nadded; i++)
    if (matches(&added[i], dev))
      quirks |= added[i].quirks;
  return quirks & ~UQ_VENDOR_ONLY;
}
