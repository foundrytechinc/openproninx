#ifndef FNU_PRONINX_BLOCKDEV_H
#define FNU_PRONINX_BLOCKDEV_H

#include "inc/types.h"
#include "inc/abi.h"

#define FNU_BLOCKDEV_SECTOR_SIZE 512

#define NBLOCKMAJOR 16

struct buf;

// the block device switch
struct blockdrv {
  const char *name;
  const char *prefix;
  uint units;
  void (*strategy)(struct buf *b);
  uint64_t (*size)(uint unit);
  int (*removable)(uint unit);
  uint (*maxphys)(uint unit);
  int (*flush)(uint unit);
  int (*powerdown)(uint unit); // after the flush
  void (*release)(uint unit);  // a gone unit no mount names any more
  int (*memory)(uint unit);
  int (*ident)(uint unit, struct fnu_diskinfo *out); // model, serial, kind
};

struct blockdev {
  uint device;
  uint64_t first_lba;
  uint64_t sector_count;
};

int blockdev_register(const struct blockdrv *driver);
const char *blockdev_driver_name(uint device);

int blockdev_name(uint device, char *out, int size);
int blockdev_removable(uint device);
int blockdev_ident(uint device, struct fnu_diskinfo *out);
int blockdev_nth_disk(uint n, uint *device);
int blockdev_serial(uint device, char *out, int size);
void blockdev_serno(uint device, uint disk, int part);
void blockdev_publish(uint device);
void blockdev_publish_as(uint device, const char *name);

void partition_init(void);
void partition_scan(uint parent);
void partition_reprobe(uint parent);

void storage_publish_labels(uint dev);
void storage_forget_labels(uint dev);
void partition_forget(uint parent);
uint partition_by_uuid(const char *uuid);
uint partition_labelled(const char *label, int *count);
uint partition_disk(uint device);
int partition_busy_disk(uint disk);
int partition_is(uint device);
void partition_release(uint device);
int blockdev_busy(uint device);
int vfs_dev_mounted(uint dev);
int swap_holds(uint dev);
void blockdev_release(uint device);
void blockdev_released(uint device);
uint partition_root(uint disk);
uint partition_swap(uint disk);
void blockdev_withdraw(uint device);
void blockdev_publish_all(void);

uint64_t blockdev_device_size(uint device);
void blockdev_rw(struct buf *b);
uint blockdev_maxphys(uint device);
int blockdev_flush(uint device);
void blockdev_stall(void);
void blockdev_restart(void);
void blockdev_flush_all(void);
void blockdev_powerdown_all(void);
int blockdev_memory(uint device);

struct uio;
void blockdev_stage_init(void);
int blockdev_read_uio(const struct blockdev *volume, struct uio *uio);
int blockdev_read_data_uio(const struct blockdev *volume, uint64_t offset,
                           struct uio *uio, uint length);
int blockdev_write_data_uio(const struct blockdev *volume, uint64_t offset,
                            struct uio *uio, uint length);
int blockdev_write_uio(const struct blockdev *volume, struct uio *uio);
int blockdev_read_raw(const struct blockdev *volume, uint64_t offset, void *dst,
                      uint length);
int blockdev_write_raw(const struct blockdev *volume, uint64_t offset,
                       const void *src, uint length);
int blockdev_read(const struct blockdev *volume, uint64_t offset, void *dst,
                  uint length);
int blockdev_write(const struct blockdev *volume, uint64_t offset,
                   const void *src, uint length);
int blockdev_read_data(const struct blockdev *volume, uint64_t offset,
                       void *dst, uint length);
int blockdev_write_data(const struct blockdev *volume, uint64_t offset,
                        const void *src, uint length);

#endif
