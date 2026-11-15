// THE DESCRIPTOR TABLE
#include "defs.h"
#include "file.h"
#include "fs.h"
#include "malloc.h"
#include "inc/abi.h"
#include "inc/open.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"

void fd_init(struct process *pr) {
  mtx_init(&pr->fdlock, "filedesc");
  pr->ofile = pr->ofile0;
  pr->ofileflags = pr->ofileflags0;
  pr->nofile = NDFILE;
  memset(pr->ofile0, 0, sizeof(pr->ofile0));
  memset(pr->ofileflags0, 0, sizeof(pr->ofileflags0));
}

void fd_reset(struct process *pr) {
  if (pr->ofile != 0 && pr->ofile != pr->ofile0)
    free(pr->ofile, M_FILE);
  pr->ofile = pr->ofile0;
  pr->ofileflags = pr->ofileflags0;
  pr->nofile = NDFILE;
  memset(pr->ofile0, 0, sizeof(pr->ofile0));
  memset(pr->ofileflags0, 0, sizeof(pr->ofileflags0));
}

int fd_limit(void) {
  int limit = maxfilesperproc;

  if (curproc->p_p->rlim[FNU_LIMIT_OPENFILES].cur != FNU_LIMIT_NONE &&
      curproc->p_p->rlim[FNU_LIMIT_OPENFILES].cur < (uint64_t)limit)
    limit = (int)curproc->p_p->rlim[FNU_LIMIT_OPENFILES].cur;
  return limit;
}

// lock held
static int fd_grow_locked(struct process *pr, int need) {
  struct file **nf;
  uchar *nfl, *raw;
  void *old;
  int n;

  while (need >= pr->nofile) {
    if (need >= maxfilesperproc)
      return -1;
    for (n = pr->nofile; n <= need; n *= 2)
      ;
    if (n > maxfilesperproc)
      n = maxfilesperproc;
    mtx_leave(&pr->fdlock);
    raw = malloc((uint64_t)n * (sizeof(*nf) + sizeof(*nfl)), M_FILE,
                 M_WAITOK | M_CANFAIL);
    mtx_enter(&pr->fdlock);
    if (raw == 0)
      return -1;
    if (n <= pr->nofile) {
      mtx_leave(&pr->fdlock);
      free(raw, M_FILE);
      mtx_enter(&pr->fdlock);
      continue;
    }
    nf = (struct file **)raw;
    nfl = raw + (uint64_t)n * sizeof(*nf);
    memset(raw, 0, (uint64_t)n * (sizeof(*nf) + sizeof(*nfl)));
    memmove(nf, pr->ofile, (uint64_t)pr->nofile * sizeof(*nf));
    memmove(nfl, pr->ofileflags, (uint64_t)pr->nofile);
    old = pr->ofile != pr->ofile0 ? pr->ofile : 0;
    pr->ofile = nf;
    pr->ofileflags = nfl;
    pr->nofile = n;
    if (old != 0) {
      mtx_leave(&pr->fdlock);
      free(old, M_FILE);
      mtx_enter(&pr->fdlock);
    }
  }
  return 0;
}

struct file *fd_getfile(struct process *pr, int fd) {
  struct file *f = 0;

  mtx_enter(&pr->fdlock);
  if (fd >= 0 && fd < pr->nofile && (f = pr->ofile[fd]) != 0)
    filedup(f);
  mtx_leave(&pr->fdlock);
  return f;
}

int at_dir(int fd, struct inode **dp) {
  struct file *f;
  int error = 0;

  *dp = 0;
  if (fd == FNU_FD_CWD)
    return 0;
  if ((f = fd_getfile(curproc->p_p, fd)) == 0)
    return FNU_ERR_BADHANDLE;
  if (f->type != FD_INODE || f->ip == 0 || f->ip->type != FNU_TYPE_DIR)
    error = FNU_ERR_NOTDIR;
  else
    *dp = idup(f->ip);
  FRELE(f);
  return error;
}

int fd_alloc_at(struct process *pr, struct file *f, int min, int flags) {
  int fd, limit = fd_limit();

  mtx_enter(&pr->fdlock);
  for (fd = min; fd < limit; fd++) {
    if (fd >= pr->nofile && fd_grow_locked(pr, fd) < 0)
      break;
    if (pr->ofile[fd] == 0) {
      pr->ofile[fd] = f;
      pr->ofileflags[fd] = (uchar)flags;
      mtx_leave(&pr->fdlock);
      return fd;
    }
  }
  mtx_leave(&pr->fdlock);
  return -1;
}

