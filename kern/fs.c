#include "fs.h"
#include "kclock.h"
#include "ufs2.h"
#include "blockdev.h"
#include "buf.h"
#include "defs.h"
#include "mmu.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "malloc.h"
#include "param.h"
#include "uvm.h"
#include "proc.h"
#include "uio.h"
#include "unveil.h"
#include "vfs.h"

uint rootdev;

int inode_access(struct inode *ip, uint uid, int mask) {
  uint bits;

  if (ip == 0 || ip->type == 0)
    return 0;
  if ((mask & 2) && ip->type != FNU_TYPE_DEVICE && vfs_readonly(ip->dev))
    return 0;
  if (uid == 0) {
    if ((mask & 1) && ip->type != FNU_TYPE_DIR && (ip->mode & 0111) == 0)
      return 0;
    return 1;
  }
  if (uid == ip->uid)
    bits = (ip->mode >> 6) & 7;
  else if (groupmember(ip->gid))
    bits = (ip->mode >> 3) & 7;
  else
    bits = ip->mode & 7;
  return (bits & (uint)mask) == (uint)mask;
}

// THE IN-CORE INODES
static struct {
  struct mutex lock;
  struct inode **hash;
  uint64_t mask;
  struct inode *spare;
  int numvnodes;
} icache;

#define IHASH(dev, inum)                                                         (((uint64_t)(dev) * 0x9e3779b97f4a7c15ull ^ (inum)) & icache.mask)

// icache.lock held
static void ihash_remove(struct inode *ip) {
  struct inode **pp;

  for (pp = &icache.hash[IHASH(ip->dev, ip->inum)]; *pp != 0; pp = &(*pp)->i_hash)
    if (*pp == ip) {
      *pp = ip->i_hash;
      break;
    }
  ip->i_hash = 0;
}

static void ihash_insert(struct inode *ip) {
  struct inode **b = &icache.hash[IHASH(ip->dev, ip->inum)];

  ip->i_hash = *b;
  *b = ip;
}

static struct sleeplock renamelock;

void fs_rename_lock(void) { acquiresleep(&renamelock); }
void fs_rename_unlock(void) { releasesleep(&renamelock); }

void iinit(void) {
  uint64_t n = 16;

  mtx_init(&icache.lock, "icache");
  initsleeplock(&renamelock, "rename");
  while (n < (uint64_t)maxvnodes)
    n <<= 1;
  icache.hash = malloc(n * sizeof(*icache.hash), M_DEVBUF, M_WAITOK | M_ZERO);
  icache.mask = n - 1;
  sysctl_add_int("vfs.numvnodes", &icache.numvnodes, FNU_CTLF_STDIO, 0, 0);
}

int fs_mountroot(uint dev, const char *type, int readonly, int nosuid) {
  struct vfs_mount m;

  memset(&m, 0, sizeof(m));
  if (storage_recognise(dev, type, &m) < 0)
    return -1;
  m.dev = dev;
  m.readonly = readonly;
  m.nosuid = nosuid;
  strlcpy(m.from, blockdev_driver_name(dev), VFS_NAMELEN);
  strlcpy(m.on, "/", VFS_NAMELEN);
  if (vfs_mount_add(&m) < 0)
    return -1;
  rootdev = dev;
  printf("FNU Root: %s on %s (%d,%d)%s\n", m.type, m.from, major(dev),
          minor(dev), vfs_readonly(dev) ? ", read-only" : "");
  if (strcmp(m.type, "ufs2") == 0)
    clock_base(ufs2_time(dev));
  return 0;
}

int fs_dev_busy(uint dev) {
  struct inode *ip;
  uint64_t b;
  int busy = 0;

  mtx_enter(&icache.lock);
  for (b = 0; b <= icache.mask && !busy; b++)
    for (ip = icache.hash[b]; ip != 0; ip = ip->i_hash)
      if (ip->dev == dev) {
        busy = 1;
        break;
      }
  mtx_leave(&icache.lock);
  return busy;
}

void fs_forget_dev(uint dev) { bio_forget_dev(dev); }

int fs_dev_writers(uint dev) {
  struct inode *ip;
  uint64_t b;
  int busy = 0;

  mtx_enter(&icache.lock);
  for (b = 0; b <= icache.mask && !busy; b++)
    for (ip = icache.hash[b]; ip != 0; ip = ip->i_hash)
      if (ip->dev == dev && ip->ref > 0 && ip->writecount > 0) {
        busy = 1;
        break;
      }
  mtx_leave(&icache.lock);
  return busy || vm_dev_write_mapped(dev);
}

