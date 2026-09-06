#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "fs.h"
#include "ramdisk.h"

// bread() is currently addressed in legacy filesystem blocks.  It is safe to
// use it as a sector transport only while both sizes are identical.  Keep the
// assumption explicit: a future VirtIO/NVMe transport can remove it without
// changing UFS2's byte-oriented interface.
#if BSIZE != FNU_BLOCKDEV_SECTOR_SIZE
#error "blockdev requires a sector-addressed backing transport"
#endif

uint64_t blockdev_device_size(uint device) {
  if (device < DEV_IDE_END) {
    return ide_size(device);
  } else if (device >= DEV_SATA_START && device < DEV_SATA_END) {
    return ahci_size(device - DEV_SATA_START);
  } else if (device >= DEV_NVME_START && device < DEV_NVME_END) {
    return nvme_size(device - DEV_NVME_START);
  } else if (device == DEV_RAMDISK) {
    return ramdisk_size();
  }
  return 0;
}

void blockdev_rw(struct buf *b) {
  if (b->dev < DEV_IDE_END) {
    iderw(b);
  } else if (b->dev >= DEV_SATA_START && b->dev < DEV_SATA_END) {
    ahcirw(b);
  } else if (b->dev >= DEV_NVME_START && b->dev < DEV_NVME_END) {
    nvmerw(b);
  } else if (b->dev == DEV_RAMDISK) {
    ramdiskrw(b);
  } else {
    panic("blockdev_rw: unknown device");
  }
}

int blockdev_read(const struct blockdev *volume, uint64_t offset, void *dst,
                  uint length) {
  uint64_t absolute_lba;
  char *out = dst;

  if (volume == 0 || dst == 0)
    return -1;
  if (volume->sector_count > ~(uint64_t)0 / FNU_BLOCKDEV_SECTOR_SIZE)
    return -1;
  if (offset > volume->sector_count * FNU_BLOCKDEV_SECTOR_SIZE ||
      (uint64_t)length >
          volume->sector_count * FNU_BLOCKDEV_SECTOR_SIZE - offset)
    return -1;

  while (length > 0) {
    struct buf *buffer;
    uint in_sector = offset % FNU_BLOCKDEV_SECTOR_SIZE;
    uint count = FNU_BLOCKDEV_SECTOR_SIZE - in_sector;

    if (count > length)
      count = length;
    absolute_lba = volume->first_lba + offset / FNU_BLOCKDEV_SECTOR_SIZE;
    // The current legacy buffer cache uses 32-bit sector addresses. Refuse a
    // request that it cannot represent instead of truncating the address.
    if (absolute_lba > 0xffffffffULL)
      return -1;

    buffer = bread(volume->device, (uint)absolute_lba);
    memmove(out, buffer->data + in_sector, count);
    brelse(buffer);
    out += count;
    offset += count;
    length -= count;
  }
  return 0;
}

// This is deliberately a synchronous, raw write.  It is not a replacement
// for a UFS2 transaction: a filesystem writer must update on-disk metadata in
// a recoverable order before it can use this transport for normal operations.
int blockdev_write(const struct blockdev *volume, uint64_t offset,
                   const void *src, uint length) {
  uint64_t absolute_lba;
  const char *in = src;

  if (volume == 0 || src == 0)
    return -1;
  if (volume->sector_count > ~(uint64_t)0 / FNU_BLOCKDEV_SECTOR_SIZE)
    return -1;
  if (offset > volume->sector_count * FNU_BLOCKDEV_SECTOR_SIZE ||
      (uint64_t)length >
          volume->sector_count * FNU_BLOCKDEV_SECTOR_SIZE - offset)
    return -1;

  while (length > 0) {
    struct buf *buffer;
    uint in_sector = offset % FNU_BLOCKDEV_SECTOR_SIZE;
    uint count = FNU_BLOCKDEV_SECTOR_SIZE - in_sector;

    if (count > length)
      count = length;
    absolute_lba = volume->first_lba + offset / FNU_BLOCKDEV_SECTOR_SIZE;
    if (absolute_lba > 0xffffffffULL)
      return -1;

    buffer = bread(volume->device, (uint)absolute_lba);
    memmove(buffer->data + in_sector, in, count);
    bwrite(buffer);
    brelse(buffer);
    in += count;
    offset += count;
    length -= count;
  }
  return 0;
}
