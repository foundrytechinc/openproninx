#include "file.h"
#include "blockdev.h"
#include "inc/open.h"
#include "proc.h"
#include "inc/stat.h"
#include "defs.h"
#include "param.h"
#include "malloc.h"
#include "uio.h"

struct devsw devsw[NDEV];

struct {
  struct mutex lock;
  int nfiles;
} ftable;

void fileinit(void) {
  mtx_init(&ftable.lock, "ftable");
  sysctl_add_int("kern.openfiles", &ftable.nfiles, FNU_CTLF_STDIO, 0, 0);
}

struct file *filealloc(void) {
  struct proc *p = curproc;
  uint ruid = p != 0 ? p->p_p->ruid : 0;
  struct file *f = malloc(sizeof(*f), M_FILE, M_WAITOK | M_ZERO);

  if (f == 0)
    return 0;
  mtx_enter(&ftable.lock);
  if (ftable.nfiles >= maxfiles - maxfilesrootres &&
      (ruid != 0 || ftable.nfiles >= maxfiles)) {
    mtx_leave(&ftable.lock);
    free(f, M_FILE);
    return 0;
  }
  ftable.nfiles++;
  mtx_leave(&ftable.lock);
  f->type = FD_NONE;
  f->ref = 1;
  return f;
}

int file_refs(struct file *f) {
  int n;

  mtx_enter(&ftable.lock);
  n = f->ref;
  mtx_leave(&ftable.lock);
  return n;
}

struct file *filedup(struct file *f) {
  mtx_enter(&ftable.lock);
  if (f->ref < 1) {
    panic("filedup");
  }
  f->ref++;
  mtx_leave(&ftable.lock);
  return f;
}

void fileclose(struct file *f) {
  struct file ff;

  mtx_enter(&ftable.lock);
  if (f->ref < 1) {
    panic("fileclose");
  }
  if (--f->ref > 0) {
    mtx_leave(&ftable.lock);
    return;
  }
  mtx_leave(&ftable.lock);
  flock_release(f);
  mtx_enter(&ftable.lock);
  ff = *f;
  ftable.nfiles--;
  mtx_leave(&ftable.lock);
  free(f, M_FILE);

  if (ff.type == FD_PIPE) {
    if (ff.ip != 0) {
      fifo_close(ff.pipe, ff.readable, ff.writable);
      iput(ff.ip);
    } else
      pipeclose(ff.pipe, ff.writable);
  } else if (ff.type == FD_SOCKET) {
    soclose(ff.sock);
  } else if (ff.type == FD_EVENTQ) {
    eventq_close(ff.eq);
  } else if (ff.type == FD_INODE) {
    if (ff.ip->type == FNU_TYPE_DEVICE && ff.ip->major >= 0 && ff.ip->major < NDEV &&
        devsw[ff.ip->major].close != 0)
      devsw[ff.ip->major].close(ff.ip);
    if (ff.ip->type == FNU_TYPE_DISK && ff.writable)
      partition_reprobe(makedev((uint)ff.ip->major, (uint)ff.ip->minor));
    if (ff.ip->type == FNU_TYPE_FILE && ff.writable) {
      ilock(ff.ip);
      ff.ip->writecount--;
      iunlock(ff.ip);
    }
    iput(ff.ip);
  }
}

int filestat(struct file *f, struct fnu_stat *st) {
  if (f->type == FD_INODE || (f->type == FD_PIPE && f->ip != 0)) {
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    return 0;
  }
  if (f->type == FD_PIPE) {
    pipe_stat(f->pipe, st);
    return 0;
  }
  if (f->type == FD_SOCKET) {
    so_stat(f->sock, st);
    return 0;
  }
  if (f->type == FD_EVENTQ) {
    memset(st, 0, sizeof(*st));
    st->size = (uint64)eventq_count(f->eq);
    st->mode = 0600;
    st->uid = curproc->p_p->uid;
    st->gid = curproc->p_p->gid;
    return 0;
  }
  return FNU_ERR_BADHANDLE;
}

static int would_block(struct file *f, int want) {
  if ((f->flags & FNU_OPEN_NOWAIT) == 0)
    return 0;
  if (f->type == FD_PIPE)
    return pipe_ready(f->pipe, want) == 0;
  if (f->type == FD_INODE && f->ip != 0 && f->ip->type == FNU_TYPE_DEVICE &&
      f->ip->major >= 0 && f->ip->major < NDEV && devsw[f->ip->major].poll != 0 &&
      (want & FNU_SEL_READ))
    return !devsw[f->ip->major].poll(f->ip, FNU_SEL_READ);
  return 0;
}

// THE TRANSFER
uint file_device(struct file *f) {
  if (f->ip->type == FNU_TYPE_DISK)
    return makedev((uint)f->ip->major, (uint)f->ip->minor);
  return f->ip->dev;
}

static int vn_transfer(struct file *f, struct uio *uio, uint64_t *offp,
                       int (*op)(struct file *, struct uio *, uint64_t *)) {
  struct proc *p = curproc;
  int through = (f->flags & FNU_OPEN_WRITETHROUGH) && uio->uio_rw == UIO_WRITE;
  int saved = p->ioflag, r;

  p->ioflag = ((f->flags & FNU_OPEN_UNCACHED) ? PIO_NOREUSE : 0) |
              (through ? PIO_SYNC : 0);
  r = op(f, uio, offp);
  p->ioflag = saved;
  if (r > 0 && through && f->ip->type != FNU_TYPE_DEVICE)
    blockdev_flush(file_device(f));
  return r;
}

