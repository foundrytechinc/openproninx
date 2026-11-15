#include "defs.h"
#include "fs.h"
#include "malloc.h"
#include "file.h"
#include "vfs.h"
#include "inc/abi.h"
#include "inc/dir.h"
#include "inc/open.h"
#include "inc/stat.h"
#include "inc/syscall.h"
#include "param.h"
#include "proc.h"
#include "uio.h"
#include "pathname.h"
#include "devfs.h"
#include "unveil.h"

// A CALL ON A PATH
typedef int (*pathcall)(struct proc *p, const register_t *uap,
                        register_t *retval, char *path);
typedef int (*pathcall2)(struct proc *p, const register_t *uap,
                         register_t *retval, char *a, char *b);

static int withpath(struct proc *p, const register_t *uap, register_t *retval,
                    int n, pathcall fn) {
  struct pathname pn;
  int error;

  if ((error = pn_get((uintptr_t)uap[n], &pn)) != 0)
    return error;
  error = fn(p, uap, retval, pn.pn_buf);
  pn_free(&pn);
  return error;
}

static int withpath2(struct proc *p, const register_t *uap, register_t *retval,
                     int na, int nb, pathcall2 fn) {
  struct pathname a, b;
  int error;

  if ((error = pn_get((uintptr_t)uap[na], &a)) != 0)
    return error;
  if ((error = pn_get((uintptr_t)uap[nb], &b)) == 0) {
    error = fn(p, uap, retval, a.pn_buf, b.pn_buf);
    pn_free(&b);
  }
  pn_free(&a);
  return error;
}

// A CALL ON A DESCRIPTOR
typedef int (*filecall)(struct proc *p, const register_t *uap,
                        register_t *retval, struct file *f, int fd);

static int withfile(struct proc *p, const register_t *uap, register_t *retval,
                    filecall fn) {
  struct file *f;
  int fd = (int)uap[0], error;

  if ((f = fd_getfile(p->p_p, fd)) == 0)
    return FNU_ERR_BADHANDLE;
  error = fn(p, uap, retval, f, fd);
  FRELE(f);
  return error;
}

static int namei_from(int dirfd, char *path, int nofollow, int want,
                      struct inode **ip) {
  struct inode *start;
  int error;

  if ((error = at_dir(dirfd, &start)) != 0)
    return error;
  error = nofollow ? namei_nofollow_at(start, path, want, ip)
                   : namei_at(start, path, want, ip);
  if (start != 0)
    iput(start);
  return error;
}

static int parent_from(int dirfd, char *path, char *name, int want,
                       struct inode **dp) {
  struct inode *start;
  int error;

  if ((error = at_dir(dirfd, &start)) != 0)
    return error;
  error = nameiparent_at(start, path, name, want, dp);
  if (start != 0)
    iput(start);
  return error;
}

// A CALL BY NAME
static int name_target(int dirfd, uintptr_t upath, int nofollow, int want,
                       struct inode **ipp) {
  struct pathname pn;
  struct file *f;
  int error;

  if (upath == 0) {
    if ((f = fd_getfile(curproc->p_p, dirfd)) == 0)
      return FNU_ERR_BADHANDLE;
    error = f->ip != 0 ? 0 : FNU_ERR_UNSUPPORTED;
    if (error == 0)
      *ipp = idup(f->ip);
    FRELE(f);
    return error;
  }
  if ((error = pn_get(upath, &pn)) != 0)
    return error;
  error = namei_from(dirfd, pn.pn_buf, nofollow, want, ipp);
  pn_free(&pn);
  return error;
}

static int ioctl_file(struct proc *p, const register_t *uap, register_t *retval __unused, struct file *f, int fd) {
  uint64_t cmd, arg_val;

  cmd = uap[1];
  arg_val = uap[2];

  // sys_ioctl: what every descriptor answers
  switch (cmd) {
  case FNU_IOCLOSEEXEC:
    return fd_setflags(p->p_p, fd, FNU_FD_CLOSEEXEC, 0) < 0 ? FNU_ERR_BADHANDLE : 0;
  case FNU_IOKEEPEXEC:
    return fd_setflags(p->p_p, fd, 0, FNU_FD_CLOSEEXEC) < 0 ? FNU_ERR_BADHANDLE : 0;
  case FNU_IONOWAIT: {
    int on;

    if (copyin(p->p_p->pgdir, &on, arg_val, sizeof(on)) < 0)
      return FNU_ERR_FAULT;
    if (on)
      f->flags |= FNU_OPEN_NOWAIT;
    else
      f->flags &= ~FNU_OPEN_NOWAIT;
    return 0;
  }
  case FNU_IOWAITING: {
    int n = -1;

    if (f->type == FD_PIPE)
      n = pipe_nread(f->pipe);
    else if (f->type == FD_INODE && f->ip->type == FNU_TYPE_FILE) {
      ilock(f->ip);
      n = f->off >= f->ip->size ? 0 : (int)(f->ip->size - f->off);
      iunlock(f->ip);
    }
    if (n < 0)
      break;
    return copyout(p->p_p->pgdir, arg_val, &n, sizeof(n)) < 0 ? FNU_ERR_FAULT : 0;
  }
  }

  if (f->type == FD_INODE && f->ip->type == FNU_TYPE_DEVICE) {
    if (f->ip->major < 0 || f->ip->major >= NDEV || devsw[f->ip->major].ioctl == 0)
      return FNU_ERR_BADREQUEST;
    return devsw[f->ip->major].ioctl(f->ip, cmd, arg_val);
  }
  if (f->type == FD_INODE && f->ip->type == FNU_TYPE_DISK) {
    uint64_t sectors = blockdev_device_size(makedev(f->ip->major, f->ip->minor));
    uint32_t sector = FNU_BLOCKDEV_SECTOR_SIZE;

    if (cmd == FNU_DISKSIZE) {
      sectors *= FNU_BLOCKDEV_SECTOR_SIZE;
      return copyout(p->p_p->pgdir, arg_val, &sectors, sizeof(sectors)) < 0 ? FNU_ERR_FAULT : 0;
    }
    if (cmd == FNU_DISKSECTOR)
      return copyout(p->p_p->pgdir, arg_val, &sector, sizeof(sector)) < 0 ? FNU_ERR_FAULT : 0;
  }

  return FNU_ERR_BADREQUEST;
}

int sys_ioctl(struct proc *p, const register_t *uap, register_t *retval) {
  struct file *f;
  int fd, error;

  if ((f = fd_getfile(p->p_p, fd = (int)uap[0])) == 0)
    return FNU_ERR_BADHANDLE;
  error = ioctl_file(p, uap, retval, f, fd);
  FRELE(f);
  return error;
}

static int lseek_file(struct proc *p __unused, const register_t *uap, register_t *retval, struct file *f, int fd __unused) {
  uint64_t raw;
  int64_t off;
  int whence;

  raw = uap[1];
  whence = (int)uap[2];
  off = (int64_t)raw;
  if (f->type != FD_INODE)
    return FNU_ERR_NOTSEEKABLE;

  ilock(f->ip);
  switch (whence) {
  case FNU_SEEK_START: break;
  case FNU_SEEK_HERE: off += (int64_t)f->off; break;
  case FNU_SEEK_TAIL: off += (int64_t)f->ip->size; break;
  default:
    iunlock(f->ip);
    return FNU_ERR_INVALID;
  }
  iunlock(f->ip);
  if (off < 0)
    return FNU_ERR_INVALID;
  f->off = (uint64_t)off;
  *retval = off;
  return 0;
}

int sys_seek(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, lseek_file);
}

static int frozen(struct inode *ip) {
  return (ip->flags & (FNU_IMMUTABLE | FNU_APPEND)) != 0;
}

