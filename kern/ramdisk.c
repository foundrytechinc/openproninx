#include "ramdisk.h"
#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "fs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"

static char *ramdisk_data;
static uint64_t ramdisk_sectors;

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
    cprintf("RAMDISK: rootfs from loader (%u sectors, %u KB)\n",
            (uint)ramdisk_sectors, (uint)(bi->module[i].size / 1024));
    return;
  }
  cprintf("RAMDISK: loader supplied no rootfs\n");
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

  if (ramdisk_data == 0 || ramdisk_sectors == 0)
    panic("ramdiskrw: no ramdisk present");

  uint sectors_per_block = BSIZE / FNU_BLOCKDEV_SECTOR_SIZE;
  uint64_t sector = (uint64_t)b->blockno * sectors_per_block;

  if (sector + sectors_per_block > ramdisk_sectors)
    panic("ramdiskrw: sector beyond ramdisk");

  char *target = ramdisk_data + ((uint64_t)b->blockno * BSIZE);

  if (b->flags & B_DIRTY) {
    memmove(target, b->data, BSIZE);
  } else {
    memmove(b->data, target, BSIZE);
  }

  b->flags |= B_VALID;
  b->flags &= ~B_DIRTY;
}