// inodes get read again at their next lock
void fs_reload_dev(uint dev) {
  struct inode *ip;
  uint64_t b;

  mtx_enter(&icache.lock);
  for (b = 0; b <= icache.mask; b++)
    for (ip = icache.hash[b]; ip != 0; ip = ip->i_hash)
      if (ip->dev == dev)
        ip->valid = 0;
  mtx_leave(&icache.lock);
}

// INODES

void itimes(struct inode *ip) {
  struct fnu_timespec now;

  if ((ip->iflag & (IN_ACCESS | IN_UPDATE | IN_CHANGE)) == 0)
    return;
  time_now(&now);
  if (ip->iflag & IN_ACCESS)
    ip->atime = now;
  if (ip->iflag & IN_UPDATE)
    ip->mtime = now;
  if (ip->iflag & IN_CHANGE)
    ip->ctime = now;
  ip->iflag = (ip->iflag & ~(IN_ACCESS | IN_UPDATE | IN_CHANGE)) | IN_MODIFIED;
}

void iupdate(struct inode *ip) {
  if (ip->i_op->update != 0)
    ip->i_op->update(ip);
}

struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *fresh = 0;

  for (;;) {
    mtx_enter(&icache.lock);
    for (ip = icache.hash[IHASH(dev, inum)]; ip != 0; ip = ip->i_hash)
      if (ip->dev == dev && ip->inum == inum) {
        ip->ref++;
        break;
      }
    if (ip == 0 && (ip = icache.spare) != 0)
      icache.spare = ip->i_free;
    if (ip == 0 && fresh != 0) {
      ip = fresh;
      fresh = 0;
    }
    if (ip == 0) {
      if (icache.numvnodes >= maxvnodes) {
        mtx_leave(&icache.lock);
        return 0;
      }
      icache.numvnodes++;
      mtx_leave(&icache.lock);
      if ((fresh = malloc(sizeof(*fresh), M_VNODE, M_WAITOK | M_ZERO)) == 0) {
        mtx_enter(&icache.lock);
        icache.numvnodes--;
        mtx_leave(&icache.lock);
        return 0;
      }
      initsleeplock(&fresh->lock, "inode");
      continue;
    }
    if (ip->ref == 0) {
      memset(&ip->type, 0,
             sizeof(*ip) - __builtin_offsetof(struct inode, type));
      ip->dev = dev;
      ip->inum = inum;
      ip->ref = 1;
      ip->valid = 0;
      ip->i_op = vfs_vnops(dev);
      ihash_insert(ip);
    }
    if (fresh != 0) {
      fresh->i_free = icache.spare;
      icache.spare = fresh;
    }
    mtx_leave(&icache.lock);
    return ip;
  }
}

void iupdate_inum(struct inode *ip, uint inum) {
  mtx_enter(&icache.lock);
  ihash_remove(ip);
  ip->inum = inum;
  ihash_insert(ip);
  mtx_leave(&icache.lock);
}

struct inode *idup(struct inode *ip) {
  mtx_enter(&icache.lock);
  ip->ref++;
  mtx_leave(&icache.lock);
  return ip;
}

// lock and read if needed
void ilock(struct inode *ip) {
  if (ip == 0 || ip->ref < 1)
    panic("ilock");
  acquiresleep(&ip->lock);
  if (ip->valid == 0)
    ip->i_op->load(ip);
}

void iunlock(struct inode *ip) {
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) {
    panic("iunlock");
  }

  releasesleep(&ip->lock);
}

void iput(struct inode *ip) {
  if (ip->valid && (ip->iflag & (IN_ACCESS | IN_UPDATE | IN_CHANGE)) != 0 &&
      ip->type != 0 && !vfs_readonly(ip->dev)) {
    mtx_enter(&icache.lock);
    if (ip->ref == 1 && ip->nlink > 0) {
      mtx_leave(&icache.lock);
      ilock(ip);
      iupdate(ip);
      iunlock(ip);
    } else {
      mtx_leave(&icache.lock);
    }
  }
  mtx_enter(&icache.lock);
  if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
    mtx_leave(&icache.lock);
    ilock(ip);
    if (ip->i_op->inactive != 0)
      ip->i_op->inactive(ip);
    ip->type = 0;
    ip->valid = 0;
    iunlock(ip);
    mtx_enter(&icache.lock);
  }
  if (--ip->ref == 0) {
    ihash_remove(ip);
    ip->valid = 0;
    ip->i_free = icache.spare;
    icache.spare = ip;
  }
  mtx_leave(&icache.lock);
}