// who may truncate
static int truncate_inode(struct inode *ip, int64_t length, int ask) {
  uint64_t old;
  int error;

  if (length < 0 || ip->type != FNU_TYPE_FILE) {
    error = length < 0 ? FNU_ERR_INVALID : (ip->type == FNU_TYPE_DIR ? FNU_ERR_ISDIR : FNU_ERR_INVALID);
    iput(ip);
    return error;
  }
  ilock(ip);
  if (vfs_readonly(ip->dev)) {
    iunlockput(ip);
    return FNU_ERR_READONLY;
  }
  if (ask && !inode_access(ip, curproc->p_p->uid, 2)) {
    iunlockput(ip);
    return FNU_ERR_DENIED;
  }
  if (frozen(ip)) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  if (vm_text_busy(ip)) {
    iunlockput(ip);
    return FNU_ERR_TEXTBUSY;
  }
  old = ip->size;
  error = ip->i_op->truncate != 0 ? ip->i_op->truncate(ip, (uint64_t)length)
                                  : FNU_ERR_UNSUPPORTED;
  if (error == 0)
    knote_inode(ip, FNU_NOTE_ATTRIB | ((uint64_t)length < old ? FNU_NOTE_TRUNCATE
                                                              : FNU_NOTE_EXTEND));
  iunlockput(ip);
  return error;
}

static int access_path(struct proc *p, const register_t *uap,
                       register_t *retval __unused, char *path) {
  struct inode *ip;
  int how = (int)uap[2], flags = (int)uap[3], ok = 1, error;
  uint uid;

  if ((how & ~(FNU_ACCESS_READ | FNU_ACCESS_WRITE | FNU_ACCESS_EXEC)) ||
      (flags & ~FNU_ACCESS_REAL))
    return FNU_ERR_INVALID;
  uid = (flags & FNU_ACCESS_REAL) ? p->p_p->ruid : p->p_p->uid;
  if ((error = namei_from((int)uap[0], path, 0, UV_READ, &ip)) != 0)
    return error;
  ilock(ip);
  if (how & FNU_ACCESS_READ)
    ok = ok && inode_access(ip, uid, 4);
  if (how & FNU_ACCESS_WRITE)
    ok = ok && inode_access(ip, uid, 2);
  if (how & FNU_ACCESS_EXEC)
    ok = ok && inode_access(ip, uid, 1);
  iunlockput(ip);
  return ok ? 0 : FNU_ERR_DENIED;
}

int sys_access(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath(p, uap, retval, 1, access_path);
}

static struct inode *create(struct inode *start, char *path, short type,
                            int mode, const char *target, int *error);

#define OPEN_FLAGS                                                             \
  (FNU_OPEN_READWRITE | FNU_OPEN_NOWAIT | FNU_OPEN_APPEND | FNU_OPEN_CREATE |   \
   FNU_OPEN_TRUNCATE | FNU_OPEN_NEW | FNU_OPEN_CLOSEEXEC | FNU_OPEN_UNCACHED |  \
   FNU_OPEN_WRITETHROUGH | FNU_OPEN_NOLINK | FNU_OPEN_DIRONLY)
#define OPEN_THROUGH 0x40000000

struct inode *vn_mksock(char *path, int *error) {
  return create(0, path, FNU_TYPE_SOCKET, 0777, 0, error);
}

int sys_getattr(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_stat st;
  struct inode *ip;
  struct file *f;
  int flags = (int)uap[2], error;

  if (flags & ~FNU_PATH_NOFOLLOW)
    return FNU_ERR_INVALID;
  if (uap[1] == 0) {
    if ((f = fd_getfile(p->p_p, (int)uap[0])) == 0)
      return FNU_ERR_BADHANDLE;
    error = filestat(f, &st);
    FRELE(f);
  } else {
    if ((error = pledge_need(p, FNU_PLEDGE_RPATH, SYS_getattr)) != 0 ||
        (error = name_target((int)uap[0], (uintptr_t)uap[1],
                             flags & FNU_PATH_NOFOLLOW, UV_READ, &ip)) != 0)
      return error;
    ilock(ip);
    stati(ip, &st);
    iunlockput(ip);
  }
  if (error != 0)
    return error;
  return copyout(p->p_p->pgdir, (uintptr_t)uap[3], &st, sizeof(st)) < 0
             ? FNU_ERR_FAULT
             : 0;
}

static int readlink_path(struct proc *p, const register_t *uap,
                         register_t *retval, char *path) {
  char scratch[MAXPATHLEN];
  struct inode *ip;
  int size = (int)uap[3], n, error;

  if (size <= 0)
    return FNU_ERR_INVALID;
  if ((error = namei_from((int)uap[0], path, 1, UV_READ, &ip)) != 0)
    return error;
  ilock(ip);
  n = readlinki(ip, scratch, sizeof(scratch));
  iunlockput(ip);
  if (n < 0)
    return FNU_ERR_INVALID;
  if (n > size)
    n = size;
  if (copyout(p->p_p->pgdir, (uintptr_t)uap[2], scratch, (uint)n) < 0)
    return FNU_ERR_FAULT;
  *retval = n;
  return 0;
}

int sys_readlink(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath(p, uap, retval, 1, readlink_path);
}

int sys_ready(struct proc *p, const register_t *uap, register_t *retval) {
  struct fnu_ready stack[FNU_READY_MAX], *list = stack;
  struct fnu_timespec timeout;
  uint64_t ulist, uto, umask;
  uint64_t left = 0;
  uint64_t mask = 0;
  int forever = 1, n, count, error = 0;

  ulist = uap[0];
  n = (int)uap[1];
  uto = uap[2];
  umask = uap[3];
  if (umask != 0 && copyin(p->p_p->pgdir, (char *)&mask, (uintptr_t)umask,
                           sizeof(mask)) < 0)
    return FNU_ERR_FAULT;
  if (n < 0 || n > FNU_READY_LIMIT)
    return FNU_ERR_INVALID;
  if (n > FNU_READY_MAX &&
      (list = malloc((uint64_t)n * sizeof(*list), M_TEMP,
                     M_WAITOK | M_CANFAIL)) == 0)
    return FNU_ERR_NOMEMORY;
  if (n > 0 && copyin(p->p_p->pgdir, (char *)list, (uintptr_t)ulist,
                      (size_t)n * sizeof(list[0])) < 0) {
    error = FNU_ERR_FAULT;
    goto out;
  }
  if (uto != 0) {
    if (copyin(p->p_p->pgdir, (char *)&timeout, (uintptr_t)uto,
               sizeof(timeout)) < 0) {
      error = FNU_ERR_FAULT;
      goto out;
    }
    if (timeout.sec < 0 || timeout.nsec < 0 || timeout.nsec >= 1000000000) {
      error = FNU_ERR_INVALID;
      goto out;
    }
    forever = (uint64_t)timeout.sec > ~(uint64_t)0 / HZ - 1;
    left = (uint64_t)timeout.sec * HZ +
           (uint64_t)(timeout.nsec + (1000000000 / HZ) - 1) / (1000000000 / HZ);
  }

  if (umask != 0) {
    p->sig_oldmask = p->sig_mask;
    p->sig_suspended = 1;
    p->sig_mask = mask & ~FNU_SIG_UNCATCHABLE;
  }
  count = fnu_ready_wait(list, n, left, forever);
  p->sig_restart = 0;
  if (count < 0) {
    error = -count;
    goto out;
  }
  if (n > 0 && copyout(p->p_p->pgdir, (uintptr_t)ulist, list,
                       (size_t)n * sizeof(list[0])) < 0) {
    error = FNU_ERR_FAULT;
    goto out;
  }
  *retval = count;
out:
  if (list != stack)
    free(list, M_TEMP);
  return error;
}

