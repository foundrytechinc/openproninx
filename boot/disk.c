#include "boot.h"

#define BOUNCE_SECTORS 127

// room for a whole transfer inside one 64 KB page
static uint8 bounce_room[2 * BOUNCE_SECTORS * SECTOR] __aligned(16);
static uint8 *bounce;

static uint32 bounce_run(uint32 count) {
  if (bounce == 0) {
    uint32 off = vtophys(bounce_room) & 0xffffu;

    bounce = bounce_room;
    if (0x10000u - off < BOUNCE_SECTORS * SECTOR)
      bounce += 0x10000u - off;
  }
  return count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
}

// three tries, the drive reset before each
static int read_raw(uint64 lba, uint32 count, uint32 linear) {
  struct bios_dap dap;
  int try;

  for (try = 0; try < 3; try++) {
    if (try > 0)
      bios_disk_reset(boot_drive);
    dap.size = sizeof(dap);
    dap.zero = 0;
    dap.count = (uint16)count;
    dap.off = (uint16)(linear & 0xf);
    dap.seg = (uint16)(linear >> 4);
    dap.lba = lba;
    if (bios_disk_read(boot_drive, &dap) == 0)
      return 0;
  }
  return -1;
}

int disk_read(uint64 lba, uint32 count, void *dst) {
  uint8 *out = dst;

  while (count > 0) {
    uint32 n = bounce_run(count);

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
    uint32 n = bounce_run(count);

    if (read_raw(lba, n, vtophys(bounce)) < 0)
      return -1;
    flat_write(linear, bounce, n * SECTOR);
    linear += n * SECTOR;
    lba += n;
    count -= n;
  }
  return 0;
}

// int 13h ah=48h
struct drive_params {
  uint16 size;
  uint16 flags;
  uint32 cylinders;
  uint32 heads;
  uint32 sectors_per_track;
  uint64 sectors;
  uint16 bytes_per_sector;
} __packed;

static int bootdisk_read(void *ctx, uint64 lba, uint32 count, void *dst) {
  (void)ctx;
  return disk_read(lba, count, dst);
}

void bootdisk_open(struct bootdisk *d) {
  static struct drive_params p;

  d->read = bootdisk_read;
  d->ctx = 0;
  d->shift = 9;
  d->blocks = 0;
  memset(&p, 0, sizeof(p));
  p.size = sizeof(p);
  if (bios_disk_params(boot_drive, &p) == 0 && p.bytes_per_sector == SECTOR)
    d->blocks = p.sectors;
}