void iunlockput(struct inode *ip) {
  iunlock(ip);
  iput(ip);
}

void stati(struct inode *ip, struct fnu_stat *st) {
  memset(st, 0, sizeof(*st));
  st->device = ip->dev;
  st->type = ip->type;
  if (ip->type == FNU_TYPE_DISK)
    st->devclass = FNU_DEVCLASS_DISK;
  else if (ip->type == FNU_TYPE_DEVICE && ip->major >= 0 && ip->major < NDEV)
    st->devclass = (uint8)devsw[ip->major].class;
  st->size = ip->size;
  st->mode = ip->mode;
  st->flags = ip->flags;
  st->uid = ip->uid;
  st->gid = ip->gid;
  st->nlink = (uint32)ip->nlink;
  if (ip->type == FNU_TYPE_DEVICE || ip->type == FNU_TYPE_DISK) {
    st->major = ip->major;
    st->minor = ip->minor;
  }
  st->atime = ip->atime;
  st->mtime = ip->mtime;
  st->ctime = ip->ctime;
  st->birthtime = ip->birthtime;
  st->ino = ip->inum;
  st->blksize = PGSIZE;
  if (ip->i_op->getattr != 0)
    ip->i_op->getattr(ip, st);
}

int readi_uio(struct inode *ip, struct uio *uio) {
  if (ip->type == FNU_TYPE_DIR)
    return -FNU_ERR_ISDIR;
  if (ip->i_op->read == 0)
    return -FNU_ERR_IO;
  return ip->i_op->read(ip, uio);
}

int readi(struct inode *ip, char *dst, uint64_t off, uint n) {
  struct kiovec iov;
  struct uio uio;

  uio_prepare(&uio, &iov, dst, n, off, UIO_SYSSPACE, UIO_READ);
  return readi_uio(ip, &uio);
}

int writei_uio(struct inode *ip, struct uio *uio) {
  uint64_t old = ip->size;
  int r;

  if (ip->type == FNU_TYPE_DIR)
    return -FNU_ERR_ISDIR;
  if (ip->i_op->write == 0)
    return -FNU_ERR_IO;
  if ((r = ip->i_op->write(ip, uio)) > 0)
    knote_inode(ip, FNU_NOTE_WRITE | (ip->size > old ? FNU_NOTE_EXTEND : 0));
  return r;
}

int writei(struct inode *ip, char *src, uint64_t off, uint n) {
  struct kiovec iov;
  struct uio uio;

  uio_prepare(&uio, &iov, src, n, off, UIO_SYSSPACE, UIO_WRITE);
  return writei_uio(ip, &uio);
}

// DIRECTORIES

struct inode *dirlookup(struct inode *dp, char *name) {
  if (dp->type != FNU_TYPE_DIR)
    panic("dirlookup not DIR");
  return dp->i_op->lookup != 0 ? dp->i_op->lookup(dp, name) : 0;
}

int name_is_dot(const char *n) {
  return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

// PATHS

static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/') {
    path++;
  }
  if (*path == 0) {
    return 0;
  }
  s = path;
  while (*path != '/' && *path != 0) {
    path++;
  }
  len = path - s;
  if (len > FNU_NAMELEN_MAX)
    return 0;
  memmove(name, s, len);
  name[len] = 0;
  while (*path == '/') {
    path++;
  }
  return path;
}

static int dir_name_of(struct inode *dp, uint child, char *name, uint size) {
  uint off = 0, next, inum;
  uchar dtype;

  while (dirread(dp, off, &next, &inum, name, (int)size, &dtype) == 0) {
    if (inum == child && !name_is_dot(name))
      return 0;
    off = next;
  }
  return -1;
}

static int path_prepend(char *buf, uint *pos, const char *s) {
  uint n = strlen(s);

  if (*pos < n + 1)
    return -1;
  *pos -= n;
  memmove(buf + *pos, s, n);
  buf[--(*pos)] = '/';
  return 0;
}

static struct inode *mount_leave(struct inode *ip);