static int getdirentries_file(struct proc *p, const register_t *uap, register_t *retval, struct file *f, int fd __unused) {
  uintptr_t buf = (uintptr_t)uap[1];
  uint64_t basep = uap[3];
  int count = (int)uap[2], got = 0;
  struct fnu_dirent de;
  uint inum, next;
  uchar dtype;

  if (f->type != FD_INODE || f->ip->type != FNU_TYPE_DIR)
    return FNU_ERR_INVALID;
  if (basep != 0 && copyout(p->p_p->pgdir, basep, &f->off, sizeof(f->off)) < 0)
    return FNU_ERR_FAULT;

  ilock(f->ip);
  if (!inode_access(f->ip, p->p_p->uid, 4)) {
    iunlock(f->ip);
    return FNU_ERR_DENIED;
  }

  while (dirread(f->ip, (uint)f->off, &next, &inum, de.name, sizeof(de.name),
                 &dtype) == 0) {
    int namelen = strlen(de.name);
    int reclen = (int)FNU_DIRENT_LEN(namelen);

    if (got + reclen > count)
      break;
    de.ino = inum;
    de.next = next;
    de.reclen = (uint16)reclen;
    de.type = dtype;
    de.pad0 = 0;
    de.namelen = (uint16)namelen;
    de.pad1 = 0;
    memset(de.name + namelen, 0, reclen - __offsetof(struct fnu_dirent, name) - namelen);
    if (copyout(p->p_p->pgdir, buf + (uintptr_t)got, &de, (size_t)reclen) < 0)
      break;
    f->off = next;
    got += reclen;
  }

  iunlock(f->ip);
  *retval = got;
  return 0;
}

int sys_readdir(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, getdirentries_file);
}

int sys_createmask(struct proc *p __unused, const register_t *uap, register_t *retval) {
  *retval = proc_umask((int)uap[0] & 0777);
  return 0;
}

static int dup_file(struct proc *p, const register_t *uap, register_t *retval,
                    struct file *f, int fd) {
  struct file *old;
  int to = (int)uap[1], flags = (int)uap[2], nfd;

  if (to < 0 || to >= fd_limit() ||
      (flags & ~(FNU_FD_CLOSEEXEC | FNU_DUP_EXACT)) != 0)
    return FNU_ERR_INVALID;
  if (flags & FNU_DUP_EXACT) {
    if (to == fd)
      return FNU_ERR_INVALID;
    filedup(f);
    // the hold becomes the table's
    if (fd_set(p->p_p, to, f, flags & FNU_FD_CLOSEEXEC, &old) < 0) {
      fileclose(f);
      return FNU_ERR_NOMEMORY;
    }
    if (old != 0)
      fileclose(old);
    *retval = to;
    return 0;
  }
  filedup(f);
  if ((nfd = fd_alloc_at(p->p_p, f, to, flags & FNU_FD_CLOSEEXEC)) < 0) {
    fileclose(f);
    return FNU_ERR_TOOMANYOPEN;
  }
  *retval = nfd;
  return 0;
}

int sys_dup(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, dup_file);
}

static int fdstate_get_file(struct proc *p, const register_t *uap, register_t *retval __unused, struct file *f, int fd) {
  struct fnu_fdstate st;
  uint64_t dst;

  dst = uap[1];
  memset(&st, 0, sizeof(st));
  st.fdflags = (fd_getflags(p->p_p, fd) & FNU_FD_CLOSEEXEC) ? FNU_FD_CLOSEEXEC : 0;
  st.fileflags = (uint32_t)f->flags;
  if (copyout(p->p_p->pgdir, (uintptr_t)dst, &st, sizeof(st)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

int sys_fdstate_get(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, fdstate_get_file);
}

#define FDSTATE_FILEFLAGS                                                      \
  (FNU_OPEN_NOWAIT | FNU_OPEN_APPEND | FNU_OPEN_UNCACHED | FNU_OPEN_WRITETHROUGH)

static int fdstate_set_file(struct proc *p, const register_t *uap, register_t *retval __unused, struct file *f, int fd) {
  struct fnu_fdstate st;
  uint64_t src;

  src = uap[1];
  if (copyin(p->p_p->pgdir, &st, (uintptr_t)src, sizeof(st)) < 0)
    return FNU_ERR_FAULT;
  if ((st.fdflags & ~FNU_FD_CLOSEEXEC) != 0)
    return FNU_ERR_INVALID;
  if (fd_setflags(p->p_p, fd, st.fdflags & FNU_FD_CLOSEEXEC,
                  ~st.fdflags & FNU_FD_CLOSEEXEC) < 0)
    return FNU_ERR_BADHANDLE;
  f->flags = (f->flags & ~FDSTATE_FILEFLAGS) |
             (int)(st.fileflags & (uint32_t)FDSTATE_FILEFLAGS);
  return 0;
}

int sys_fdstate_set(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, fdstate_set_file);
}

static int rw_file(struct proc *p, const register_t *uap, register_t *retval,
                   struct file *f, enum uio_rw rw) {
  struct kiovec aiov[UIO_SMALLIOV], *iov;
  int64_t at = (int64_t)uap[3];
  uint64_t off, *offp;
  struct uio uio;
  size_t total;
  int cnt = (int)uap[2], r;

  if (at != FNU_IO_HERE && at < 0)
    return FNU_ERR_INVALID;
  if (at != FNU_IO_HERE && f->type != FD_INODE)
    return FNU_ERR_NOTSEEKABLE;
  if ((r = iovec_copyin(uap[1], &iov, aiov, cnt, &total)) != 0) {
    iovec_free(iov, cnt);
    return r;
  }
  off = (uint64_t)at;
  offp = at == FNU_IO_HERE ? &f->off : &off;
  uio.uio_iov = iov;
  uio.uio_iovcnt = cnt;
  uio.uio_offset = *offp;
  uio.uio_resid = total;
  uio.uio_segflg = UIO_USERSPACE;
  uio.uio_rw = rw;
  uio.uio_procp = p;
  r = total == 0 ? 0
      : rw == UIO_READ ? fileread_uio(f, &uio, offp)
                       : filewrite_uio(f, &uio, offp);
  iovec_free(iov, cnt);
  if (r < 0)
    return -r;
  *retval = r;
  return 0;
}

static int read_file(struct proc *p, const register_t *uap, register_t *retval,
                     struct file *f, int fd __unused) {
  return rw_file(p, uap, retval, f, UIO_READ);
}

static int write_file(struct proc *p, const register_t *uap, register_t *retval,
                      struct file *f, int fd __unused) {
  return rw_file(p, uap, retval, f, UIO_WRITE);
}

int sys_read(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, read_file);
}

int sys_write(struct proc *p, const register_t *uap, register_t *retval) {
  return withfile(p, uap, retval, write_file);
}

int sys_close(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct file *f;
  int fd;

  fd = (int)uap[0];
  if ((f = fd_take(p->p_p, fd)) == 0)
    return FNU_ERR_BADHANDLE;
  fileclose(f);
  return 0;
}

static int linkat_paths(struct proc *p, const register_t *uap,
                        register_t *retval __unused, char *old, char *new) {
  char name[NAMEBUFSZ];
  struct inode *newdir, *dp, *ip, *other;
  int flags = (int)uap[4], error;

  if (flags & ~FNU_PATH_FOLLOW)
    return FNU_ERR_INVALID;
  if ((error = namei_from((int)uap[0], old, !(flags & FNU_PATH_FOLLOW), UV_READ,
                          &ip)) != 0)
    return error;

  ilock(ip);
  if (ip->type == FNU_TYPE_DIR) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  iunlock(ip);

  if ((error = at_dir((int)uap[2], &newdir)) != 0) {
    iput(ip);
    return error;
  }
  error = nameiparent_at(newdir, new, name, UV_CREATE, &dp);
  if (newdir != 0)
    iput(newdir);
  if (error != 0) {
    iput(ip);
    return error;
  }
  ilock(dp);
  if (vfs_readonly(dp->dev) || !inode_access(dp, p->p_p->uid, 3) ||
      dp->dev != ip->dev || (dp->flags & FNU_IMMUTABLE) ||
      dp->i_op->link == 0) {
    error = vfs_readonly(dp->dev) ? FNU_ERR_READONLY
            : dp->dev != ip->dev ? FNU_ERR_CROSSDEVICE
            : (dp->flags & FNU_IMMUTABLE) ? FNU_ERR_NOTPERMITTED
            : dp->i_op->link == 0 ? FNU_ERR_UNSUPPORTED : FNU_ERR_DENIED;
    iunlockput(dp);
    iput(ip);
    return error;
  }
  if ((other = dirlookup(dp, name)) != 0) {
    iunlockput(dp);
    iput(other);
    iput(ip);
    return FNU_ERR_EXISTS;
  }
  ilock(ip);
  error = frozen(ip) ? FNU_ERR_NOTPERMITTED : dp->i_op->link(dp, name, ip);
  if (error == 0) {
    knote_inode(ip, FNU_NOTE_LINK);
    knote_inode(dp, FNU_NOTE_WRITE);
  }
  iunlockput(dp);
  iunlockput(ip);
  return error;
}

