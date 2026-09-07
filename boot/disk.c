#include "boot.h"

#define BOUNCE_SECTORS 64

static uint8 bounce[BOUNCE_SECTORS * SECTOR] __attribute__((aligned(16)));

struct gpt_hdr {
  uint8 sig[8];
  uint32 revision;
  uint32 hdr_size;
  uint32 crc_self;
  uint32 rsvd;
  uint64 lba_self;
  uint64 lba_alt;
  uint64 lba_start;
  uint64 lba_end;
  uint8 guid[16];
  uint64 lba_table;
  uint32 entries;
  uint32 entsz;
  uint32 crc_table;
};

struct gpt_ent {
  uint8 type[16];
  uint8 guid[16];
  uint64 lba_start;
  uint64 lba_end;
  uint64 attr;
  uint16 name[36];
};

_Static_assert(sizeof(struct gpt_hdr) == 92, "gpt_hdr");
_Static_assert(sizeof(struct gpt_ent) == 128, "gpt_ent");

static int read_raw(uint64 lba, uint32 count, uint32 linear) {
  struct bios_dap dap;

  dap.size = sizeof(dap);
  dap.zero = 0;
  dap.count = (uint16)count;
  dap.off = (uint16)(linear & 0xf);
  dap.seg = (uint16)(linear >> 4);
  dap.lba = lba;
  return bios_disk_read(boot_drive, &dap) == 0 ? 0 : -1;
}

int disk_read(uint64 lba, uint32 count, void *dst) {
  uint8 *out = dst;

  while (count > 0) {
    uint32 n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
    if (read_raw(lba, n, vtophys(bounce)) < 0)
      return -1;
    memcpy(out, bounce, n * SECTOR);
    out += n * SECTOR;
    lba += n;
    count -= n;
  }
  return 0;
}

int disk_read_far(uint64 lba, uint32 count, uint32 linear) {
  while (count > 0) {
    uint32 n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
    if (read_raw(lba, n, vtophys(bounce)) < 0)
      return -1;
    flat_write(linear, bounce, n * SECTOR);
    linear += n * SECTOR;
    lba += n;
    count -= n;
  }
  return 0;
}

int gpt_find(const uint8 type[16], uint64 *start, uint64 *count) {
  static uint8 hdrbuf[SECTOR];
  static uint8 entbuf[SECTOR];
  struct gpt_hdr *h = (struct gpt_hdr *)hdrbuf;
  uint32 left, per, i;
  uint64 lba;

  if (disk_read(1, 1, hdrbuf) < 0)
    return -1;
  if (memcmp(h->sig, "EFI PART", 8) != 0)
    return -1;
  if (h->entsz < sizeof(struct gpt_ent) || h->entsz > SECTOR)
    return -1;
  if (h->entries == 0 || h->entries > 512)
    return -1;

  per = SECTOR / h->entsz;
  left = h->entries;
  lba = h->lba_table;

  while (left > 0) {
    if (disk_read(lba, 1, entbuf) < 0)
      return -1;
    for (i = 0; i < per && left > 0; i++, left--) {
      struct gpt_ent *e = (struct gpt_ent *)(entbuf + i * h->entsz);
      if (memcmp(e->type, type, 16) != 0)
        continue;
      *start = e->lba_start;
      *count = e->lba_end - e->lba_start + 1;
      return 0;
    }
    lba++;
  }
  return -1;
}