int inode_path(struct inode *start, char *buf, uint size) {
  struct inode *ip, *parent;
  char name[NAMEBUFSZ];
  uint pos;
  int depth;

  if (size < 2)
    return -1;
  pos = size - 1;
  buf[pos] = 0;

  ip = idup(start);
  for (depth = 0; depth < MAXPATHDEPTH; depth++) {
    ip = mount_leave(ip);
    ilock(ip);
    if (ip->type != FNU_TYPE_DIR) {
      iunlockput(ip);
      return -1;
    }
    parent = dirlookup(ip, "..");
    if (parent == 0) {
      iunlock(ip);
      break;
    }
    if (parent->dev == ip->dev && parent->inum == ip->inum) {
      iunlock(ip);
      iput(parent);
      break;
    }
    iunlock(ip);
    ilock(parent);
    if (dir_name_of(parent, ip->inum, name, sizeof(name)) < 0) {
      iunlockput(parent);
      iput(ip);
      return -1;
    }
    iunlock(parent);
    iput(ip);
    ip = parent;
    if (path_prepend(buf, &pos, name) < 0) {
      iput(ip);
      return -1;
    }
  }
  if (depth == MAXPATHDEPTH) {
    iput(ip);
    return -1;
  }
  iput(ip);

  if (buf[pos] == 0) {
    buf[--pos] = '/';
  }
  memmove(buf, buf + pos, size - pos);
  return 0;
}

struct inode *mount_enter(struct inode *ip) {
  const struct vfs_mount *m;
  struct inode *root;

  while (ip != 0 && (m = vfs_covering(ip->dev, ip->inum)) != 0) {
    if ((root = iget(m->dev, m->root_inum)) == 0)
      break;
    iput(ip);
    ip = root;
  }
  return ip;
}

// and back out on ..
static struct inode *mount_leave(struct inode *ip) {
  const struct vfs_mount *m;
  struct inode *cover;

  while (ip != 0 && (m = vfs_rooted_at(ip->dev, ip->inum)) != 0) {
    if ((cover = iget(m->cover_dev, m->cover_inum)) == 0)
      break;
    iput(ip);
    ip = cover;
  }
  return ip;
}

// what a symlink stands for
int readlinki(struct inode *ip, char *buf, int size) {
  if (ip->type != FNU_TYPE_LINK || ip->i_op->readlink == 0)
    return -1;
  return ip->i_op->readlink(ip, buf, size);
}

struct inode *inode_dotdot(struct inode *ip) {
  struct inode *parent;

  ip = mount_leave(idup(ip));
  ilock(ip);
  parent = ip->type == FNU_TYPE_DIR ? dirlookup(ip, "..") : 0;
  iunlockput(ip);
  return parent;
}

static int lookup_parent(struct lookup_unveil *lu, struct inode *dp, char *name) {
  struct inode *ip;
  int error;

  if (lu->want == UV_NONE || !unveil_active(curproc->p_p))
    return 0;
  ilock(dp);
  ip = dirlookup(dp, name);
  iunlock(dp);
  ip = mount_enter(ip);
  error = unveil_final(lu, ip, dp, name);
  if (ip != 0)
    iput(ip);
  return error;
}

