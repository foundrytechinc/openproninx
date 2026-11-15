// UFS2

#include "defs.h"
#include "file.h"
#include "fs.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"
#include "ufs2.h"
#include "uio.h"
#include "vfs.h"

#define VOL(ip) vfs_ufs2_volume((ip)->dev)
#define UFS_LINK_MAX 32767

static void ufs_take(struct inode *ip) {
  ip->size = ip->ufs2_info.size;
  ip->nlink = (short)ip->ufs2_info.nlink;
  ip->mtime = ip->ufs2_info.mtime;
  ip->ctime = ip->ufs2_info.ctime;
}

static void ufs_reload(struct inode *ip) {
  const struct ufs2_volume *v = VOL(ip);

  if (v != 0 && ufs2_read_inode(v, ip->inum, &ip->ufs2_info) == 0)
    ufs_take(ip);
}

static int ufs_store(struct inode *ip) {
  const struct ufs2_volume *v = VOL(ip);

  itimes(ip);
  if (v == 0)
    return FNU_ERR_IO;
  ip->ufs2_info.mode = (ip->ufs2_info.mode & UFS2_IFMT) | (ip->mode & 07777);
  ip->ufs2_info.uid = ip->uid;
  ip->ufs2_info.gid = ip->gid;
  ip->ufs2_info.flags = ip->flags;
  ip->ufs2_info.nlink = (uint16_t)ip->nlink;
  ip->ufs2_info.atime = ip->atime;
  ip->ufs2_info.mtime = ip->mtime;
  ip->ufs2_info.ctime = ip->ctime;
  if (ufs2_write_inode(v, ip->inum, &ip->ufs2_info) < 0)
    return FNU_ERR_IO;
  ip->iflag &= ~IN_MODIFIED;
  return 0;
}

static void ufs_update(struct inode *ip) { ufs_store(ip); }

static short ufs_type(uint16_t mode) {
  switch (mode & UFS2_IFMT) {
  case UFS2_IFDIR:
    return FNU_TYPE_DIR;
  case UFS2_IFREG:
    return FNU_TYPE_FILE;
  case UFS2_IFLNK:
    return FNU_TYPE_LINK;
  case UFS2_IFIFO:
    return FNU_TYPE_FIFO;
  case UFS2_IFSOCK:
    return FNU_TYPE_SOCKET;
  default:
    return 0;
  }
}

static uint16_t ufs_ifmt(short type) {
  switch (type) {
  case FNU_TYPE_DIR:
    return UFS2_IFDIR;
  case FNU_TYPE_FILE:
    return UFS2_IFREG;
  case FNU_TYPE_LINK:
    return UFS2_IFLNK;
  case FNU_TYPE_FIFO:
    return UFS2_IFIFO;
  case FNU_TYPE_SOCKET:
    return UFS2_IFSOCK;
  default:
    return 0;
  }
}

static uchar ufs_dirtype(short type) {
  switch (type) {
  case FNU_TYPE_DIR:
    return UFS2_DIRTYPE_DIR;
  case FNU_TYPE_LINK:
    return UFS2_DIRTYPE_LNK;
  case FNU_TYPE_FIFO:
    return UFS2_DIRTYPE_FIFO;
  case FNU_TYPE_SOCKET:
    return UFS2_DIRTYPE_SOCK;
  default:
    return UFS2_DIRTYPE_REG;
  }
}

static void ufs_load(struct inode *ip) {
  const struct ufs2_volume *v = VOL(ip);

  if (v == 0 || ufs2_read_inode(v, ip->inum, &ip->ufs2_info) < 0 ||
      (ip->type = ufs_type(ip->ufs2_info.mode)) == 0) {
    memset(&ip->ufs2_info, 0, sizeof(ip->ufs2_info));
    ip->type = 0;
    ip->size = 0;
    ip->nlink = 1;
    ip->flags = 0;
    ip->valid = 0; // read again next time
    return;
  }
  ip->uid = ip->ufs2_info.uid;
  ip->gid = ip->ufs2_info.gid;
  ip->mode = ip->ufs2_info.mode & 07777;
  ip->flags = ip->ufs2_info.flags;
  ip->atime = ip->ufs2_info.atime;
  ip->birthtime = ip->ufs2_info.birthtime;
  ufs_take(ip);
  ip->iflag = 0;
  ip->valid = 1;
}

static void ufs_inactive(struct inode *ip) {
  const struct ufs2_volume *v = VOL(ip);

  if (v == 0)
    return;
  ufs2_truncate(v, ip->inum, &ip->ufs2_info);
  ufs2_free_inode(v, ip->inum);
  ip->size = 0;
}