static int vn_read(struct file *f, struct uio *uio, uint64_t *offp) {
  int r, held;

  if (uio->uio_resid == 0 && f->ip->type != FNU_TYPE_DEVICE)
    return 0;
  if ((held = f->ip->type != FNU_TYPE_DEVICE) && (r = uio_hold(uio)) != 0)
    return -r;
  ilock(f->ip);
  uio->uio_offset = *offp;
  if ((r = readi_uio(f->ip, uio)) > 0)
    *offp += r;
  iunlock(f->ip);
  if (held)
    uio_unhold(uio);
  return r < 0 ? (r < -FNU_ERR_LAST ? -FNU_ERR_IO : r) : r;
}

int fileread_uio(struct file *f, struct uio *uio, uint64_t *offp) {
  int flags = (f->flags & FNU_OPEN_NOWAIT) ? FNU_MSG_NOWAIT : 0;

  if (f->type == FD_EVENTQ)
    return -FNU_ERR_NODEVICE;
  if (f->readable == 0)
    return -FNU_ERR_BADHANDLE;
  if (would_block(f, FNU_SEL_READ))
    return -FNU_ERR_WOULDBLOCK;
  if (f->type == FD_SOCKET)
    return soreceive(f->sock, uio, 0, 0, &flags);
  if (f->type == FD_PIPE)
    return piperead(f->pipe, uio, (f->flags & FNU_OPEN_NOWAIT) != 0);
  if (f->type == FD_INODE)
    return vn_transfer(f, uio, offp, vn_read);
  panic("fileread");
}

int fileread_off(struct file *f, char *addr, int n, uint64_t *offp) {
  struct kiovec iov;
  struct uio uio;

  uio_prepare(&uio, &iov, addr, (size_t)n, *offp, UIO_SYSSPACE, UIO_READ);
  return fileread_uio(f, &uio, offp);
}

int fileread(struct file *f, char *addr, int n) {
  return fileread_off(f, addr, n, &f->off);
}

int fileread_user_off(struct file *f, uintptr_t uaddr, int n, uint64_t *offp) {
  struct kiovec iov;
  struct uio uio;

  if (n < 0)
    return -FNU_ERR_INVALID;
  uio_prepare(&uio, &iov, (char *)uaddr, (size_t)n, *offp, UIO_USERSPACE,
              UIO_READ);
  return fileread_uio(f, &uio, offp);
}

int fileread_user(struct file *f, uintptr_t uaddr, int n) {
  return fileread_user_off(f, uaddr, n, &f->off);
}

static int vn_write(struct file *f, struct uio *uio, uint64_t *offp) {
  size_t want = uio->uio_resid;
  int append = (f->flags & FNU_OPEN_APPEND) && offp == &f->off;
  int r, held;

  if (*offp + want < *offp)
    return -FNU_ERR_TOOLARGE;
  if (want == 0 && f->ip->type != FNU_TYPE_DEVICE)
    return 0;
  if ((held = f->ip->type != FNU_TYPE_DEVICE) && (r = uio_hold(uio)) != 0)
    return -r;
  ilock(f->ip);
  // FNU_OPEN_APPEND
  if (append)
    *offp = f->ip->size;
  uio->uio_offset = *offp;
  if (f->ip->type == FNU_TYPE_FILE && (f->ip->flags & FNU_APPEND) &&
      *offp != f->ip->size)
    r = -FNU_ERR_NOTPERMITTED;
  else if ((r = writei_uio(f->ip, uio)) > 0)
    *offp += r;
  iunlock(f->ip);
  if (held)
    uio_unhold(uio);
  return r;
}

int filewrite_uio(struct file *f, struct uio *uio, uint64_t *offp) {
  size_t want = uio->uio_resid;

  if (f->writable == 0)
    return -FNU_ERR_BADHANDLE;
  if (would_block(f, FNU_SEL_WRITE))
    return -FNU_ERR_WOULDBLOCK;
  if (f->type == FD_SOCKET)
    return sosend(f->sock, uio, 0, 0,
                  (f->flags & FNU_OPEN_NOWAIT) ? FNU_MSG_NOWAIT : 0);
  if (f->type == FD_INODE &&
      curproc->p_p->rlim[FNU_LIMIT_FILESIZE].cur != FNU_LIMIT_NONE &&
      (uint64_t)*offp + want > curproc->p_p->rlim[FNU_LIMIT_FILESIZE].cur) {
    sig_post(curproc, FNU_SIG_FILELIMIT, 0);
    return -FNU_ERR_TOOLARGE;
  }
  if (f->type == FD_PIPE)
    return pipewrite(f->pipe, uio, (f->flags & FNU_OPEN_NOWAIT) != 0);
  if (f->type == FD_INODE)
    return vn_transfer(f, uio, offp, vn_write);
  panic("filewrite");
}

int filewrite_off(struct file *f, char *addr, int n, uint64_t *offp) {
  struct kiovec iov;
  struct uio uio;

  uio_prepare(&uio, &iov, addr, (size_t)n, *offp, UIO_SYSSPACE, UIO_WRITE);
  return filewrite_uio(f, &uio, offp);
}

int filewrite(struct file *f, char *addr, int n) {
  return filewrite_off(f, addr, n, &f->off);
}

int filewrite_user_off(struct file *f, uintptr_t uaddr, int n, uint64_t *offp) {
  struct kiovec iov;
  struct uio uio;

  if (n < 0)
    return -FNU_ERR_INVALID;
  uio_prepare(&uio, &iov, (char *)uaddr, (size_t)n, *offp, UIO_USERSPACE,
              UIO_WRITE);
  return filewrite_uio(f, &uio, offp);
}

int filewrite_user(struct file *f, uintptr_t uaddr, int n) {
  return filewrite_user_off(f, uaddr, n, &f->off);
}
