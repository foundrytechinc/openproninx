#ifndef FNU_PRONINX_BLOCKDEV_H
#define FNU_PRONINX_BLOCKDEV_H

#include "inc/types.h"

// The legacy IDE layer currently transfers 512-byte logical sectors. Keep
// this fact at the adapter boundary rather than leaking it into UFS2 code.
#define FNU_BLOCKDEV_SECTOR_SIZE 512

struct blockdev {
  uint device;
  uint64_t first_lba;
  uint64_t sector_count;
};

#define DEV_IDE_START   0
#define DEV_IDE_END     4
#define DEV_SATA_START  4
#define DEV_SATA_END    20
#define DEV_NVME_START  20
#define DEV_NVME_END    28
#define DEV_RAMDISK     28

struct buf;

uint64_t blockdev_device_size(uint device);
void blockdev_rw(struct buf *b);

// Read or write bytes relative to the beginning of a bounded volume. These
// are raw transports: callers are responsible for filesystem consistency.
int blockdev_read(const struct blockdev *volume, uint64_t offset, void *dst,
                  uint length);
int blockdev_write(const struct blockdev *volume, uint64_t offset,
                   const void *src, uint length);

#endif /* FNU_PRONINX_BLOCKDEV_H */
