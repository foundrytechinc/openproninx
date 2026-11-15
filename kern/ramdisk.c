#include "ramdisk.h"
#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "fs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"

static char *ramdisk_data;
static uint64_t ramdisk_sectors;
static int ramdisk_major;

static uint64_t ramdisk_unit_size(uint unit) {
  return unit == 0 ? ramdisk_sectors : 0;
}

static uint ramdisk_maxphys(uint unit) {
  (void)unit;
  return MAXPHYS;
}

static int ramdisk_memory(uint unit) {
  (void)unit;
  return 1;
}

static int ramdisk_ident(uint unit __unused, struct fnu_diskinfo *out) {
  strlcpy(out->model, "ramdisk", FNU_DISK_MODEL_MAX);
  out->kind = FNU_DISK_RAMDISK;
  return 0;
}

static const struct blockdrv ramdisk_blockdrv = {
    .name = "ramdisk",
    .prefix = "rd",
    .units = 1,
    .strategy = ramdiskrw,
    .maxphys = ramdisk_maxphys,
    .size = ramdisk_unit_size,
    .memory = ramdisk_memory,
    .ident = ramdisk_ident,
};

void ramdisk_init(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint i;

  if (bi == 0)
    return;
  for (i = 0; i < bi->modules; i++) {
    if (strncmp(bi->module[i].name, "ramdisk", FNU_MODULE_NAME) != 0)
      continue;
    if (bi->module[i].size < FNU_BLOCKDEV_SECTOR_SIZE)
      break;
    ramdisk_data = (char *)P2V((uintptr_t)bi->module[i].base);
    ramdisk_sectors = bi->module[i].size / FNU_BLOCKDEV_SECTOR_SIZE;
    ramdisk_major = blockdev_register(&ramdisk_blockdrv);
    if (ramdisk_major != 0)
      rootdev = makedev(ramdisk_major, 0);
    printf("RAMDISK: rootfs from loader (%u sectors, %u KB), device %d,0\n",
            (uint)ramdisk_sectors, (uint)(bi->module[i].size / 1024),
            ramdisk_major);
    return;
  }
}

uint64_t ramdisk_size(void) { return ramdisk_sectors; }

int ramdisk_available(void) {
  return (ramdisk_sectors > 0 && ramdisk_data != 0);
}

void ramdiskrw(struct buf *b) {
  if (!holdingsleep(&b->lock))
    panic("ramdiskrw: buf not locked");
  if ((b->flags & (B_VALID | B_DIRTY)) == B_VALID)
    panic("ramdiskrw: nothing to do");

  uint bytes = bufcount(b);
  uint sectors = bytes / FNU_BLOCKDEV_SECTOR_SIZE;
  uint64_t sector = b->blockno * (BSIZE / FNU_BLOCKDEV_SECTOR_SIZE);

  if (minor(b->dev) != 0 || ramdisk_data == 0 ||
      sector + sectors > ramdisk_sectors) {
    b->flags |= B_ERROR;
    return;
  }

  char *target = ramdisk_data + b->blockno * BSIZE;

  if (b->flags & B_DIRTY) {
    memmove(target, bufdata(b), bytes);
  } else {
    memmove(bufdata(b), target, bytes);
  }

  b->flags |= B_VALID;
  b->flags &= ~B_DIRTY;
}
