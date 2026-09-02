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

// Read bytes relative to the beginning of a bounded volume. The initial UFS2
// integration is read-only; write support is intentionally absent.
int blockdev_read(const struct blockdev *volume, uint64_t offset, void *dst,
                  uint length);

#endif /* FNU_PRONINX_BLOCKDEV_H */
