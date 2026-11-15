#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "devfs.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "fs.h"
#include "ramdisk.h"
#include "mmu.h"
#include "param.h"
#include "mutex.h"
#include "proc.h"
#include "uio.h"

#if BSIZE != FNU_BLOCKDEV_SECTOR_SIZE
#error "blockdev requires a sector-addressed backing transport"
#endif

static const struct blockdrv *bdevsw[NBLOCKMAJOR];

// suspend holds the gate. only the suspender walks through
static struct {
  struct mutex lock;
  int stalled;
  int inflight;
  struct proc *owner;
} gate = {MUTEX_INITIALIZER("bdevgate"), 0, 0, 0};

void blockdev_stall(void) {
  mtx_enter(&gate.lock);
  gate.stalled = 1;
  gate.owner = curproc;
  while (gate.inflight > 0)
    msleep(&gate.inflight, &gate.lock, PRIBIO, "bdevdrain", 0);
  mtx_leave(&gate.lock);
}

void blockdev_restart(void) {
  mtx_enter(&gate.lock);
  gate.stalled = 0;
  gate.owner = 0;
  wakeup(&gate.stalled);
  mtx_leave(&gate.lock);
}

int blockdev_register(const struct blockdrv *driver) {
  int maj;

  if (driver == 0 || driver->strategy == 0 || driver->size == 0 ||
      driver->units == 0)
    return 0;
  for (maj = 1; maj < NBLOCKMAJOR; maj++)
    if (bdevsw[maj] == 0) {
      bdevsw[maj] = driver;
      return maj;
    }
  printf("blockdev: no free major for %s\n", driver->name);
  return 0;
}

static const struct blockdrv *lookup(uint device) {
  uint maj = major(device);

  if (maj == 0 || maj >= NBLOCKMAJOR)
    return 0;
  return bdevsw[maj];
}

const char *blockdev_driver_name(uint device) {
  const struct blockdrv *d = lookup(device);

  return d != 0 ? d->name : "none";
}

int blockdev_removable(uint device) {
  const struct blockdrv *d = lookup(device);

  return d != 0 && d->removable != 0 ? d->removable(minor(device)) : 0;
}

int blockdev_ident(uint device, struct fnu_diskinfo *out) {
  const struct blockdrv *d = lookup(device);

  memset(out, 0, sizeof(*out));
  if (d == 0 || (out->sectors = blockdev_device_size(device)) == 0)
    return -1;
  out->sector_size = FNU_BLOCKDEV_SECTOR_SIZE;
  if (d->ident == 0 || d->ident(minor(device), out) < 0)
    strlcpy(out->model, d->name, sizeof(out->model));
  return 0;
}

int blockdev_nth_disk(uint n, uint *device) {
  uint maj, unit;

  for (maj = 1; maj < NBLOCKMAJOR; maj++)
    for (unit = 0; bdevsw[maj] != 0 && bdevsw[maj]->prefix != 0 &&
                   unit < bdevsw[maj]->units;
         unit++)
      if (blockdev_device_size(makedev(maj, unit)) != 0 && n-- == 0) {
        *device = makedev(maj, unit);
        return 0;
      }
  return -1;
}

int blockdev_serial(uint device, char *out, int size) {
  struct fnu_diskinfo info;
  const char *raw = info.serial;
  int i, n = 0, end;
  char c;

  if (size <= 0 || blockdev_ident(device, &info) < 0 || info.serial[0] == 0)
    return -1;
  for (i = 0; raw[i] == ' '; i++)
    ;
  for (end = (int)strlen(raw); end > i && raw[end - 1] == ' '; end--)
    ;
  for (; i < end && n < size - 1; i++) {
    c = raw[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '@' || c == '+' ||
          c == '.'))
      c = '_';
    out[n++] = c;
  }
  out[n] = 0;
  return n > 0 ? 0 : -1;
}

