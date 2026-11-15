#include "boot/bootdisk.h"

#define GPT_SIG "EFI PART"
#define HDR_MIN 92
#define ENT_MIN 128
#define ENT_MAX 1024

#define H_SIZE 12
#define H_CRC 16
#define H_SELF 24
#define H_ALT 32
#define H_TABLE 72
#define H_ENTRIES 80
#define H_ENTSZ 84
#define H_TABCRC 88
#define E_TYPE 0
#define E_GUID 16
#define E_START 32
#define E_END 40
#define E_NAME 56
#define E_NAMELEN 36

static uint8 blk[BOOTDISK_MAXBLK];

static uint32 rd32(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}
static uint64 rd64(const uint8 *p) {
  return (uint64)rd32(p) | ((uint64)rd32(p + 4) << 32);
}

static int same(const uint8 *a, const uint8 *b, uint32 n) {
  while (n-- > 0)
    if (*a++ != *b++)
      return 0;
  return 1;
}

static void copy(uint8 *d, const uint8 *s, uint32 n) {
  while (n-- > 0)
    *d++ = *s++;
}

static uint32 crc32(uint32 crc, const uint8 *p, uint32 n) {
  uint32 k;

  crc = ~crc;
  while (n-- > 0) {
    crc ^= *p++;
    for (k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

struct hdr {
  uint64 alt, table;
  uint32 entries, entsz, tabcrc;
};

static int header(const struct bootdisk *d, uint64 lba, struct hdr *h) {
  uint32 bsize = 1u << d->shift, size, crc;
  uint8 zero[4] = {0, 0, 0, 0};

  if (d->read(d->ctx, lba, 1, blk) < 0 || !same(blk, (const uint8 *)GPT_SIG, 8))
    return -1;
  size = rd32(blk + H_SIZE);
  if (size < HDR_MIN || size > bsize)
    return -1;
  crc = crc32(0, blk, H_CRC);
  crc = crc32(crc, zero, 4);
  crc = crc32(crc, blk + H_CRC + 4, size - H_CRC - 4);
  if (crc != rd32(blk + H_CRC) || rd64(blk + H_SELF) != lba)
    return -1;
  h->alt = rd64(blk + H_ALT);
  h->table = rd64(blk + H_TABLE);
  h->entries = rd32(blk + H_ENTRIES);
  h->entsz = rd32(blk + H_ENTSZ);
  h->tabcrc = rd32(blk + H_TABCRC);
  if (h->entsz < ENT_MIN || h->entsz > bsize || bsize % h->entsz != 0 ||
      h->entries == 0 || h->entries > ENT_MAX)
    return -1;
  return 0;
}

static int named(const uint8 *raw, const char *name) {
  uint32 i;

  for (i = 0; i < E_NAMELEN; i++) {
    uint8 lo = raw[i * 2], hi = raw[i * 2 + 1];

    if (hi != 0 || lo != (uint8)name[i])
      return 0;
    if (lo == 0)
      return 1;
  }
  return name[i] == 0;
}

static int table(const struct bootdisk *d, const struct hdr *h,
                 const uint8 type[16], const char *name, struct gpt_part *out) {
  uint32 bsize = 1u << d->shift, per = bsize / h->entsz, left = h->entries;
  uint32 crc = 0, i;
  uint64 lba = h->table;
  int found = 0;

  while (left > 0) {
    if (d->read(d->ctx, lba++, 1, blk) < 0)
      return -1;
    for (i = 0; i < per && left > 0; i++, left--) {
      const uint8 *e = blk + i * h->entsz;

      crc = crc32(crc, e, h->entsz);
      if (found || !same(e + E_TYPE, type, 16) ||
          (name != 0 && !named(e + E_NAME, name)) ||
          rd64(e + E_END) < rd64(e + E_START))
        continue;
      out->start = rd64(e + E_START);
      out->count = rd64(e + E_END) - out->start + 1;
      copy(out->guid, e + E_GUID, 16);
      found = 1;
    }
  }
  if (crc != h->tabcrc)
    return -1;
  return found ? 0 : 1;
}

int gpt_scan(const struct bootdisk *d, const uint8 type[16], const char *name,
             struct gpt_part *out) {
  struct hdr h;
  uint64 alt = d->blocks > 1 ? d->blocks - 1 : 0;
  int r;

  if (header(d, 1, &h) == 0) {
    if ((r = table(d, &h, type, name, out)) >= 0)
      return r;
    alt = h.alt;
  }
  if (alt > 1 && header(d, alt, &h) == 0)
    return table(d, &h, type, name, out);
  return -1;
}