int sys_link(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath2(p, uap, retval, 1, 3, linkat_paths);
}

// the sticky bit
static int sticky_denies(struct inode *dp, struct inode *ip) {
  uint uid = curproc->p_p->uid;

  return (dp->mode & FNU_MODE_STICKY) != 0 && uid != 0 && uid != ip->uid &&
         uid != dp->uid;
}

static int isdirempty(struct inode *dp) {
  return dp->i_op->isempty != 0 && dp->i_op->isempty(dp);
}

static int remove_path(struct proc *p, const register_t *uap,
                       register_t *retval __unused, char *path) {
  struct inode *ip, *dp;
  char name[NAMEBUFSZ];
  int flags = (int)uap[2], error;

  if (flags & ~FNU_REMOVE_DIR)
    return FNU_ERR_INVALID;
  if ((error = parent_from((int)uap[0], path, name, UV_CREATE, &dp)) != 0)
    return error;
  ilock(dp);
  if (vfs_readonly(dp->dev))
    error = FNU_ERR_READONLY;
  else if (!inode_access(dp, p->p_p->uid, 3))
    error = FNU_ERR_DENIED;
  else if (name_is_dot(name))
    error = FNU_ERR_INVALID;
  else if (dp->i_op->remove == 0)
    error = FNU_ERR_UNSUPPORTED;
  else if ((ip = dirlookup(dp, name)) == 0)
    error = FNU_ERR_NOTFOUND;
  else {
    ilock(ip);
    if (sticky_denies(dp, ip) || frozen(ip) ||
        (dp->flags & (FNU_IMMUTABLE | FNU_APPEND)))
      error = FNU_ERR_NOTPERMITTED;
    else if ((flags & FNU_REMOVE_DIR) && ip->type != FNU_TYPE_DIR)
      error = FNU_ERR_NOTDIR;
    else if (!(flags & FNU_REMOVE_DIR) && ip->type == FNU_TYPE_DIR)
      error = FNU_ERR_ISDIR;
    else if (vfs_covering(ip->dev, ip->inum) != 0)
      error = FNU_ERR_BUSY;
    else if (ip->type == FNU_TYPE_DIR && !isdirempty(ip))
      error = FNU_ERR_NOTEMPTY;
    else
      error = dp->i_op->remove(dp, name, ip);
    if (error == 0) {
      knote_inode(ip, FNU_NOTE_DELETE);
      knote_inode(dp, FNU_NOTE_WRITE | (ip->type == FNU_TYPE_DIR ? FNU_NOTE_LINK : 0));
    }
    iunlockput(dp);
    iunlockput(ip);
    return error;
  }
  iunlockput(dp);
  return error;
}

int sys_remove(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath(p, uap, retval, 1, remove_path);
}

static int dir_under(struct inode *ip, struct inode *dp) {
  struct inode *cur, *next;
  int depth;

  if (ip->dev != dp->dev)
    return 0;
  if (ip->inum == dp->inum)
    return 1;
  cur = idup(dp);
  for (depth = 0; depth < MAXPATHDEPTH; depth++) {
    ilock(cur);
    next = cur->type == FNU_TYPE_DIR ? dirlookup(cur, "..") : 0;
    iunlock(cur);
    if (next == 0) {
      iput(cur);
      return 0;
    }
    if (next == cur) {
      iput(next);
      iput(cur);
      return 0;
    }
    iput(cur);
    cur = next;
    if (cur->inum == ip->inum) {
      iput(cur);
      return 1;
    }
  }
  iput(cur);
  return 1;
}

static int rename_lock_dirs(struct inode **set, struct inode *fdp,
                            struct inode *tdp) {
  int n = 0, i;

  if (fdp == tdp || !dir_under(tdp, fdp)) {
    set[n++] = fdp;
    if (tdp != fdp)
      set[n++] = tdp;
  } else {
    set[n++] = tdp;
    set[n++] = fdp;
  }
  for (i = 0; i < n; i++)
    ilock(set[i]);
  return n;
}

static void rename_unlock_set(struct inode **set, int n) {
  while (n-- > 0)
    iunlock(set[n]);
}

static int rename_paths(struct proc *p, const register_t *uap,
                        register_t *retval __unused, char *from, char *to) {
  char fromname[NAMEBUFSZ], toname[NAMEBUFSZ];
  struct inode *ip = 0, *fdp = 0, *tdp = 0, *victim = 0, *set[4];
  int error, moved_dir, n = 0;

  fs_rename_lock();
  if ((error = namei_from((int)uap[0], from, 1, UV_READ | UV_CREATE, &ip)) != 0)
    goto out;
  if ((error = parent_from((int)uap[0], from, fromname, UV_READ | UV_CREATE, &fdp)) != 0)
    goto out;
  if ((error = parent_from((int)uap[2], to, toname, UV_CREATE, &tdp)) != 0)
    goto out;
  error = FNU_ERR_INVALID;
  if (name_is_dot(fromname) || name_is_dot(toname))
    goto out;
  error = FNU_ERR_CROSSDEVICE;
  if (fdp->dev != tdp->dev || ip->dev != fdp->dev)
    goto out;
  error = vfs_readonly(tdp->dev) ? FNU_ERR_READONLY : FNU_ERR_UNSUPPORTED;
  if (vfs_readonly(tdp->dev) || tdp->i_op->rename == 0)
    goto out;
  ilock(ip);
  moved_dir = ip->type == FNU_TYPE_DIR;
  iunlock(ip);
  error = FNU_ERR_INVALID;
  if (moved_dir && dir_under(ip, tdp))
    goto out;

  for (;;) {
    struct inode *src, *dst;
    int vdir = 0;

    ilock(tdp);
    victim = dirlookup(tdp, toname);
    iunlock(tdp);
    if (victim != 0 && victim == ip) {
      error = 0;
      goto out;
    }
    if (victim != 0) {
      ilock(victim);
      vdir = victim->type == FNU_TYPE_DIR;
      iunlock(victim);
      if (vdir && dir_under(victim, fdp)) {
        error = FNU_ERR_NOTEMPTY;
        goto out;
      }
    }
    n = rename_lock_dirs(set, fdp, tdp);
    src = dirlookup(fdp, fromname);
    dst = dirlookup(tdp, toname);
    if (src == ip && dst == victim) {
      iput(src);
      if (dst != 0)
        iput(dst);
      set[n++] = ip;
      ilock(ip);
      if (victim != 0) {
        set[n++] = victim;
        ilock(victim);
      }
      break;
    }
    rename_unlock_set(set, n);
    n = 0;
    if (src != 0)
      iput(src);
    if (dst != 0)
      iput(dst);
    if (victim != 0)
      iput(victim);
    victim = 0;
    if (src != ip) {
      error = FNU_ERR_NOTFOUND;
      goto out;
    }
  }

  error = FNU_ERR_DENIED;
  if (!inode_access(fdp, p->p_p->uid, 3) || !inode_access(tdp, p->p_p->uid, 3))
    goto unlock;
  error = FNU_ERR_NOTPERMITTED;
  if (sticky_denies(fdp, ip) || (victim != 0 && sticky_denies(tdp, victim)) ||
      frozen(ip) || (victim != 0 && frozen(victim)) ||
      (fdp->flags & (FNU_IMMUTABLE | FNU_APPEND)) ||
      (tdp->flags & FNU_IMMUTABLE) ||
      (victim != 0 && (tdp->flags & FNU_APPEND)))
    goto unlock;
  if (victim != 0) {
    error = moved_dir ? FNU_ERR_NOTDIR : FNU_ERR_ISDIR;
    if ((victim->type == FNU_TYPE_DIR) != moved_dir)
      goto unlock;
    error = FNU_ERR_BUSY;
    if (vfs_covering(victim->dev, victim->inum) != 0)
      goto unlock;
    error = FNU_ERR_NOTEMPTY;
    if (victim->type == FNU_TYPE_DIR && !isdirempty(victim))
      goto unlock;
  }

  error = tdp->i_op->rename(fdp, fromname, ip, tdp, toname, victim);
  if (error == 0) {
    knote_inode(ip, FNU_NOTE_RENAME);
    knote_inode(fdp, FNU_NOTE_WRITE);
    if (tdp != fdp)
      knote_inode(tdp, FNU_NOTE_WRITE);
    if (victim != 0)
      knote_inode(victim, FNU_NOTE_DELETE);
  }

unlock:
  rename_unlock_set(set, n);
out:
  if (victim)
    iput(victim);
  if (tdp)
    iput(tdp);
  if (fdp)
    iput(fdp);
  if (ip)
    iput(ip);
  fs_rename_unlock();
  return error;
}

