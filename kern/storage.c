// FNU Data discovery. This is intentionally not a mount: it gives UFS2 a
// dedicated, read-only volume without coupling it to the legacy root image.
#include "blockdev.h"
#include "defs.h"
#include "param.h"
#include "ufs2.h"

#define FNU_DATA_DEVICE 2

static struct ufs2_volume fnu_data_volume;
static struct ufs2_volume fnu_root_volume;
static int fnu_data_ready;
static int fnu_root_ready;

const struct ufs2_volume *storage_ufs2_volume(uint device) {
  if (fnu_root_ready && device == rootdev)
    return &fnu_root_volume;
  if (fnu_data_ready && device == FNU_DATA_DEVICE)
    return &fnu_data_volume;
  return 0;
}

static int storage_validate_volume(uint device, struct ufs2_volume *volume) {
  struct blockdev block_device;
  struct ufs2_inode root;
  uint dot;
  uint64_t sectors = blockdev_device_size(device);

  if (sectors == 0)
    return -1;
  block_device.device = device;
  block_device.first_lba = 0;
  block_device.sector_count = sectors;
  if (ufs2_probe(&block_device, volume) < 0 ||
      ufs2_read_inode(volume, UFS2_ROOT_INO, &root) < 0 ||
      ufs2_lookup(volume, UFS2_ROOT_INO, ".", &dot) < 0 ||
      dot != UFS2_ROOT_INO)
    return -1;
  return 0;
}

int storage_mount_root(void) {
  if (fnu_root_ready)
    return 0;
  if (storage_validate_volume(rootdev, &fnu_root_volume) < 0)
    return -1;
  fnu_root_ready = 1;
  cprintf("FNU Root: UFS2 read-only system volume ready\n");
  return 0;
}

void storageinit(void) {
  uint64_t sectors = blockdev_device_size(FNU_DATA_DEVICE);

  if (sectors == 0) {
    return;
  }
  if (storage_validate_volume(FNU_DATA_DEVICE, &fnu_data_volume) < 0) {
    cprintf("FNU Data: device present, not a valid UFS2 volume\n");
    return;
  }
  fnu_data_ready = 1;
  cprintf("FNU Data: UFS2 read-only volume ready (%d sectors)\n",
          (uint)sectors);
}