static int namex(struct inode *start, char *path, int nameiparent, char *name,
                 int nofollow_last, int want, struct inode **ipp) {
  struct inode *ip, *next, *parent, *last = 0;
  struct lookup_unveil lu;
  char scratch[MAXPATHLEN];
  int follows = 0, error;

  *ipp = 0;
  lu.want = want;
  if (*path == '/') {
    const struct vfs_mount *r = vfs_root();

    if (r == 0 || (ip = iget(r->dev, r->root_inum)) == 0)
      return FNU_ERR_NOTFOUND;
  } else {
    ip = start != 0 ? idup(start) : fd_cwd(curproc->p_p);
  }
  unveil_start(&lu, ip);
  unveil_component(&lu, ip, 0);

  while ((path = skipelem(path, name)) != 0) {
    if (last != 0) {
      iput(last);
      last = 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
      unveil_component(&lu, ip, 1);
      ip = mount_leave(ip);
    }
    ilock(ip);
    if (ip->type == 0) {
      iunlockput(ip); // unreadable, or its disk is gone
      return FNU_ERR_IO;
    }
    if (ip->type != FNU_TYPE_DIR) {
      iunlockput(ip);
      return FNU_ERR_NOTDIR;
    }
    if (!inode_access(ip, curproc->p_p->uid, 1)) {
      iunlockput(ip);
      return FNU_ERR_DENIED;
    }
    // . is the inode itself
    if (name[0] == '.' && name[1] == '\0' && !(nameiparent && *path == '\0')) {
      iunlock(ip);
      continue;
    }
    if (nameiparent && *path == '\0') {
      iunlock(ip);
      if ((error = lookup_parent(&lu, ip, name)) != 0) {
        iput(ip);
        return error;
      }
      *ipp = ip;
      return 0;
    }

    if ((next = dirlookup(ip, name)) == 0) {
      iunlockput(ip);
      return FNU_ERR_NOTFOUND;
    }
    iunlock(ip);
    parent = ip;
    ip = mount_enter(next);

    ilock(ip);
    if (ip->type == FNU_TYPE_LINK && !(nofollow_last && *path == '\0')) {
      char target[MAXPATHLEN];
      int tlen, rest = strlen(path);

      if (++follows > MAXSYMLINKS || (tlen = readlinki(ip, target, sizeof(target))) < 0 ||
          tlen + 1 + rest >= (int)sizeof(scratch)) {
        iunlockput(ip);
        iput(parent);
        return follows > MAXSYMLINKS ? FNU_ERR_LOOP : FNU_ERR_NAMETOOLONG;
      }
      iunlockput(ip);
      if (rest > 0) {
        memmove(scratch + tlen + 1, path, rest);
        scratch[tlen] = '/';
        scratch[tlen + 1 + rest] = 0;
      } else
        scratch[tlen] = 0;
      memmove(scratch, target, tlen);
      path = scratch;
      if (*path == '/') {
        const struct vfs_mount *r = vfs_root();

        iput(parent);
        if (r == 0 || (ip = iget(r->dev, r->root_inum)) == 0)
          return FNU_ERR_NOTFOUND;
        unveil_component(&lu, ip, 0);
      } else
        ip = parent;
      continue;
    }
    if (ip->type == FNU_TYPE_DIR && *path != '\0') {
      iunlock(ip);
      unveil_component(&lu, ip, 0);
    } else
      iunlock(ip);
    last = parent;
  }
  if (nameiparent) {
    if (last != 0)
      iput(last);
    iput(ip);
    return FNU_ERR_ISDIR;
  }
  error = unveil_final(&lu, ip, last, name);
  if (last != 0)
    iput(last);
  if (error != 0) {
    iput(ip);
    return error;
  }
  *ipp = ip;
  return 0;
}

int namei_nofollow_at(struct inode *start, char *path, int want, struct inode **ipp) {
  char name[NAMEBUFSZ];

  return namex(start, path, 0, name, 1, want, ipp);
}

int namei_at(struct inode *start, char *path, int want, struct inode **ipp) {
  char name[NAMEBUFSZ];

  return namex(start, path, 0, name, 0, want, ipp);
}

int nameiparent_at(struct inode *start, char *path, char *name, int want,
                   struct inode **ipp) {
  return namex(start, path, 1, name, 0, want, ipp);
}

int namei(char *path, struct inode **ipp) {
  return namei_at(0, path, UV_NONE, ipp);
}

int dirread(struct inode *dp, uint off, uint *next, uint *inum, char *name,
            int namesz, uchar *dtype) {
  if (dp == 0 || dp->type != FNU_TYPE_DIR || namesz < 2 || dp->i_op->readdir == 0)
    return -1;
  *dtype = FNU_TYPE_NONE;
  return dp->i_op->readdir(dp, off, next, inum, name, namesz, dtype);
}

int vop_pathconf_default(int name, int64_t *value) {
  switch (name) {
  case FNU_PC_PATH_MAX:
  case FNU_PC_SYMLINK_MAX:
    *value = MAXPATHLEN;
    return 0;
  case FNU_PC_PIPE_BUF:
    *value = pipe_bufsize();
    return 0;
  case FNU_PC_CHOWN_RESTRICTED:
  case FNU_PC_NO_TRUNC:
    *value = 1;
    return 0;
  default:
    return FNU_ERR_INVALID;
  }
}

static void dead_load(struct inode *ip) {
  ip->type = 0;
  ip->size = 0;
  ip->nlink = 1;
  ip->flags = 0;
  ip->valid = 1;
}

const struct vnodeops dead_vnops = {
    .load = dead_load,
};
