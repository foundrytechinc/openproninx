// SMBIOS

#include "defs.h"
#include "inc/bootinfo.h"
#include "inc/abi.h"
#include "param.h"

#define TYPE_BIOS 0
#define TYPE_SYSTEM 1
#define TYPE_BASEBOARD 2
#define TYPE_EOT 127

#define SMBIOS_UUID_REP "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"

struct entry {
  char anchor[4];
  uint8_t checksum;
  uint8_t length;
  uint8_t major;
  uint8_t minor;
  uint16_t maxsize;
  uint8_t revision;
  uint8_t formatted[5];
  char dmi[5]; // "_DMI_"
  uint8_t dmi_checksum;
  uint16_t size; // of the structures
  uint32_t addr;
  uint16_t count;
  uint8_t bcd;
} __packed;

// SMBIOS 3
struct entry3 {
  char anchor[5]; // "_SM3_"
  uint8_t checksum;
  uint8_t length;
  uint8_t major;
  uint8_t minor;
  uint8_t docrev;
  uint8_t revision;
  uint8_t reserved;
  uint32_t maxsize;
  uint64_t addr;
} __packed;

struct header {
  uint8_t type;
  uint8_t length;
  uint16_t handle;
} __packed;

static uint8_t *table;
static uint tablelen;
static int count, major, minor;

char hw_vendor[64], hw_product[64], hw_version[64], hw_serial[64], hw_uuid[40];
static char bios_vendor[64], bios_version[64], bios_date[64];
static char board_vendor[64], board_product[64], board_serial[64];

static const char *const uninformative[] = {"System", "Not ", "To be", "SYS-"};

static int sum_ok(const uint8_t *p, int len) {
  uint8_t sum = 0;

  while (len-- > 0)
    sum += *p++;
  return sum == 0;
}

static struct header *find(int type, int nth) {
  uint8_t *p = table, *end = table + tablelen;
  int i;

  for (i = 0; i < count && p + sizeof(struct header) <= end; i++) {
    struct header *h = (struct header *)p;

    if (h->type == type && nth-- == 0)
      return h;
    if (h->type == TYPE_EOT)
      break;
    p += h->length;
    while (p + 1 < end && !(p[0] == 0 && p[1] == 0))
      p++;
    p += 2;
  }
  return 0;
}

static int string(struct header *h, int idx, char *out, size_t size) {
  const uint8_t *p = (const uint8_t *)h + h->length, *end = table + tablelen;
  char *s, *e;
  uint i;

  out[0] = 0;
  if (idx == 0)
    return 0;
  for (; idx > 1 && p < end && *p; idx--)
    while (p < end && *p++)
      ;
  if (p >= end || *p == 0)
    return 0;
  strlcpy(out, (char *)p, (int)size);
  for (i = 0; i < sizeof(uninformative) / sizeof(uninformative[0]); i++)
    if (strncmp(out, uninformative[i], strlen(uninformative[i])) == 0) {
      out[0] = 0;
      return 0;
    }
  for (s = out; *s == ' '; s++)
    ;
  if (*s == 0) {
    out[0] = 0;
    return 0;
  }
  for (e = s + strlen(s) - 1; e > s && *e == ' '; e--)
    ;
  memmove(out, s, (size_t)(e - s + 1));
  out[e - s + 1] = 0;
  return 1;
}

const char *hw_vendor_name(void) { return hw_vendor[0] ? hw_vendor : 0; }

static int entry_point(uint64_t pa) {
  struct entry *e;
  struct entry3 *e3;

  if ((e = ioremap(pa, sizeof(*e))) == 0)
    return -1;
  if (memcmp(e->anchor, "_SM_", 4) == 0) {
    if (e->length < sizeof(*e) || !sum_ok((uint8_t *)e, e->length) ||
        memcmp(e->dmi, "_DMI_", 5) != 0 || e->major < 2 ||
        (table = ioremap(e->addr, e->size)) == 0)
      return -1;
    tablelen = e->size;
    count = e->count;
    major = e->major;
    minor = e->minor;
    return 0;
  }
  e3 = (struct entry3 *)e;
  if (memcmp(e3->anchor, "_SM3_", 5) != 0 || e3->length < sizeof(*e3) ||
      !sum_ok((uint8_t *)e3, e3->length) || e3->maxsize == 0 ||
      (table = ioremap(e3->addr, e3->maxsize)) == 0)
    return -1;
  tablelen = e3->maxsize;
  count = 0x7fffffff;
  major = e3->major;
  minor = e3->minor;
  return 0;
}

void smbios_init(void) {
  uint64_t pa = bootinfo()->smbios;
  struct header *h;
  int i;

  if (pa == 0 || entry_point(pa) != 0)
    return;

  if ((h = find(TYPE_BIOS, 0)) != 0 && h->length >= 9) {
    uint8_t *f = (uint8_t *)h;

    string(h, f[4], bios_vendor, sizeof(bios_vendor));
    string(h, f[5], bios_version, sizeof(bios_version));
    string(h, f[8], bios_date, sizeof(bios_date));
    if (bios_vendor[0] || bios_version[0])
      printf("SMBIOS: BIOS vendor %s version \"%s\" date %s\n", bios_vendor,
              bios_version, bios_date);
  }
  if ((h = find(TYPE_BASEBOARD, 0)) != 0 && h->length >= 8) {
    uint8_t *f = (uint8_t *)h;

    string(h, f[4], board_vendor, sizeof(board_vendor));
    string(h, f[5], board_product, sizeof(board_product));
    string(h, f[7], board_serial, sizeof(board_serial));
  }
  if ((h = find(TYPE_SYSTEM, 0)) == 0 || h->length < 8)
    return;
  {
    uint8_t *f = (uint8_t *)h;

    if (!string(h, f[4], hw_vendor, sizeof(hw_vendor)))
      strlcpy(hw_vendor, board_vendor, sizeof(hw_vendor));
    if (!string(h, f[5], hw_product, sizeof(hw_product)))
      strlcpy(hw_product, board_product, sizeof(hw_product));
    string(h, f[6], hw_version, sizeof(hw_version));
    string(h, f[7], hw_serial, sizeof(hw_serial));
    if (h->length >= 24 && (major > 2 || (major == 2 && minor >= 1))) {
      int allset = 1, none = 1;

      for (i = 0; i < 16; i++) {
        if (f[8 + i] != 0xff)
          allset = 0;
        if (f[8 + i] != 0)
          none = 0;
      }
      if (allset)
        strlcpy(hw_uuid, "Not Set", sizeof(hw_uuid));
      else if (!none)
        snprintf(hw_uuid, sizeof(hw_uuid), SMBIOS_UUID_REP, f[8], f[9], f[10],
                 f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19],
                 f[20], f[21], f[22], f[23]);
    }
  }
  if (hw_vendor[0] && hw_product[0])
    printf("SMBIOS: %s %s\n", hw_vendor, hw_product);
}

void smbios_sysctl(void) {
  if (hw_vendor[0])
    sysctl_add_string("hw.vendor", hw_vendor, sizeof(hw_vendor), 0);
  if (hw_product[0])
    sysctl_add_string("hw.product", hw_product, sizeof(hw_product), 0);
  if (hw_version[0])
    sysctl_add_string("hw.version", hw_version, sizeof(hw_version), 0);
  if (hw_serial[0])
    sysctl_add_string("hw.serialno", hw_serial, sizeof(hw_serial),
                      FNU_CTLF_PRIVATE);
  if (hw_uuid[0])
    sysctl_add_string("hw.uuid", hw_uuid, sizeof(hw_uuid), FNU_CTLF_PRIVATE);
}