static int ufs_truncate(struct inode *ip, uint64_t length) {
  const struct ufs2_volume *v = VOL(ip);
  int error;

  if (v == 0)
    return FNU_ERR_IO;
  error = -ufs2_truncate_to(v, ip->inum, &ip->ufs2_info, length);
  ufs_take(ip);
  return error;
}

static int ufs_read(struct inode *ip, struct uio *uio) {
  const struct ufs2_volume *v = VOL(ip);
  uint64_t off = uio->uio_offset;
  size_t n;

  if (v == 0)
    return -FNU_ERR_IO;
  if (off >= ip->size)
    return 0;
  if (uio->uio_resid > ip->size - off)
    uio->uio_resid = (size_t)(ip->size - off);
  n = uio->uio_resid;
  ip->iflag |= IN_ACCESS;
  if (ufs2_read_uio(v, ip->inum, &ip->ufs2_info, uio) < 0)
    return -FNU_ERR_IO;
  return (int)n;
}

static int ufs_write(struct inode *ip, struct uio *uio) {
  const struct ufs2_volume *v = VOL(ip);
  size_t want = uio->uio_resid;
  uint64_t osize;
  int r;

  if (v == 0)
    return -FNU_ERR_NODEVICE;
  if (ip->type != FNU_TYPE_FILE)
    return -FNU_ERR_INVALID;
  if (uio->uio_offset + uio->uio_resid < uio->uio_offset)
    return -FNU_ERR_TOOLARGE;
  osize = ip->ufs2_info.size;
  r = ufs2_write_uio(v, ip->inum, &ip->ufs2_info, uio);
  if (r == -FNU_ERR_NOSPACE && uio->uio_resid > 0 && ufs2_request_cleanup(v) &&
      ufs2_write_uio(v, ip->inum, &ip->ufs2_info, uio) >= 0)
    r = (int)want;
  // all or nothing: what it grew goes back
  if (r < 0) {
    if (ip->ufs2_info.size > osize)
      (void)ufs2_truncate_to(v, ip->inum, &ip->ufs2_info, osize);
    uio->uio_offset -= want - uio->uio_resid;
    uio->uio_resid = want;
  }
  ufs_take(ip);
  return r;
}

static struct inode *ufs_lookup(struct inode *dp, char *name) {
  const struct ufs2_volume *v = VOL(dp);
  uint inum;

  if (v == 0 || ufs2_lookup(v, dp->inum, name, &inum) < 0)
    return 0;
  return iget(dp->dev, inum);
}

static void ufs_unmake(const struct ufs2_volume *v, uint inum) {
  struct ufs2_inode d;

  if (ufs2_read_inode(v, inum, &d) == 0)
    ufs2_truncate(v, inum, &d);
  ufs2_free_inode(v, inum);
}

static int makeinode(struct inode *dp, char *name, struct vattr *va,
                     struct inode **ipp) {
  const struct ufs2_volume *v = VOL(dp);
  uint16_t fmt = ufs_ifmt(va->va_type);
  struct inode *ip;
  uint inum = 0;

  if (fmt == 0)
    return FNU_ERR_UNSUPPORTED;
  if (v == 0)
    return FNU_ERR_IO;
  ufs2_begin(v);
  if (ufs2_alloc_inode(v, fmt | (va->va_mode & 07777), dp->inum, &inum) < 0 ||
      (va->va_type == FNU_TYPE_DIR && ufs2_make_directory(v, inum, dp->inum) < 0) ||
      ufs2_dirlink(v, dp->inum, name, inum, ufs_dirtype(va->va_type)) < 0) {
    if (inum >= UFS2_ROOT_INO)
      ufs_unmake(v, inum);
    ufs2_end(v);
    return FNU_ERR_NOSPACE;
  }
  ufs_reload(dp);
  if (va->va_type == FNU_TYPE_DIR) {
    dp->nlink++;
    ufs_store(dp);
  }
  ufs2_end(v);
  if ((ip = iget(dp->dev, inum)) == 0)
    return FNU_ERR_SYSTEMFULL;
  ilock(ip);
  *ipp = ip;
  return 0;
}

static int ufs_create(struct inode *dp, char *name, struct vattr *va,
                      struct inode **ipp) {
  int error = makeinode(dp, name, va, ipp);

  if (error == FNU_ERR_NOSPACE && ufs2_request_cleanup(VOL(dp)))
    error = makeinode(dp, name, va, ipp);
  return error;
}