void blockdev_serno(uint device, uint disk, int part) {
  char name[DEVFS_NAME_MAX + 1];
  int n;

  if (blockdev_serial(disk, name, sizeof(name)) < 0)
    return;
  n = (int)strlen(name);
  if (part > 0 && snprintf(name + n, sizeof(name) - (size_t)n, "p%d", part) >=
                      (int)sizeof(name) - n)
    return;
  devfs_add_alias("serno", name, FNU_TYPE_DISK, (short)major(device),
                  (short)minor(device), 0640);
}

static void serno_forget(uint device) {
  devfs_remove_aliases("serno", (short)major(device), (short)minor(device));
}

uint64_t blockdev_device_size(uint device) {
  const struct blockdrv *d = lookup(device);

  return d != 0 ? d->size(minor(device)) : 0;
}

uint blockdev_maxphys(uint device) {
  const struct blockdrv *d = lookup(device);
  uint n;

  if (d == 0 || d->maxphys == 0)
    return BSIZE;
  n = d->maxphys(minor(device));
  return n < BSIZE ? BSIZE : n;
}

int blockdev_flush(uint device) {
  const struct blockdrv *d = lookup(device);

  if (d == 0)
    return -1;
  return d->flush != 0 ? d->flush(minor(device)) : 0;
}

void blockdev_flush_all(void) {
  uint maj, unit, dev;

  for (maj = 1; maj < NBLOCKMAJOR; maj++)
    for (unit = 0; bdevsw[maj] != 0 && unit < bdevsw[maj]->units; unit++) {
      dev = makedev(maj, unit);
      if (bdevsw[maj]->flush != 0 && blockdev_device_size(dev) != 0)
        bdevsw[maj]->flush(unit);
    }
}

// a gone device keeps its number, or the next disk inherits its mounts.
// вот это был пиздец
int blockdev_busy(uint device) {
  return vfs_dev_mounted(device) || swap_holds(device) || partition_busy_disk(device);
}

void blockdev_release(uint device) {
  const struct blockdrv *d = lookup(device);

  if (d != 0 && d->release != 0)
    d->release(minor(device));
}

// a mount or the swap let go of device
void blockdev_released(uint device) {
  if (partition_is(device))
    partition_release(device);
  else if (blockdev_device_size(device) == 0)
    blockdev_release(device);
}

void blockdev_powerdown_all(void) {
  uint maj, unit, dev;

  for (maj = 1; maj < NBLOCKMAJOR; maj++)
    for (unit = 0; bdevsw[maj] != 0 && unit < bdevsw[maj]->units; unit++) {
      dev = makedev(maj, unit);
      if (bdevsw[maj]->powerdown != 0 && blockdev_device_size(dev) != 0)
        bdevsw[maj]->powerdown(unit);
    }
}

int blockdev_memory(uint device) {
  const struct blockdrv *d = lookup(device);

  return d != 0 && d->memory != 0 && d->memory(minor(device));
}

void blockdev_rw(struct buf *b) {
  const struct blockdrv *d = lookup(b->dev);
  int flags = b->flags;

  if (bufdata(b) == 0 || bufcount(b) == 0)
    panic("blockdev_rw: buffer without memory");
  if (d == 0) {
    b->flags |= B_ERROR;
    return;
  }
  b->flags &= ~B_ERROR;
  mtx_enter(&gate.lock);
  while (gate.stalled && curproc != gate.owner && curproc != 0 && panicstr == 0)
    msleep(&gate.stalled, &gate.lock, PRIBIO, "bdevstall", 0);
  gate.inflight++;
  mtx_leave(&gate.lock);
  d->strategy(b);
  mtx_enter(&gate.lock);
  if (--gate.inflight == 0 && gate.stalled)
    wakeup(&gate.inflight);
  mtx_leave(&gate.lock);
  if ((flags & (B_LOGREAD | B_FLUSH)) || (b->flags & B_ERROR))
    return;
  if (flags & B_DIRTY)
    ulog_overlay_write(b->dev, b->blockno, bufcount(b) / BSIZE);
  else
    ulog_overlay_read(b->dev, b->blockno, bufcount(b) / BSIZE, bufdata(b));
}