int sys_rename(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath2(p, uap, retval, 1, 3, rename_paths);
}

// FNU_OPEN_CREATE
static int existing_ok(const struct inode *ip) {
  return ip->type == FNU_TYPE_FILE || ip->type == FNU_TYPE_DEVICE || ip->type == FNU_TYPE_DISK ||
         ip->type == FNU_TYPE_FIFO;
}

static struct inode *create(struct inode *start, char *path, short type,
                            int mode, const char *target, int *error) {
  struct inode *ip, *dp;
  char name[NAMEBUFSZ];
  struct vattr va;

  mode &= 07777 & ~(curproc->p_p->umask & 0777);
  if ((*error = nameiparent_at(start, path, name, UV_CREATE, &dp)) != 0)
    return 0;
  ilock(dp);
  if (vfs_readonly(dp->dev))
    *error = FNU_ERR_READONLY;
  else if (!inode_access(dp, curproc->p_p->uid, 3))
    *error = FNU_ERR_DENIED;
  else if (dp->flags & FNU_IMMUTABLE)
    *error = FNU_ERR_NOTPERMITTED;
  if (*error != 0) {
    iunlockput(dp);
    return 0;
  }
  if ((ip = dirlookup(dp, name)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == FNU_TYPE_FILE && existing_ok(ip))
      return ip;
    iunlockput(ip);
    *error = FNU_ERR_EXISTS;
    return 0;
  }
  memset(&va, 0, sizeof(va));
  va.va_type = type;
  va.va_mode = (ushort)mode;
  if (type == FNU_TYPE_LINK)
    *error = dp->i_op->symlink != 0
                 ? dp->i_op->symlink(dp, name, &va, target, &ip)
                 : FNU_ERR_UNSUPPORTED;
  else
    *error = dp->i_op->create != 0 ? dp->i_op->create(dp, name, &va, &ip)
                                   : FNU_ERR_UNSUPPORTED;
  if (*error == 0)
    knote_inode(dp, FNU_NOTE_WRITE | (type == FNU_TYPE_DIR ? FNU_NOTE_LINK : 0));
  iunlockput(dp);
  return *error == 0 ? ip : 0;
}

enum { SF_NULL = 1, SF_TTY, SF_SHADOW, SF_PASSWD, SF_GROUP, SF_NETID, SF_DNS,
       SF_TIME };

static const struct {
  const char *path;
  int item;
} sysfiles[] = {
    {"/dev/null", SF_NULL},          {"/dev/tty", SF_TTY},
    {"/etc/group", SF_GROUP},        {"/etc/hosts", SF_DNS},
    {"/etc/localtime", SF_TIME},     {"/etc/master.passwd", SF_SHADOW},
    {"/etc/netid", SF_NETID},        {"/etc/passwd", SF_PASSWD},
    {"/etc/protocols", SF_DNS},      {"/etc/resolv.conf", SF_DNS},
    {"/etc/services", SF_DNS},
};

#define ZONEINFO "/usr/share/zoneinfo/"

static int sysfile_item(const char *path) {
  const char *c;
  size_t i;

  for (i = 0; i < nitems(sysfiles); i++)
    if (strcmp(path, sysfiles[i].path) == 0)
      return sysfiles[i].item;
  if (strncmp(path, ZONEINFO, sizeof(ZONEINFO) - 1) != 0)
    return 0;
  for (c = path + sizeof(ZONEINFO) - 1; *c != 0; c++)
    if (c[0] == '.' && c[1] == '.')
      return 0;
  return SF_TIME;
}

static int sysfile_open(struct proc *p, const char *path, uint64_t nip, int *error) {
  uint64_t ple = p->p_p->pledge;

  *error = 0;
  switch (sysfile_item(path)) {
  case 0:
    pledge_fail(p, SYS_open);
    *error = FNU_ERR_DENIED;
    return 0;
  case SF_NULL:
    return (nip & ~(FNU_PLEDGE_RPATH | FNU_PLEDGE_WPATH)) == 0;
  case SF_TTY:
    return (ple & FNU_PLEDGE_TTY) &&
           (nip & ~(FNU_PLEDGE_RPATH | FNU_PLEDGE_WPATH)) == 0;
  case SF_SHADOW:
    if ((ple & FNU_PLEDGE_GETPW) && nip == FNU_PLEDGE_RPATH)
      *error = FNU_ERR_NOTPERMITTED;
    return 0;
  case SF_PASSWD:
  case SF_GROUP:
  case SF_NETID:
    return (ple & FNU_PLEDGE_GETPW) && nip == FNU_PLEDGE_RPATH;
  case SF_DNS:
    return (ple & FNU_PLEDGE_DNS) && nip == FNU_PLEDGE_RPATH;
  default:
    return nip == FNU_PLEDGE_RPATH;
  }
}

static int open_path(struct proc *p, const register_t *uap,
                     register_t *retval, char *path) {
  struct inode *start;
  int omode = (int)uap[2], error, through = 0;
  uint64_t nip = ((omode & FNU_OPEN_READ) ? FNU_PLEDGE_RPATH : 0) |
                 ((omode & FNU_OPEN_WRITE) ? FNU_PLEDGE_WPATH : 0) |
                 ((omode & FNU_OPEN_CREATE) ? FNU_PLEDGE_CPATH : 0);

  if ((omode & FNU_OPEN_SYSFILE) && p->p_p->pledged) {
    through = sysfile_open(p, path, nip, &error);
    if (error != 0)
      return error;
  }
  omode &= ~FNU_OPEN_SYSFILE;
  if (through)
    omode |= OPEN_THROUGH;
  else if ((error = pledge_need_all(p, nip, SYS_open)) != 0)
    return error;
  if ((error = at_dir((int)uap[0], &start)) != 0)
    return error;
  error = kern_open(p, start, path, omode,
                    (omode & FNU_OPEN_CREATE) ? (int)uap[3] : 0, retval);
  if (start != 0)
    iput(start);
  return error;
}

int sys_open(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath(p, uap, retval, 1, open_path);
}