static int ufs_symlink(struct inode *dp, char *name, struct vattr *va,
                       const char *target, struct inode **ipp) {
  const struct ufs2_volume *v = VOL(dp);
  struct vattr lva = *va;
  struct inode *ip;
  int error;

  lva.va_type = FNU_TYPE_LINK;
  if ((error = ufs_create(dp, name, &lva, &ip)) != 0)
    return error;
  if (ufs2_symlink(v, ip->inum, &ip->ufs2_info, target) < 0) {
    ufs2_begin(v);
    ufs2_dirunlink(v, dp->inum, name, 0);
    ufs_reload(dp);
    ip->nlink = 0;
    ufs_store(ip);
    ufs2_end(v);
    iunlockput(ip);
    return FNU_ERR_IO;
  }
  ufs_take(ip);
  *ipp = ip;
  return 0;
}

static int link_once(struct inode *dp, char *name, struct inode *ip) {
  const struct ufs2_volume *v = VOL(dp);

  if (v == 0)
    return FNU_ERR_IO;
  if (ip->nlink == 0)
    return FNU_ERR_NOTFOUND;
  if (ip->nlink >= UFS_LINK_MAX)
    return FNU_ERR_TOOMANYLINKS;
  ufs2_begin(v);
  ip->nlink++;
  ip->iflag |= IN_CHANGE;
  if (ufs_store(ip) != 0 ||
      ufs2_dirlink(v, dp->inum, name, ip->inum, ufs_dirtype(ip->type)) < 0) {
    ip->nlink--;
    ufs_store(ip);
    ufs2_end(v);
    return FNU_ERR_IO;
  }
  ufs_reload(dp);
  ufs2_end(v);
  return 0;
}

static int ufs_link(struct inode *dp, char *name, struct inode *ip) {
  int error = link_once(dp, name, ip);

  if (error == FNU_ERR_IO && ufs2_request_cleanup(VOL(dp)))
    error = link_once(dp, name, ip);
  return error;
}

static int ufs_remove(struct inode *dp, char *name, struct inode *ip) {
  const struct ufs2_volume *v = VOL(dp);
  uint removed;

  if (v == 0 || ip->nlink == 0)
    return FNU_ERR_IO;
  ufs2_begin(v);
  if (ufs2_dirunlink(v, dp->inum, name, &removed) < 0 || removed != ip->inum) {
    ufs2_end(v);
    return FNU_ERR_IO;
  }
  ufs_reload(dp);
  if (ip->type == FNU_TYPE_DIR) {
    if (dp->nlink > 0)
      dp->nlink--;
    ufs_store(dp);
    ip->nlink = 0;
  } else
    ip->nlink--;
  ip->iflag |= IN_CHANGE;
  ufs_store(ip);
  ufs2_end(v);
  return 0;
}

static int ufs_rename(struct inode *fdp, char *fname, struct inode *ip,
                      struct inode *tdp, char *tname, struct inode *victim) {
  const struct ufs2_volume *v = VOL(tdp);
  int dir = ip->type == FNU_TYPE_DIR, error = FNU_ERR_IO;

  if (v == 0)
    return FNU_ERR_IO;
  ufs2_begin(v);
  if (victim != 0) {
    if (ufs2_dirunlink(v, tdp->inum, tname, 0) < 0)
      goto out;
    ufs_reload(tdp);
    if (victim->type == FNU_TYPE_DIR) {
      victim->nlink = 0;
      if (tdp->nlink > 0)
        tdp->nlink--;
      ufs_store(tdp);
    } else if (victim->nlink > 0)
      victim->nlink--;
    victim->iflag |= IN_CHANGE;
    ufs_store(victim);
  }
  if (ufs2_dirlink(v, tdp->inum, tname, ip->inum, ufs_dirtype(ip->type)) < 0)
    goto out;
  ufs_reload(tdp);
  if (ufs2_dirunlink(v, fdp->inum, fname, 0) < 0)
    goto out;
  ufs_reload(fdp);
  if (dir && tdp != fdp) {
    if (ufs2_dirunlink(v, ip->inum, "..", 0) < 0 ||
        ufs2_dirlink(v, ip->inum, "..", tdp->inum, UFS2_DIRTYPE_DIR) < 0)
      goto out;
    ufs_reload(ip);
    if (fdp->nlink > 0)
      fdp->nlink--;
    ufs_store(fdp);
    tdp->nlink++;
    ufs_store(tdp);
  }
  ip->iflag |= IN_CHANGE;
  ufs_store(ip);
  error = 0;
out:
  ufs2_end(v);
  return error;
}

static int ufs_readlink(struct inode *ip, char *buf, int size) {
  const struct ufs2_volume *v = VOL(ip);

  return v != 0 ? ufs2_readlink(v, ip->inum, &ip->ufs2_info, buf, size) : -1;
}

static int ufs_readdir(struct inode *dp, uint off, uint *next, uint *inum,
                       char *name, int namesz, uchar *dtype __unused) {
  const struct ufs2_volume *v = VOL(dp);

  if (v == 0 || ufs2_readdir(v, dp->inum, off, inum, name, (uint)namesz) < 0)
    return -1;
  *next = off + 1;
  return 0;
}