int fdalloc(struct file *f) { return fd_alloc_at(curproc->p_p, f, 0, 0); }

int fd_set(struct process *pr, int fd, struct file *f, int flags,
           struct file **old) {
  *old = 0;
  mtx_enter(&pr->fdlock);
  if (fd >= pr->nofile && fd_grow_locked(pr, fd) < 0) {
    mtx_leave(&pr->fdlock);
    return -1;
  }
  *old = pr->ofile[fd];
  pr->ofile[fd] = f;
  pr->ofileflags[fd] = (uchar)flags;
  mtx_leave(&pr->fdlock);
  return 0;
}

struct file *fd_take(struct process *pr, int fd) {
  struct file *f = 0;

  mtx_enter(&pr->fdlock);
  if (fd >= 0 && fd < pr->nofile && (f = pr->ofile[fd]) != 0)
    pr->ofile[fd] = 0;
  mtx_leave(&pr->fdlock);
  return f;
}

void fd_untake(struct process *pr, int fd, struct file *f) {
  int took = 0;

  mtx_enter(&pr->fdlock);
  if (fd >= 0 && fd < pr->nofile && pr->ofile[fd] == f) {
    pr->ofile[fd] = 0;
    took = 1;
  }
  mtx_leave(&pr->fdlock);
  if (took)
    fileclose(f);
}

int fd_getflags(struct process *pr, int fd) {
  int flags = -1;

  mtx_enter(&pr->fdlock);
  if (fd >= 0 && fd < pr->nofile && pr->ofile[fd] != 0)
    flags = pr->ofileflags[fd];
  mtx_leave(&pr->fdlock);
  return flags;
}

int fd_setflags(struct process *pr, int fd, int set, int clear) {
  int r = -1;

  mtx_enter(&pr->fdlock);
  if (fd >= 0 && fd < pr->nofile && pr->ofile[fd] != 0) {
    pr->ofileflags[fd] = (uchar)((pr->ofileflags[fd] & ~clear) | set);
    r = 0;
  }
  mtx_leave(&pr->fdlock);
  return r;
}

struct inode *fd_cwd(struct process *pr) {
  struct inode *ip;

  mtx_enter(&pr->fdlock);
  ip = pr->cwd != 0 ? idup(pr->cwd) : 0;
  mtx_leave(&pr->fdlock);
  return ip;
}

void fd_setcwd(struct process *pr, struct inode *ip) {
  struct inode *old;

  mtx_enter(&pr->fdlock);
  old = pr->cwd;
  pr->cwd = ip;
  mtx_leave(&pr->fdlock);
  if (old != 0)
    iput(old);
}

int fd_copy(struct process *from, struct process *to) {
  int i;

  mtx_enter(&to->fdlock);
  mtx_enter(&from->fdlock);
  while (to->nofile < from->nofile) {
    int need = from->nofile - 1;

    mtx_leave(&from->fdlock);
    if (fd_grow_locked(to, need) < 0) {
      mtx_leave(&to->fdlock);
      return -1;
    }
    mtx_enter(&from->fdlock);
  }
  for (i = 0; i < from->nofile; i++)
    if (from->ofile[i] && from->ofile[i]->type != FD_EVENTQ) {
      to->ofile[i] = filedup(from->ofile[i]);
      to->ofileflags[i] = from->ofileflags[i];
    }
  mtx_leave(&from->fdlock);
  mtx_leave(&to->fdlock);
  return 0;
}

void fd_closeall(struct process *pr, int exec_only) {
  struct file *f;
  int fd;

  for (fd = 0; fd < pr->nofile; fd++) {
    mtx_enter(&pr->fdlock);
    f = pr->ofile[fd];
    if (f != 0 && (!exec_only || (pr->ofileflags[fd] & FNU_FD_CLOSEEXEC)))
      pr->ofile[fd] = 0;
    else
      f = 0;
    mtx_leave(&pr->fdlock);
    if (f != 0)
      fileclose(f);
  }
}