static int volume_read(const struct blockdev *volume, uint64_t offset,
                       void *dst, uint length, int age) {
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
    buffer = bread(volume->device, absolute_lba);
    if (buffer->flags & B_ERROR) {
      brelse(buffer);
      return -1;
    }
    memmove(out, buffer->data + in_sector, count);
    if (age && count == FNU_BLOCKDEV_SECTOR_SIZE)
      buffer->flags |= B_AGE;
    brelse(buffer);
    out += count;
    offset += count;
    length -= count;
  }
  return 0;
}

// a raw synchronous write
static int volume_write(const struct blockdev *volume, uint64_t offset,
                        const void *src, uint length, int age) {
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
    buffer = count == FNU_BLOCKDEV_SECTOR_SIZE
                 ? bgetblk(volume->device, absolute_lba)
                 : bread(volume->device, absolute_lba);
    if (buffer->flags & B_ERROR) {
      brelse(buffer);
      return -1;
    }
    memmove(buffer->data + in_sector, in, count);
    if (age && count == FNU_BLOCKDEV_SECTOR_SIZE)
      buffer->flags |= B_AGE;
    bdwrite(buffer);
    in += count;
    offset += count;
    length -= count;
  }
  return 0;
}

int blockdev_name(uint device, char *out, int size) {
  struct devfs_node dn;
  uint k;

  for (k = 0; devfs_nth(DEVFS_ROOT_INO, k, &dn) == 0; k++)
    if (dn.type == FNU_TYPE_DISK && (uint)dn.major == major(device) &&
        (uint)dn.minor == minor(device)) {
      strlcpy(out, dn.name, size);
      return 0;
    }
  return -1;
}

// announce under a chosen name
void blockdev_publish_as(uint device, const char *name) {
  char taken[DEVFS_NAME_MAX + 1];

  if (blockdev_device_size(device) == 0)
    return;
  if (blockdev_name(device, taken, sizeof(taken)) == 0)
    return;
  if (devfs_lookup(DEVFS_ROOT_INO, name, 0) == 0)
    return;
  devfs_add(name, FNU_TYPE_DISK, (short)major(device), (short)minor(device), 0640);
  printf("%s: %s, %d MB%s\n", name, blockdev_driver_name(device),
          (uint)(blockdev_device_size(device) / 2048),
          blockdev_removable(device) ? ", removable" : "");
  storage_publish_labels(device);
  hotplug_attach(FNU_DEVCLASS_DISK, name,
                 blockdev_removable(device) ? FNU_HOTPLUG_REMOVABLE : 0, device);
  root_mount_published();
}

void blockdev_publish(uint device) {
  const struct blockdrv *d = lookup(device);
  const char *prefix;
  char name[DEVFS_NAME_MAX + 1];
  int i, n;

  if (d == 0 || d->prefix == 0 || blockdev_device_size(device) == 0)
    return;
  prefix = d->prefix;
  if (blockdev_name(device, name, sizeof(name)) == 0)
    return;
  for (i = 0; i < 10; i++) {
    for (n = 0; prefix[n] != 0 && n < DEVFS_NAME_MAX - 1; n++)
      name[n] = prefix[n];
    name[n++] = (char)('0' + i);
    name[n] = 0;
    if (devfs_lookup(DEVFS_ROOT_INO, name, 0) == 0)
      continue;
    blockdev_publish_as(device, name);
    blockdev_serno(device, device, 0);
    partition_scan(device);
    return;
  }
}

void blockdev_withdraw(uint device) {
  char name[DEVFS_NAME_MAX + 1];

  partition_forget(device);
  storage_forget_labels(device);
  serno_forget(device);
  if (blockdev_name(device, name, sizeof(name)) < 0)
    return;
  devfs_remove(name);
  hotplug_detach(FNU_DEVCLASS_DISK, name, 0, device);
}

void blockdev_publish_all(void) {
  uint maj, unit, dev;

  for (maj = 1; maj < NBLOCKMAJOR; maj++)
    for (unit = 0; bdevsw[maj] != 0 && unit < bdevsw[maj]->units; unit++) {
      dev = makedev(maj, unit);
      if (blockdev_device_size(dev) == 0)
        continue;
      blockdev_publish(dev);
    }
}

// THE RAW DEVICE
#define STAGE_N 8

