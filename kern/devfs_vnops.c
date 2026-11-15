
#include "blockdev.h"
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "fs.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"
#include "uio.h"
#include "vfs.h"

// wall time of an uptime, by the clock as it is now
static void wall_of(int64_t up, struct fnu_timespec *t) {
  struct fnu_timespec now;
  int64_t ns;

  time_now(&now);
  ns = now.sec * 1000000000LL + now.nsec - (uptime_ns() - up);
  t->sec = ns / 1000000000LL;
  t->nsec = (int32_t)(ns % 1000000000LL);
  t->pad = 0;
}

static void devfs_load(struct inode *ip) {
  struct devfs_node dn;

  ip->nlink = 1;
  ip->size = 0;
  ip->valid = 1;
  if (ip->inum == DEVFS_ROOT_INO) {
    ip->type = FNU_TYPE_DIR;
    ip->mode = 0555;
    ip->uid = 0;
    ip->gid = 0;
    wall_of(0, &ip->mtime);
    ip->atime = ip->ctime = ip->birthtime = ip->mtime;
    return;
  }
  if (devfs_by_inum(ip->inum, &dn) < 0) {
    ip->type = 0;
    return;
  }
  ip->type = dn.type;
  ip->major = dn.major;
  ip->minor = dn.minor;
  ip->mode = dn.mode;
  ip->uid = dn.uid;
  ip->gid = dn.gid;
  wall_of(dn.born, &ip->mtime);
  ip->atime = ip->ctime = ip->birthtime = ip->mtime;
}

static int disk_io(struct inode *ip, struct uio *uio) {
  struct blockdev whole;
  size_t n = uio->uio_resid;
  int r;

  whole.device = makedev((uint)ip->major, (uint)ip->minor);
  whole.first_lba = 0;
  whole.sector_count = blockdev_device_size(whole.device);
  if (whole.sector_count == 0)
    return uio->uio_rw == UIO_READ ? -FNU_ERR_IO : -FNU_ERR_NODEVICE;
  r = uio->uio_rw == UIO_READ ? blockdev_read_uio(&whole, uio)
                              : blockdev_write_uio(&whole, uio);
  return r == 0 ? (int)n : -FNU_ERR_IO;
}

static int devfs_read(struct inode *ip, struct uio *uio) {
  if (ip->type == FNU_TYPE_DISK)
    return disk_io(ip, uio);
  if (ip->type != FNU_TYPE_DEVICE || ip->major < 0 || ip->major >= NDEV ||
      devsw[ip->major].read == 0)
    return -FNU_ERR_IO;
  return devsw[ip->major].read(ip, uio);
}

static int devfs_write(struct inode *ip, struct uio *uio) {
  if (ip->type == FNU_TYPE_DISK)
    return disk_io(ip, uio);
  if (ip->type != FNU_TYPE_DEVICE || ip->major < 0 || ip->major >= NDEV ||
      devsw[ip->major].write == 0)
    return -FNU_ERR_NODEVICE;
  return devsw[ip->major].write(ip, uio);
}

static struct inode *devfs_lookup_vop(struct inode *dp, char *name) {
  struct devfs_node dn;

  if (name[0] == '.' && name[1] == '\0')
    return iget(DEVFS_DEV, dp->inum);
  if (name[0] == '.' && name[1] == '.' && name[2] == '\0')
    return iget(DEVFS_DEV, devfs_parent(dp->inum));
  if (devfs_lookup(dp->inum, name, &dn) < 0)
    return 0;
  return iget(DEVFS_DEV, dn.inum);
}

struct inode *devfs_ctty(struct process *pr) {
  struct devfs_node dn;
  char name[DEVFS_NAME_MAX + 1];
  int tty;

  if ((tty = session_tty(pr)) < 0)
    return 0;
  tty_devname(tty, name, sizeof(name));
  if (devfs_lookup(DEVFS_ROOT_INO, name, &dn) < 0)
    return 0;
  return iget(DEVFS_DEV, dn.inum);
}

static int devfs_readdir(struct inode *dp, uint off, uint *next, uint *inum,
                         char *name, int namesz, uchar *dtype) {
  struct devfs_node dn;

  if (off == 0) {
    *inum = dp->inum;
    strlcpy(name, ".", namesz);
    *dtype = FNU_TYPE_DIR;
  } else if (off == 1) {
    *inum = devfs_parent(dp->inum);
    strlcpy(name, "..", namesz);
    *dtype = FNU_TYPE_DIR;
  } else {
    if (devfs_nth(dp->inum, off - 2, &dn) < 0)
      return -1;
    *inum = dn.inum;
    strlcpy(name, dn.name, namesz);
    *dtype = dn.type == FNU_TYPE_DIR ? FNU_TYPE_DIR : dn.type == FNU_TYPE_DISK ? FNU_TYPE_DISK : FNU_TYPE_DEVICE;
  }
  *next = off + 1;
  return 0;
}

static int devfs_setattr_vop(struct inode *ip, const struct vattr *va) {
  ushort mode = (va->va_mask & AT_MODE) ? va->va_mode & 07777 : ip->mode;
  uint uid = (va->va_mask & AT_OWNER) ? va->va_uid : ip->uid;
  uint gid = (va->va_mask & AT_OWNER) ? va->va_gid : ip->gid;

  if (va->va_mask & AT_FLAGS)
    return FNU_ERR_UNSUPPORTED;
  if ((va->va_mask & (AT_MODE | AT_OWNER)) &&
      devfs_setattr(ip->inum, mode, uid, gid) < 0)
    return FNU_ERR_UNSUPPORTED;
  ip->mode = mode;
  ip->uid = uid;
  ip->gid = gid;
  if (va->va_mask & AT_ATIME)
    ip->atime = va->va_atime;
  if (va->va_mask & AT_MTIME)
    ip->mtime = va->va_mtime;
  time_now(&ip->ctime);
  return 0;
}

static int devfs_pathconf(struct inode *ip __unused, int name, int64_t *value) {
  switch (name) {
  case FNU_PC_LINK_MAX:
    *value = 1;
    return 0;
  case FNU_PC_NAME_MAX:
    *value = DEVFS_NAME_MAX;
    return 0;
  case FNU_PC_2_SYMLINKS:
    *value = 0;
    return 0;
  case FNU_PC_ALLOC_SIZE_MIN:
    *value = BSIZE;
    return 0;
  case FNU_PC_FILESIZEBITS:
    *value = 64;
    return 0;
  case FNU_PC_TIMESTAMP_RESOLUTION:
    *value = 1;
    return 0;
  default:
    return vop_pathconf_default(name, value);
  }
}

static const struct vnodeops devfs_vnops = {
    .load = devfs_load,
    .read = devfs_read,
    .write = devfs_write,
    .lookup = devfs_lookup_vop,
    .readdir = devfs_readdir,
    .setattr = devfs_setattr_vop,
    .pathconf = devfs_pathconf,
};

const struct vfsops devfs_vfsops = {
    .name = "devfs",
    .vnops = &devfs_vnops,
};
