// THE MOUNT SYSTEM CALLS

#include "blockdev.h"
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "fs.h"
#include "inc/abi.h"
#include "inc/open.h"
#include "inc/stat.h"
#include "param.h"
#include "pathname.h"
#include "proc.h"
#include "unveil.h"
#include "vfs.h"

static int device_named(char *path, uint *dev) {
  struct inode *ip;

  if (namei_at(0, path, UV_READ, &ip) != 0)
    return -1;
  ilock(ip);
  if (ip->type != FNU_TYPE_DISK) {
    iunlockput(ip);
    return -1;
  }
  *dev = makedev((uint)ip->major, (uint)ip->minor);
  iunlockput(ip);
  return 0;
}

static int directory_named(char *path, uint *dev, uint *inum) {
  struct inode *ip;

  if (namei_at(0, path, UV_READ, &ip) != 0)
    return -1;
  ilock(ip);
  if (ip->type != FNU_TYPE_DIR) {
    iunlockput(ip);
    return -1;
  }
  *dev = ip->dev;
  *inum = ip->inum;
  iunlockput(ip);
  return 0;
}

int sys_mount(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_mount_req req;
  struct vfs_mount m;
  char from[FNU_MNT_NAMELEN + 1], on[FNU_MNT_NAMELEN + 1];
  char type[FNU_MNT_TYPELEN + 1];
  uint dev, cover_dev, cover_inum;

  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if (copyin(p->p_p->pgdir, &req, (uintptr_t)uap[0], sizeof(req)) < 0)
    return FNU_ERR_FAULT;
  strlcpy(from, req.from, sizeof(from));
  strlcpy(on, req.on, sizeof(on));
  strlcpy(type, req.type, sizeof(type));

  if (req.flags & FNU_MNT_UPDATE) {
    struct inode *ip;
    const struct vfs_mount *e;
    int error;

    if ((error = namei_at(0, on, UV_READ, &ip)) != 0)
      return error;
    ilock(ip);
    e = vfs_on_dev(ip->dev);
    dev = ip->dev;
    error = e == 0 || e->root_inum != ip->inum ? FNU_ERR_INVALID : 0;
    iunlockput(ip);
    return error != 0 ? error : vfs_update(dev, (int)req.flags);
  }
  if (device_named(from, &dev) < 0) {
    return FNU_ERR_NOTDISK;
  }
  if (directory_named(on, &cover_dev, &cover_inum) < 0) {
    return FNU_ERR_NOTDIR;
  }
  if (cover_inum == 0 || vfs_covering(cover_dev, cover_inum) != 0 ||
      vfs_on_dev(dev) != 0) {
    return FNU_ERR_BUSY;
  }

  memset(&m, 0, sizeof(m));
  if (storage_recognise(dev, type, &m) < 0) {
    return FNU_ERR_INVALID;
  }
  m.dev = dev;
  m.cover_dev = cover_dev;
  m.cover_inum = cover_inum;
  if (req.flags & FNU_MNT_RDONLY)
    m.readonly = 1;
  if (req.flags & FNU_MNT_NOSUID)
    m.nosuid = 1;
  strlcpy(m.from, from, VFS_NAMELEN);
  strlcpy(m.on, on, VFS_NAMELEN);
  if (vfs_mount_add(&m) < 0) {
    return FNU_ERR_BUSY;
  }
  return 0;
}

int sys_unmount(struct proc *p, const register_t *uap, register_t *retval __unused) {
  const struct vfs_mount *m;
  struct pathname on;
  struct inode *ip;
  uint dev, cover_dev, cover_inum;
  int flags = (int)uap[1], error;

  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if ((error = pn_get((uintptr_t)uap[0], &on)) != 0)
    return error;

  error = namei_at(0, on.pn_buf, UV_READ, &ip);
  pn_free(&on);
  if (error != 0)
    return error;
  ilock(ip);
  dev = ip->dev;
  m = vfs_rooted_at(ip->dev, ip->inum);
  iunlockput(ip);
  if (m == 0) {
    return FNU_ERR_INVALID;
  }
  cover_dev = m->cover_dev;
  cover_inum = m->cover_inum;
  if (!(flags & FNU_MNT_FORCE) && fs_dev_busy(dev)) {
    return FNU_ERR_BUSY;
  }
  if (vfs_unmount_at(cover_dev, cover_inum) < 0) {
    return FNU_ERR_INVALID;
  }
  fs_forget_dev(dev);
  return 0;
}

int sys_flush(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct file *f;
  int fd = (int)uap[0];

  if (fd == FNU_FLUSH_ALL) {
    vfs_sync();
    return 0;
  }
  if ((f = fd_getfile(p->p_p, fd)) == 0)
    return FNU_ERR_BADHANDLE;
  if (f->type != FD_INODE || f->ip == 0) {
    FRELE(f);
    return FNU_ERR_INVALID;
  }
  vfs_sync_dev(file_device(f));
  FRELE(f);
  return 0;
}

int sys_fsstat(struct proc *p, const register_t *uap, register_t *retval) {
  struct fnu_fsstat one;
  const struct vfs_mount *m;
  uint64_t dst;
  int capacity, i, n = 0;

  dst = uap[0];
  capacity = (int)uap[1];
  if (capacity < 0)
    return FNU_ERR_INVALID;

  for (i = 0; i < vfs_slots(); i++) {
    if ((m = vfs_nth(i)) == 0)
      continue;
    if (n >= capacity) {
      n++;
      continue;
    }
    memset(&one, 0, sizeof(one));
    strlcpy(one.from, (char *)m->from, FNU_MNT_NAMELEN);
    strlcpy(one.on, (char *)m->on, FNU_MNT_NAMELEN);
    strlcpy(one.type, (char *)m->type, FNU_MNT_TYPELEN);
    one.flags = (m->readonly ? FNU_MNT_RDONLY : 0) |
                (m->nosuid ? FNU_MNT_NOSUID : 0) | FNU_MNT_LOCAL;
    one.dev = m->dev;
    if (m->op->statfs != 0)
      m->op->statfs(m, &one);
    if (copyout(p->p_p->pgdir, (uintptr_t)(dst + n * sizeof(one)), &one,
                sizeof(one)) < 0)
      return FNU_ERR_FAULT;
    n++;
  }
  *retval = capacity == 0 || n < capacity ? n : capacity;
  return 0;
}