static struct {
  struct mutex lock;
  char *buf[STAGE_N];
  int ready;
} stage;

void blockdev_stage_init(void) {
  int i;

  mtx_init(&stage.lock, "blkstage");
  for (i = 0; i < STAGE_N; i++)
    if ((stage.buf[i] = (char *)contig_alloc(MAXPHYS)) == 0)
      break;
  stage.ready = i;
}

static char *stage_get(int *size) {
  char *p = 0;
  int i;

  mtx_enter(&stage.lock);
  for (i = 0; i < stage.ready; i++)
    if (stage.buf[i] != 0) {
      p = stage.buf[i];
      stage.buf[i] = 0;
      break;
    }
  mtx_leave(&stage.lock);
  if (p != 0) {
    *size = MAXPHYS;
    return p;
  }
  *size = PGSIZE;
  return page_alloc();
}

static void stage_put(char *p, int size) {
  int i;

  if (size == PGSIZE) {
    page_free(p);
    return;
  }
  mtx_enter(&stage.lock);
  for (i = 0; i < stage.ready; i++)
    if (stage.buf[i] == 0) {
      stage.buf[i] = p;
      break;
    }
  mtx_leave(&stage.lock);
}

int blockdev_read_uio(const struct blockdev *volume, struct uio *uio) {
  char *buf;
  int size, r = 0;

  if (volume == 0)
    return -1;
  if ((buf = stage_get(&size)) == 0)
    return -1;
  while (uio->uio_resid > 0) {
    size_t chunk = uio->uio_resid > (size_t)size ? (size_t)size
                                                 : uio->uio_resid;
    uint64_t at = uio->uio_offset;

    if ((r = blockdev_read_raw(volume, at, buf, (uint)chunk)) != 0)
      break;
    if (uiomove(buf, chunk, uio) != 0) {
      r = -1;
      break;
    }
  }
  stage_put(buf, size);
  return r;
}

int blockdev_write_uio(const struct blockdev *volume, struct uio *uio) {
  char *buf;
  int size, r = 0;

  if (volume == 0)
    return -1;
  if ((buf = stage_get(&size)) == 0)
    return -1;
  while (uio->uio_resid > 0) {
    size_t chunk = uio->uio_resid > (size_t)size ? (size_t)size
                                                 : uio->uio_resid;
    uint64_t at = uio->uio_offset;

    if (uiomove(buf, chunk, uio) != 0) {
      r = -1;
      break;
    }
    if ((r = blockdev_write_raw(volume, at, buf, (uint)chunk)) != 0)
      break;
  }
  stage_put(buf, size);
  return r;
}

int blockdev_read_raw(const struct blockdev *volume, uint64_t offset, void *dst,
                      uint length) {
  char *out = dst;
  uint most;


  if (volume == 0 || dst == 0)
    return -1;
  most = blockdev_maxphys(volume->device) / FNU_BLOCKDEV_SECTOR_SIZE;
  while (most != 0 && length >= FNU_BLOCKDEV_SECTOR_SIZE &&
         offset % FNU_BLOCKDEV_SECTOR_SIZE == 0) {
    uint run = length / FNU_BLOCKDEV_SECTOR_SIZE;

    if (run > most)
      run = most;
    if (offset / FNU_BLOCKDEV_SECTOR_SIZE + run > volume->sector_count)
      return -1;
    if (bio_read_run(volume->device,
                     volume->first_lba + offset / FNU_BLOCKDEV_SECTOR_SIZE,
                     run, (uchar *)out) < 0)
      return -1;
    out += run * FNU_BLOCKDEV_SECTOR_SIZE;
    offset += run * FNU_BLOCKDEV_SECTOR_SIZE;
    length -= run * FNU_BLOCKDEV_SECTOR_SIZE;
  }
  return length == 0 ? 0 : blockdev_read(volume, offset, out, length);
}