int kern_open_file(struct proc *p, struct inode *start, char *path, int omode,
                   int mode, struct file **fp) {
  int error, want;
  size_t sz;
  struct file *f;
  struct inode *ip;

  want = (omode & OPEN_THROUGH) ? UV_NONE
         : ((omode & FNU_OPEN_READ) ? UV_READ : 0) |
               ((omode & FNU_OPEN_WRITE) ? UV_WRITE : 0) |
               ((omode & FNU_OPEN_CREATE) ? UV_CREATE : 0);
  omode &= ~OPEN_THROUGH;
  if ((omode & FNU_OPEN_READWRITE) == 0 || (omode & ~OPEN_FLAGS))
    return FNU_ERR_INVALID;
  if ((omode & (FNU_OPEN_CREATE | FNU_OPEN_DIRONLY)) ==
      (FNU_OPEN_CREATE | FNU_OPEN_DIRONLY))
    return FNU_ERR_INVALID;

  if (omode & FNU_OPEN_CREATE) {
    ip = 0;
    if (!(omode & FNU_OPEN_NEW))
      (omode & FNU_OPEN_NOLINK) ? namei_nofollow_at(start, path, want, &ip)
                                : namei_at(start, path, want, &ip);
    if (ip != 0) {
      ilock(ip);
      if (ip->type == FNU_TYPE_LINK) {
        iunlockput(ip);
        return FNU_ERR_LOOP;
      }
      if (!existing_ok(ip)) {
        error = ip->type == FNU_TYPE_DIR ? FNU_ERR_ISDIR : FNU_ERR_EXISTS;
        iunlockput(ip);
        return error;
      }
      if ((omode & FNU_OPEN_WRITE) &&
          !inode_access(ip, p->p_p->uid, 2)) {
        iunlockput(ip);
        return FNU_ERR_DENIED;
      }
    } else {
      if (omode & FNU_OPEN_NEW) {
        struct inode *taken;
        if (namei_nofollow_at(start, path, want, &taken) == 0) {
          iput(taken);
          return FNU_ERR_EXISTS;
        }
      }
      ip = create(start, path, FNU_TYPE_FILE, mode, 0, &error);
      if (ip == 0) {
        return error;
      }
    }
  } else {
    error = (omode & FNU_OPEN_NOLINK) ? namei_nofollow_at(start, path, want, &ip)
                                      : namei_at(start, path, want, &ip);
    if (error != 0)
      return error;
    ilock(ip);
    if (ip->type == FNU_TYPE_LINK) {
      iunlockput(ip);
      return FNU_ERR_LOOP;
    }
    if ((omode & FNU_OPEN_DIRONLY) && ip->type != FNU_TYPE_DIR) {
      iunlockput(ip);
      return FNU_ERR_NOTDIR;
    }
    if ((omode & FNU_OPEN_WRITE) &&
        !inode_access(ip, p->p_p->uid, 2)) {
      iunlockput(ip);
      return FNU_ERR_DENIED;
    }
    if ((omode & FNU_OPEN_READ) &&
        !inode_access(ip, p->p_p->uid, 4)) {
      iunlockput(ip);
      return FNU_ERR_DENIED;
    }
    if (ip->type != FNU_TYPE_FILE && ip->type != FNU_TYPE_DIR && ip->type != FNU_TYPE_DEVICE &&
        ip->type != FNU_TYPE_DISK && ip->type != FNU_TYPE_FIFO) {
      iunlockput(ip);
      return FNU_ERR_UNSUPPORTED;
    }
    if (ip->type == FNU_TYPE_DIR && (omode & FNU_OPEN_WRITE)) {
      iunlockput(ip);
      return FNU_ERR_ISDIR;
    }
  }

  // FNU_OPEN_TRUNCATE
  if ((omode & FNU_OPEN_TRUNCATE) && !inode_access(ip, p->p_p->uid, 2)) {
    iunlockput(ip);
    return FNU_ERR_DENIED;
  }
  {
    int wr = (omode & FNU_OPEN_WRITE) != 0;

    if (((wr || (omode & FNU_OPEN_TRUNCATE)) && (ip->flags & FNU_IMMUTABLE)) ||
        ((ip->flags & FNU_APPEND) &&
         ((wr && !(omode & FNU_OPEN_APPEND)) || (omode & FNU_OPEN_TRUNCATE)))) {
      iunlockput(ip);
      return FNU_ERR_NOTPERMITTED;
    }
  }
  if (ip->type == FNU_TYPE_FILE &&
      (omode & (FNU_OPEN_WRITE | FNU_OPEN_TRUNCATE)) && vm_text_busy(ip)) {
    iunlockput(ip);
    return FNU_ERR_TEXTBUSY;
  }

  if ((omode & FNU_OPEN_TRUNCATE) && ip->type == FNU_TYPE_FILE && ip->size != 0) {
    error = ip->i_op->truncate != 0 ? ip->i_op->truncate(ip, 0)
                                    : FNU_ERR_UNSUPPORTED;
    if (error != 0) {
      iunlockput(ip);
      return error;
    }
    knote_inode(ip, FNU_NOTE_ATTRIB | FNU_NOTE_TRUNCATE);
  }
  if (ip->type == FNU_TYPE_DEVICE && ip->major == CTTY) {
    struct inode *tp = devfs_ctty(p->p_p);

    iunlockput(ip);
    if (tp == 0)
      return FNU_ERR_NODEVICE;
    ip = tp;
    ilock(ip);
    if (((omode & FNU_OPEN_WRITE) && !inode_access(ip, p->p_p->uid, 2)) ||
        ((omode & FNU_OPEN_READ) && !inode_access(ip, p->p_p->uid, 4))) {
      iunlockput(ip);
      return FNU_ERR_DENIED;
    }
  }
  sz = (size_t)ip->size;

  if ((f = filealloc()) == 0) {
    iunlockput(ip);
    return FNU_ERR_SYSTEMFULL;
  }
  if (ip->type == FNU_TYPE_DEVICE && ip->major >= 0 && ip->major < NDEV &&
      devsw[ip->major].open != 0 && (error = devsw[ip->major].open(ip, omode)) != 0) {
    fileclose(f);
    iunlockput(ip);
    return error;
  }
  if (ip->type == FNU_TYPE_FIFO) {
    struct pipe *pipe = 0;
    int readable = (omode & FNU_OPEN_READ) != 0;
    int writable = (omode & FNU_OPEN_WRITE) != 0;

    iunlock(ip);
    error = fifo_open(ip->dev, ip->inum, readable, writable,
                      (omode & FNU_OPEN_NOWAIT) != 0, &pipe);
    if (error != 0) {
      fileclose(f);
      iput(ip);
      return error;
    }
    f->type = FD_PIPE;
    f->pipe = pipe;
    f->ip = ip;
    f->off = 0;
    f->flags = omode;
    f->readable = readable;
    f->writable = writable;
  } else {
    if (ip->type == FNU_TYPE_FILE && (omode & FNU_OPEN_WRITE))
      ip->writecount++;
    iunlock(ip);
    f->type = FD_INODE;
    f->ip = ip;
    f->off = (omode & FNU_OPEN_APPEND) ? sz : 0;
    f->flags = omode;
    f->readable = (omode & FNU_OPEN_READ) != 0;
    f->writable = (omode & FNU_OPEN_WRITE) != 0;
  }
  *fp = f;
  return 0;
}

int kern_open(struct proc *p, struct inode *start, char *path, int omode,
              int mode, register_t *retval) {
  struct file *f;
  int fd, error;

  if ((error = kern_open_file(p, start, path, omode, mode, &f)) != 0)
    return error;
  if ((fd = fd_alloc_at(p->p_p, f, 0, (omode & FNU_OPEN_CLOSEEXEC) ? FNU_FD_CLOSEEXEC : 0)) < 0) {
    fileclose(f);
    return FNU_ERR_TOOMANYOPEN;
  }
  *retval = fd;
  return 0;
}

