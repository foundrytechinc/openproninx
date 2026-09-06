#include "ramdisk.h"
#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "fs.h"

extern char ramdisk_blob_start[];
extern char ramdisk_blob_end[];

static char *ramdisk_data;
static uint64_t ramdisk_sectors;

void ramdisk_init(void) {
  uintptr_t start = (uintptr_t)ramdisk_blob_start;
  uintptr_t end = (uintptr_t)ramdisk_blob_end;

  if (end > start) {
    uint64_t bytes = end - start;
    ramdisk_sectors = bytes / FNU_BLOCKDEV_SECTOR_SIZE;
    ramdisk_data = ramdisk_blob_start;
    cprintf("RAMDISK: embedded rootfs image ready (%d sectors, %d KB)\n",
            (uint)ramdisk_sectors, (uint)(bytes / 1024));
  } else {
    ramdisk_sectors = 0;
    ramdisk_data = 0;
  }
}

uint64_t ramdisk_size(void) {
  return ramdisk_sectors;
}

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