int blockdev_write_raw(const struct blockdev *volume, uint64_t offset,
                       const void *src, uint length) {
  const char *in = src;
  uint most;

  if (volume == 0 || src == 0)
    return -1;
  most = blockdev_maxphys(volume->device) / FNU_BLOCKDEV_SECTOR_SIZE;
  while (most != 0 && length >= FNU_BLOCKDEV_SECTOR_SIZE &&
         offset % FNU_BLOCKDEV_SECTOR_SIZE == 0) {
    uint run = length / FNU_BLOCKDEV_SECTOR_SIZE;

    if (run > most)
      run = most;
    if (offset / FNU_BLOCKDEV_SECTOR_SIZE + run > volume->sector_count)
      return -1;
    if (bio_write_run(volume->device,
                      volume->first_lba + offset / FNU_BLOCKDEV_SECTOR_SIZE,
                      run, (const uchar *)in) < 0)
      return -1;
    in += run * FNU_BLOCKDEV_SECTOR_SIZE;
    offset += run * FNU_BLOCKDEV_SECTOR_SIZE;
    length -= run * FNU_BLOCKDEV_SECTOR_SIZE;
  }
  return length == 0 ? 0 : blockdev_write(volume, offset, in, length);
}

int blockdev_read(const struct blockdev *volume, uint64_t offset, void *dst,
                  uint length) {
  return volume_read(volume, offset, dst, length, 0);
}

int blockdev_write(const struct blockdev *volume, uint64_t offset,
                   const void *src, uint length) {
  return volume_write(volume, offset, src, length, 0);
}

static int volume_read_uio(const struct blockdev *volume, uint64_t offset,
                           struct uio *uio, uint length, int age) {
  if (volume == 0)
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
    buffer = bread(volume->device,
                   volume->first_lba + offset / FNU_BLOCKDEV_SECTOR_SIZE);
    if (buffer->flags & B_ERROR) {
      brelse(buffer);
      return -1;
    }
    if (uiomove(buffer->data + in_sector, count, uio) != 0) {
      brelse(buffer);
      return -1;
    }
    if (age && count == FNU_BLOCKDEV_SECTOR_SIZE)
      buffer->flags |= B_AGE;
    brelse(buffer);
    offset += count;
    length -= count;
  }
  return 0;
}

static int volume_write_uio(const struct blockdev *volume, uint64_t offset,
                            struct uio *uio, uint length, int age) {
  if (volume == 0)
    return -1;
  if (offset > volume->sector_count * FNU_BLOCKDEV_SECTOR_SIZE ||
      (uint64_t)length >
          volume->sector_count * FNU_BLOCKDEV_SECTOR_SIZE - offset)
    return -1;
  while (length > 0) {
    struct buf *buffer;
    uint in_sector = offset % FNU_BLOCKDEV_SECTOR_SIZE;
    uint count = FNU_BLOCKDEV_SECTOR_SIZE - in_sector;
    uint64_t lba;

    if (count > length)
      count = length;
    lba = volume->first_lba + offset / FNU_BLOCKDEV_SECTOR_SIZE;
    buffer = count == FNU_BLOCKDEV_SECTOR_SIZE ? bgetblk(volume->device, lba)
                                              : bread(volume->device, lba);
    if (buffer->flags & B_ERROR) {
      brelse(buffer);
      return -1;
    }
    if (uiomove(buffer->data + in_sector, count, uio) != 0) {
      brelse(buffer);
      return -1;
    }
    if (age && count == FNU_BLOCKDEV_SECTOR_SIZE)
      buffer->flags |= B_AGE;
    bdwrite(buffer);
    offset += count;
    length -= count;
  }
  return 0;
}

int blockdev_read_data_uio(const struct blockdev *volume, uint64_t offset,
                           struct uio *uio, uint length) {
  return volume_read_uio(volume, offset, uio, length, 1);
}

int blockdev_write_data_uio(const struct blockdev *volume, uint64_t offset,
                            struct uio *uio, uint length) {
  return volume_write_uio(volume, offset, uio, length, 1);
}


int blockdev_read_data(const struct blockdev *volume, uint64_t offset,
                       void *dst, uint length) {
  return volume_read(volume, offset, dst, length, 1);
}

int blockdev_write_data(const struct blockdev *volume, uint64_t offset,
                        const void *src, uint length) {
  return volume_write(volume, offset, src, length, 1);
}