static int make_path(struct proc *p, const register_t *uap,
                     register_t *retval __unused, char *path) {
  int type = (int)uap[2], mode = (int)uap[3], error;
  struct pathname target;
  struct inode *start, *ip;

  if (mode & ~07777)
    return FNU_ERR_INVALID;
  if (type != FNU_TYPE_DIR && type != FNU_TYPE_FIFO && type != FNU_TYPE_LINK)
    return FNU_ERR_INVALID;
  if ((error = pledge_need(p, type == FNU_TYPE_FIFO ? FNU_PLEDGE_DPATH
                                                    : FNU_PLEDGE_CPATH,
                           SYS_make)) != 0)
    return error;
  target.pn_buf = 0;
  if (type == FNU_TYPE_LINK) {
    if ((error = pn_get((uintptr_t)uap[4], &target)) != 0)
      return error;
    if (target.pn_buf[0] == 0) {
      pn_free(&target);
      return FNU_ERR_NOTFOUND;
    }
    mode = 0777;
  }
  if ((error = at_dir((int)uap[0], &start)) == 0) {
    if ((ip = create(start, path, (short)type, mode, target.pn_buf, &error)) != 0)
      iunlockput(ip);
    if (start != 0)
      iput(start);
  }
  if (target.pn_buf != 0)
    pn_free(&target);
  return error;
}

int sys_make(struct proc *p, const register_t *uap, register_t *retval) {
  return withpath(p, uap, retval, 1, make_path);
}

int sys_chdir(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct inode *ip;
  int error;

  if ((error = name_target((int)uap[0], (uintptr_t)uap[1], 0, UV_READ, &ip)) != 0)
    return error;
  ilock(ip);
  if (ip->type != FNU_TYPE_DIR || !inode_access(ip, p->p_p->uid, 1)) {
    error = ip->type != FNU_TYPE_DIR ? FNU_ERR_NOTDIR : FNU_ERR_DENIED;
    iunlockput(ip);
    return error;
  }
  iunlock(ip);
  fd_setcwd(p->p_p, ip);
  return 0;
}

static int path_append(char *dst, uint size, const char *add) {
  uint n = (uint)strlen(dst), m = (uint)strlen(add);

  if (n + 1 + m + 1 > size)
    return -1;
  if (dst[1] != 0) {
    dst[n++] = '/';
    dst[n] = 0;
  }
  memmove(dst + n, add, m + 1);
  return 0;
}

static int pathof_name(struct proc *p __unused, int dirfd, char *work, char *out) {
  char target[MAXPATHLEN], name[NAMEBUFSZ];
  struct inode *ip, *dp;
  int loops, n, error;

  for (loops = 0; loops < MAXSYMLINKS; loops++) {
    if ((error = namei_from(dirfd, work, 1, UV_READ, &ip)) != 0)
      return error;
    ilock(ip);
    if (ip->type == FNU_TYPE_LINK) {
      n = readlinki(ip, target, sizeof(target));
      iunlockput(ip);
      if (n < 0 || n >= (int)sizeof(target))
        return FNU_ERR_INVALID;
      target[n] = 0;
      if (target[0] == '/') {
        strlcpy(work, target, MAXPATHLEN);
        continue;
      }
      if ((error = parent_from(dirfd, work, name, UV_READ, &dp)) != 0)
        return error;
      error = inode_path(dp, work, MAXPATHLEN) < 0 ? FNU_ERR_RANGE : 0;
      iput(dp);
      if (error != 0)
        return error;
      if (path_append(work, MAXPATHLEN, target) < 0)
        return FNU_ERR_NAMETOOLONG;
      continue;
    }
    if (ip->type == FNU_TYPE_DIR) {
      iunlock(ip);
      error = inode_path(ip, out, MAXPATHLEN) < 0 ? FNU_ERR_RANGE : 0;
      iput(ip);
      return error;
    }
    iunlockput(ip);
    if ((error = parent_from(dirfd, work, name, UV_READ, &dp)) != 0)
      return error;
    error = inode_path(dp, out, MAXPATHLEN) < 0 ? FNU_ERR_RANGE : 0;
    iput(dp);
    if (error != 0)
      return error;
    return path_append(out, MAXPATHLEN, name) < 0 ? FNU_ERR_NAMETOOLONG : 0;
  }
  return FNU_ERR_LOOP;
}

int sys_pathof(struct proc *p, const register_t *uap, register_t *retval __unused) {
  char work[MAXPATHLEN], out[MAXPATHLEN];
  int size = (int)uap[3], error;
  struct inode *dp;

  if (size < 2)
    return FNU_ERR_INVALID;
  if (uap[1] == 0) {
    if ((error = pledge_need(p, FNU_PLEDGE_RPATH | FNU_PLEDGE_WPATH, SYS_pathof)) != 0 ||
        (error = at_dir((int)uap[0], &dp)) != 0)
      return error;
    if (dp == 0)
      dp = fd_cwd(p->p_p);
    error = inode_path(dp, out, sizeof(out)) < 0 ? FNU_ERR_RANGE : 0;
    iput(dp);
  } else if ((error = pledge_need(p, FNU_PLEDGE_RPATH, SYS_pathof)) == 0 &&
             (error = copyinstr(p->p_p->pgdir, work, (uintptr_t)uap[1],
                                sizeof(work), 0)) == 0)
    error = pathof_name(p, (int)uap[0], work, out);
  if (error != 0)
    return error;
  if ((int)strlen(out) + 1 > size)
    return FNU_ERR_RANGE;
  return copyout(p->p_p->pgdir, (uintptr_t)uap[2], out, strlen(out) + 1) < 0
             ? FNU_ERR_FAULT
             : 0;
}

static int setattr_put(struct inode *ip, const struct vattr *va) {
  int error = ip->i_op->setattr != 0 ? ip->i_op->setattr(ip, va)
                                     : FNU_ERR_UNSUPPORTED;

  if (error == 0)
    knote_inode(ip, FNU_NOTE_ATTRIB);
  iunlockput(ip);
  return error;
}

// inode locked
static int chmod_inode(struct proc *p, struct inode *ip, int mode) {
  struct vattr va;

  ilock(ip);
  if (vfs_readonly(ip->dev)) {
    iunlockput(ip);
    return FNU_ERR_READONLY;
  }
  if ((p->p_p->uid != 0 && p->p_p->uid != ip->uid) || frozen(ip)) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  // setgid needs the group
  if (p->p_p->uid != 0 && (mode & FNU_MODE_SETGID) && !groupmember(ip->gid)) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  va.va_mask = AT_MODE;
  va.va_mode = (ushort)mode;
  return setattr_put(ip, &va);
}

static int chflags_inode(struct proc *p, struct inode *ip, uint flags) {
  uint uid = p->p_p->uid;
  struct vattr va;

  ilock(ip);
  if (vfs_readonly(ip->dev)) {
    iunlockput(ip);
    return FNU_ERR_READONLY;
  }
  if (uid != 0 && (uid != ip->uid ||
                   (ip->flags & (FNU_SF_IMMUTABLE | FNU_SF_APPEND)) ||
                   (flags & FNU_UF_SETTABLE) != flags)) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  va.va_mask = AT_FLAGS;
  va.va_flags = uid == 0 ? flags : (ip->flags & FNU_SF_SETTABLE) | flags;
  return setattr_put(ip, &va);
}

static int chown_inode(struct proc *p, struct inode *ip, int uid, int gid) {
  struct vattr va;

  ilock(ip);
  if (uid < 0)
    uid = (int)ip->uid;
  if (gid < 0)
    gid = (int)ip->gid;
  if (vfs_readonly(ip->dev)) {
    iunlockput(ip);
    return FNU_ERR_READONLY;
  }
  if (frozen(ip) ||
      (p->p_p->uid != 0 &&
       (p->p_p->uid != ip->uid || (uint)uid != ip->uid ||
        ((uint)gid != ip->gid && !groupmember((uint)gid))))) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  va.va_mask = AT_OWNER;
  va.va_uid = (uint)uid;
  va.va_gid = (uint)gid;
  if (p->p_p->uid != 0 && (ip->mode & (FNU_MODE_SETUID | FNU_MODE_SETGID)) &&
      ((uint)uid != ip->uid || (uint)gid != ip->gid)) {
    va.va_mask |= AT_MODE;
    va.va_mode = ip->mode & 07777 & ~(FNU_MODE_SETUID | FNU_MODE_SETGID);
  }
  return setattr_put(ip, &va);
}

int sys_exec(struct proc *p __unused, const register_t *uap, register_t *retval __unused) {
  return exec_user((uintptr_t)uap[0], (uintptr_t)uap[1], (uintptr_t)uap[2]);
}