static int ufs_isempty(struct inode *dp) {
  const struct ufs2_volume *v = VOL(dp);

  return v != 0 && ufs2_dir_empty(v, dp->inum) == 1;
}

static int ufs_setattr(struct inode *ip, const struct vattr *va) {
  struct fnu_timespec atime = ip->atime, mtime = ip->mtime;
  uint uid = ip->uid, gid = ip->gid, flags = ip->flags;
  ushort mode = ip->mode;

  if (va->va_mask & AT_MODE)
    ip->mode = va->va_mode & 07777;
  if (va->va_mask & AT_OWNER) {
    ip->uid = va->va_uid;
    ip->gid = va->va_gid;
  }
  if (va->va_mask & AT_FLAGS)
    ip->flags = va->va_flags;
  if (va->va_mask & AT_ATIME) {
    ip->atime = va->va_atime;
    ip->iflag &= ~IN_ACCESS;
  }
  if (va->va_mask & AT_MTIME) {
    ip->mtime = va->va_mtime;
    ip->iflag &= ~IN_UPDATE;
  }
  ip->iflag |= IN_CHANGE;
  if (ufs_store(ip) == 0)
    return 0;
  ip->mode = mode;
  ip->uid = uid;
  ip->gid = gid;
  ip->flags = flags;
  ip->atime = atime;
  ip->mtime = mtime;
  return FNU_ERR_IO;
}

// 512-byte blocks on the disk
static void ufs_getattr(struct inode *ip, struct fnu_stat *st) {
  const struct ufs2_volume *v = VOL(ip);

  if (v != 0 && v->block_size > st->blksize)
    st->blksize = v->block_size;
  st->blocks = (int64_t)ip->ufs2_info.blocks;
  st->gen = ip->ufs2_info.gen;
}

static int ufs_pathconf(struct inode *ip, int name, int64_t *value) {
  const struct ufs2_volume *v = VOL(ip);

  switch (name) {
  case FNU_PC_LINK_MAX:
    *value = UFS_LINK_MAX;
    return 0;
  case FNU_PC_NAME_MAX:
    *value = UFS2_MAX_NAME;
    return 0;
  case FNU_PC_2_SYMLINKS:
  case FNU_PC_TIMESTAMP_RESOLUTION:
    *value = 1;
    return 0;
  case FNU_PC_ALLOC_SIZE_MIN:
    *value = v != 0 ? (int64_t)v->fragment_size : BSIZE;
    return 0;
  case FNU_PC_FILESIZEBITS:
    *value = 64;
    return 0;
  default:
    return vop_pathconf_default(name, value);
  }
}

static const struct vnodeops ufs2_vnops = {
    .load = ufs_load,
    .update = ufs_update,
    .inactive = ufs_inactive,
    .truncate = ufs_truncate,
    .read = ufs_read,
    .write = ufs_write,
    .lookup = ufs_lookup,
    .create = ufs_create,
    .symlink = ufs_symlink,
    .link = ufs_link,
    .remove = ufs_remove,
    .rename = ufs_rename,
    .readlink = ufs_readlink,
    .readdir = ufs_readdir,
    .isempty = ufs_isempty,
    .setattr = ufs_setattr,
    .getattr = ufs_getattr,
    .pathconf = ufs_pathconf,
};

static void ufs_start(struct vfs_mount *m) {
  if (!m->readonly && ufs2_mount_rw(&m->ufs2, m->on) < 0)
    m->readonly = 1;
  if (m->readonly)
    ufs2_mount_ro(&m->ufs2, m->on);
}

static void ufs_unmount(const struct vfs_mount *m) { ufs2_unmount(&m->ufs2); }

static void ufs_sync(const struct vfs_mount *m) { ufs2_sync(&m->ufs2); }

static int ufs_mount_update(struct vfs_mount *m, int flags) {
  if (flags & FNU_MNT_RDONLY)
    return ufs2_downgrade(&m->ufs2);
  if (ufs2_upgrade(&m->ufs2, m->on) < 0)
    return -1;
  fs_reload_dev(m->dev);
  return 0;
}

static int ufs_statfs(const struct vfs_mount *m, struct fnu_fsstat *st) {
  return ufs2_statfs(&m->ufs2, st);
}

const struct vfsops ufs2_vfsops = {
    .name = "ufs2",
    .vnops = &ufs2_vnops,
    .start = ufs_start,
    .unmount = ufs_unmount,
    .sync = ufs_sync,
    .update = ufs_mount_update,
    .statfs = ufs_statfs,
};