static int utimens_inode(struct proc *p, struct inode *ip,
                         const struct fnu_timespec *times) {
  struct fnu_timespec now;
  struct vattr va;
  int owner, set_a, set_m, i;

  for (i = 0; i < 2; i++)
    if (times[i].nsec != FNU_TIME_NOW && times[i].nsec != FNU_TIME_KEEP &&
        (times[i].nsec < 0 || times[i].nsec >= 1000000000)) {
      iput(ip);
      return FNU_ERR_INVALID;
    }
  set_a = times[0].nsec != FNU_TIME_KEEP;
  set_m = times[1].nsec != FNU_TIME_KEEP;
  ilock(ip);
  if (frozen(ip)) {
    iunlockput(ip);
    return FNU_ERR_NOTPERMITTED;
  }
  owner = p->p_p->uid == 0 || p->p_p->uid == ip->uid;
  if (vfs_readonly(ip->dev)) {
    iunlockput(ip);
    return FNU_ERR_READONLY;
  }
  if (!owner) {
    if ((set_a && times[0].nsec != FNU_TIME_NOW) ||
        (set_m && times[1].nsec != FNU_TIME_NOW)) {
      iunlockput(ip);
      return FNU_ERR_NOTPERMITTED;
    }
    if (!inode_access(ip, p->p_p->uid, 2)) {
      iunlockput(ip);
      return FNU_ERR_DENIED;
    }
  }
  time_now(&now);
  memset(&va, 0, sizeof(va));
  if (set_a) {
    va.va_mask |= AT_ATIME;
    va.va_atime = times[0].nsec == FNU_TIME_NOW ? now : times[0];
  }
  if (set_m) {
    va.va_mask |= AT_MTIME;
    va.va_mtime = times[1].nsec == FNU_TIME_NOW ? now : times[1];
  }
  return setattr_put(ip, &va);
}

static int pathconf_inode(struct inode *ip, int name, int64_t *value) {
  return ip->i_op->pathconf != 0 ? ip->i_op->pathconf(ip, name, value)
                                 : vop_pathconf_default(name, value);
}

int sys_pathconf(struct proc *p, const register_t *uap, register_t *retval) {
  int flags = (int)uap[2], name = (int)uap[3], error;
  int64_t value = 0;
  struct inode *ip;
  struct file *f;

  if (flags & ~FNU_PATH_NOFOLLOW)
    return FNU_ERR_INVALID;
  if (uap[1] == 0) {
    if ((f = fd_getfile(p->p_p, (int)uap[0])) == 0)
      return FNU_ERR_BADHANDLE;
    if (f->type == FD_PIPE && f->ip == 0) {
      FRELE(f);
      if (name != FNU_PC_PIPE_BUF)
        return FNU_ERR_INVALID;
      *retval = pipe_bufsize();
      return 0;
    }
    FRELE(f);
  }
  if ((error = name_target((int)uap[0], (uintptr_t)uap[1],
                           flags & FNU_PATH_NOFOLLOW, UV_READ, &ip)) != 0)
    return error;
  ilock(ip);
  error = pathconf_inode(ip, name, &value);
  iunlockput(ip);
  if (error != 0)
    return error;
  *retval = (register_t)value;
  return 0;
}

int sys_setattr(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_timespec times[2];
  struct fnu_setattr a;
  struct inode *ip;
  struct file *f = 0;
  int flags = (int)uap[2], error = 0;

  if (flags & ~FNU_PATH_NOFOLLOW)
    return FNU_ERR_INVALID;
  if (copyin(p->p_p->pgdir, &a, (uintptr_t)uap[3], sizeof(a)) < 0)
    return FNU_ERR_FAULT;
  if (a.mask & ~(FNU_SET_MODE | FNU_SET_UID | FNU_SET_GID | FNU_SET_FLAGS |
                 FNU_SET_ATIME | FNU_SET_MTIME | FNU_SET_SIZE))
    return FNU_ERR_INVALID;
  if ((a.mask & FNU_SET_MODE) && (a.mode & ~07777))
    return FNU_ERR_INVALID;
  if ((a.mask & (FNU_SET_UID | FNU_SET_GID)) &&
      (error = pledge_need(p, FNU_PLEDGE_CHOWN, SYS_setattr)) != 0)
    return error;
  if ((a.mask & (FNU_SET_MODE | FNU_SET_FLAGS | FNU_SET_ATIME | FNU_SET_MTIME)) &&
      (error = pledge_need(p, FNU_PLEDGE_FATTR, SYS_setattr)) != 0)
    return error;
  if ((a.mask & FNU_SET_SIZE) && uap[1] != 0 &&
      (error = pledge_need(p, FNU_PLEDGE_WPATH, SYS_setattr)) != 0)
    return error;
  if (uap[1] == 0 && (f = fd_getfile(p->p_p, (int)uap[0])) == 0)
    return FNU_ERR_BADHANDLE;
  if (f != 0 && (a.mask & FNU_SET_SIZE) &&
      (f->type != FD_INODE || f->writable == 0))
    error = FNU_ERR_INVALID;
  else
    error = name_target((int)uap[0], (uintptr_t)uap[1],
                        flags & FNU_PATH_NOFOLLOW, UV_WRITE, &ip);
  if (f != 0)
    FRELE(f);
  if (error != 0)
    return error;
  if (a.mask & FNU_SET_SIZE)
    error = truncate_inode(idup(ip), (int64_t)a.size, f == 0);
  if (error == 0 && (a.mask & FNU_SET_MODE))
    error = chmod_inode(p, idup(ip), (int)a.mode);
  if (error == 0 && (a.mask & (FNU_SET_UID | FNU_SET_GID)))
    error = chown_inode(p, idup(ip), (a.mask & FNU_SET_UID) ? (int)a.uid : -1,
                        (a.mask & FNU_SET_GID) ? (int)a.gid : -1);
  if (error == 0 && (a.mask & FNU_SET_FLAGS))
    error = chflags_inode(p, idup(ip), a.flags);
  if (error == 0 && (a.mask & (FNU_SET_ATIME | FNU_SET_MTIME))) {
    times[0] = a.atime;
    times[1] = a.mtime;
    if (!(a.mask & FNU_SET_ATIME))
      times[0].nsec = FNU_TIME_KEEP;
    if (!(a.mask & FNU_SET_MTIME))
      times[1].nsec = FNU_TIME_KEEP;
    error = utimens_inode(p, idup(ip), times);
  }
  iput(ip);
  return error;
}

int sys_pipe(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct file *rf, *wf;
  uint64_t dst;
  int fd[2], flags;

  dst = uap[0];
  flags = (int)uap[1];
  if ((flags & ~(FNU_FD_CLOSEEXEC | FNU_FD_NOWAIT)) != 0)
    return FNU_ERR_INVALID;
  if (pipealloc(&rf, &wf) < 0)
    return FNU_ERR_SYSTEMFULL;
  if (flags & FNU_FD_NOWAIT) {
    rf->flags |= FNU_OPEN_NOWAIT;
    wf->flags |= FNU_OPEN_NOWAIT;
  }
  flags &= FNU_FD_CLOSEEXEC;
  if ((fd[0] = fd_alloc_at(p->p_p, rf, 0, flags)) < 0) {
    fileclose(rf);
    fileclose(wf);
    return FNU_ERR_TOOMANYOPEN;
  }
  if ((fd[1] = fd_alloc_at(p->p_p, wf, 0, flags)) < 0) {
    fd_untake(p->p_p, fd[0], rf);
    fileclose(wf);
    return FNU_ERR_TOOMANYOPEN;
  }
  if (copyout(p->p_p->pgdir, (uintptr_t)dst, fd, sizeof(fd)) < 0) {
    fd_untake(p->p_p, fd[0], rf);
    fd_untake(p->p_p, fd[1], wf);
    return FNU_ERR_FAULT;
  }
  return 0;
}
