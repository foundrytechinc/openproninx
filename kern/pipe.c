// PIPES
#include "inc/abi.h"
#include "inc/stat.h"
#include "defs.h"
#include "file.h"
#include "proc.h"
#include "sleeplock.h"
#include "mutex.h"
#include "uio.h"

#define PIPESIZE 16384
#define BIGPIPESIZE 65536
#define MINPIPESIZE (PIPESIZE / 3)
#define LIMITBIGPIPES 32
#define PIPEPAGES (BIGPIPESIZE / PGSIZE)

struct pipe {
  struct mutex lock;
  int iolock;
  int iowant;
  char *page[PIPEPAGES];
  uint size;
  uint in, out, cnt;
  int readopen;
  int writeopen;          // and the writing one
  int wantr, wantw;
  struct selinfo rsel, wsel;
  struct fnu_timespec atime, mtime, ctime;
  uint uid, gid;
  struct pipe *fnext;
  uint fdev, finum;
  int frefs;
};

static struct {
  struct mutex lock;
  uint nbig;
} bigpipes;

void pipeinit(void) { mtx_init(&bigpipes.lock, "bigpipe"); }

int pipe_nbig(void) { return (int)bigpipes.nbig; }
int pipe_maxbig(void) { return LIMITBIGPIPES; }

int pipe_bufsize(void) { return FNU_PIPE_ATOMIC; }

static void pipefree(struct pipe *p) {
  int i;

  for (i = 0; i < PIPEPAGES; i++)
    if (p->page[i] != 0)
      page_free(p->page[i]);
  if (p->size > PIPESIZE) {
    mtx_enter(&bigpipes.lock);
    bigpipes.nbig--;
    mtx_leave(&bigpipes.lock);
  }
  page_free((char *)p);
}

static struct pipe *pipe_new(char *what) {
  struct pipe *p;

  if ((p = (struct pipe *)page_alloc()) == 0)
    return 0;
  memset(p, 0, sizeof(*p));
  p->size = PIPESIZE;
  mtx_init(&p->lock, what);
  time_now(&p->ctime);
  p->atime = p->mtime = p->ctime;
  p->uid = curproc != 0 ? curproc->p_p->uid : 0;
  p->gid = curproc != 0 ? curproc->p_p->gid : 0;
  return p;
}

int pipe_ready(struct pipe *p, int want) {
  int ready = 0;

  mtx_enter(&p->lock);
  if ((want & FNU_READY_IN) && p->cnt != 0)
    ready |= FNU_READY_IN;
  if (p->writeopen == 0)
    ready |= (want & FNU_READY_HUP) ? FNU_READY_HUP : FNU_READY_IN;
  if ((want & FNU_READY_OUT) && p->size - p->cnt >= FNU_PIPE_ATOMIC)
    ready |= FNU_READY_OUT;
  if (p->readopen == 0)
    ready |= (want & FNU_READY_HUP) ? FNU_READY_ERR : FNU_READY_OUT;
  if ((want & FNU_READY_IN) && !(ready & (FNU_READY_IN | FNU_READY_HUP)))
    selrecord(&p->rsel);
  if ((want & FNU_READY_OUT) && !(ready & (FNU_READY_OUT | FNU_READY_ERR)))
    selrecord(&p->wsel);
  mtx_leave(&p->lock);
  return ready;
}

int64_t pipe_avail(struct pipe *p, int write, int *eof) {
  int64_t n;

  mtx_enter(&p->lock);
  if (write) {
    n = p->size - p->cnt;
    *eof = p->readopen == 0;
  } else {
    n = p->cnt;
    *eof = p->writeopen == 0;
  }
  mtx_leave(&p->lock);
  return n;
}

// p->lock held
static int pipe_iolock(struct pipe *p) {
  while (p->iolock) {
    p->iowant = 1;
    if (proc_interrupted())
      return FNU_ERR_INTERRUPTED;
    msleep(p, &p->lock, PRIBIO | PCATCH, "pipe", 0);
  }
  p->iolock = 1;
  return 0;
}

static void pipe_iounlock(struct pipe *p) {
  p->iolock = 0;
  if (p->iowant) {
    p->iowant = 0;
    wakeup(p);
  }
}

static int pipe_iosleep(struct pipe *p, void *chan, int timo) {
  pipe_iounlock(p);
  if (proc_interrupted())
    return FNU_ERR_INTERRUPTED;
  if (msleep(chan, &p->lock, PRIBIO | PCATCH, "pipe", timo) == FNU_ERR_WOULDBLOCK)
    return FNU_ERR_WOULDBLOCK;
  return pipe_iolock(p);
}

void pipe_stat(struct pipe *p, struct fnu_stat *st) {
  memset(st, 0, sizeof(*st));
  mtx_enter(&p->lock);
  st->type = FNU_TYPE_FIFO;
  st->size = p->cnt;
  st->uid = p->uid;
  st->gid = p->gid;
  st->blocks = (p->cnt + 511) / 512;
  st->blksize = p->size;
  st->atime = p->atime;
  st->mtime = p->mtime;
  st->ctime = p->ctime;
  st->birthtime = p->ctime;
  mtx_leave(&p->lock);
}

int pipe_nread(struct pipe *p) {
  int n;

  mtx_enter(&p->lock);
  n = (int)p->cnt;
  mtx_leave(&p->lock);
  return n;
}

struct pipe *pipe_create(void) {
  struct pipe *p = pipe_new("sockpipe");

  if (p != 0)
    p->readopen = p->writeopen = 1;
  return p;
}

int pipealloc(struct file **f0, struct file **f1) {
  struct pipe *p;

  *f0 = *f1 = 0;
  if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if ((p = pipe_new("pipe")) == 0)
    goto bad;
  p->readopen = 1;
  p->writeopen = 1;
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = p;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = p;
  return 0;

bad:
  if (*f0)
    fileclose(*f0);
  if (*f1)
    fileclose(*f1);
  return -1;
}

void pipeclose(struct pipe *p, int writable) {
  mtx_enter(&p->lock);
  if (writable) {
    if (p->writeopen > 0)
      p->writeopen--;
  } else {
    if (p->readopen > 0)
      p->readopen--;
  }
  wakeup(&p->wantr);
  wakeup(&p->wantw);
  selwakeup(&p->rsel);
  selwakeup(&p->wsel);
  if (p->readopen == 0 && p->writeopen == 0) {
    mtx_leave(&p->lock);
    pipefree(p);
  } else
    mtx_leave(&p->lock);
}

static char *pipe_page(struct pipe *p, uint at) {
  char *pg;

  mtx_enter(&p->lock);
  pg = p->page[at / PGSIZE];
  mtx_leave(&p->lock);
  if (pg != 0)
    return pg;
  if ((pg = page_alloc()) == 0)
    return 0;
  mtx_enter(&p->lock);
  p->page[at / PGSIZE] = pg;
  mtx_leave(&p->lock);
  return pg;
}

int pipewrite_timo(struct pipe *p, struct uio *uio, int nonblock, int timo) {
  size_t orig = uio->uio_resid;
  int error = 0, eof = 0, locked;
  struct fnu_timespec now;

  mtx_enter(&p->lock);
  if (p->readopen == 0) {
    mtx_leave(&p->lock);
    sig_post(curproc, FNU_SIG_BROKENPIPE, 0);
    return -FNU_ERR_BROKENPIPE;
  }
  if ((error = pipe_iolock(p)) != 0) {
    mtx_leave(&p->lock);
    return -error;
  }
  locked = 1;

  if (uio->uio_resid > PIPESIZE && p->size <= PIPESIZE && p->cnt == 0) {
    mtx_enter(&bigpipes.lock);
    if (bigpipes.nbig < LIMITBIGPIPES) {
      bigpipes.nbig++;
      p->size = BIGPIPESIZE;
    }
    mtx_leave(&bigpipes.lock);
  }

  while (uio->uio_resid > 0) {
    size_t space;

    if (p->readopen == 0) {
      eof = 1;
      break;
    }
    space = p->size - p->cnt;
    if (space < uio->uio_resid && orig <= FNU_PIPE_ATOMIC)
      space = 0;

    if (space > 0) {
      size_t size = space > uio->uio_resid ? uio->uio_resid : space;
      uint at = p->in, left = (uint)size;

      mtx_leave(&p->lock);
      while (left > 0 && error == 0) {
        uint run = PGSIZE - at % PGSIZE;
        char *pg;

        if (run > left)
          run = left;
        if ((pg = pipe_page(p, at)) == 0) {
          error = FNU_ERR_NOMEMORY;
          break;
        }
        error = uiomove(pg + at % PGSIZE, run, uio);
        at = (at + run) % p->size;
        left -= run;
      }
      mtx_enter(&p->lock);
      if (error != 0)
        break;
      p->in = at;
      p->cnt += (uint)size;
    } else {
      if (p->wantr) {
        p->wantr = 0;
        wakeup(&p->wantr);
      }
      selwakeup(&p->rsel);
      if (nonblock) {
        error = FNU_ERR_WOULDBLOCK;
        break;
      }
      p->wantw = 1;
      if ((error = pipe_iosleep(p, &p->wantw, timo)) != 0) {
        locked = 0;
        break;
      }
    }
  }
  if (locked)
    pipe_iounlock(p);

  if (p->cnt > 0 && p->wantr) {
    p->wantr = 0;
    wakeup(&p->wantr);
  }
  if (eof && uio->uio_resid == orig)
    error = FNU_ERR_BROKENPIPE;
  if (p->cnt > 0)
    selwakeup(&p->rsel);
  mtx_leave(&p->lock);

  if (uio->uio_resid != orig) {
    time_now(&now);
    mtx_enter(&p->lock);
    p->mtime = now;
    mtx_leave(&p->lock);
    return (int)(orig - uio->uio_resid);
  }
  if (error == FNU_ERR_BROKENPIPE)
    sig_post(curproc, FNU_SIG_BROKENPIPE, 0);
  return error != 0 ? -error : 0;
}

int pipewrite(struct pipe *p, struct uio *uio, int nonblock) {
  return pipewrite_timo(p, uio, nonblock, 0);
}

int piperead_timo(struct pipe *p, struct uio *uio, int nonblock, int timo) {
  size_t orig = uio->uio_resid;
  int error = 0, locked;
  struct fnu_timespec now;

  mtx_enter(&p->lock);
  if ((error = pipe_iolock(p)) != 0) {
    mtx_leave(&p->lock);
    return -error;
  }
  locked = 1;
  while (uio->uio_resid > 0) {
    if (p->cnt > 0) {
      uint size = p->size - p->out, run;
      char *pg;

      run = PGSIZE - p->out % PGSIZE;
      if (size > run)
        size = run;
      if (size > p->cnt)
        size = p->cnt;
      if (size > uio->uio_resid)
        size = (uint)uio->uio_resid;
      pg = p->page[p->out / PGSIZE];
      mtx_leave(&p->lock);
      error = uiomove(pg + p->out % PGSIZE, size, uio);
      mtx_enter(&p->lock);
      if (error != 0)
        break;
      p->out += size;
      if (p->out >= p->size)
        p->out = 0;
      p->cnt -= size;
      if (p->cnt == 0)
        p->in = p->out = 0;
    } else {
      if (p->writeopen == 0)
        break;
      if (p->wantw) {
        p->wantw = 0;
        wakeup(&p->wantw);
      }
      if (uio->uio_resid != orig)
        break;
      if (nonblock) {
        error = FNU_ERR_WOULDBLOCK;
        break;
      }
      p->wantr = 1;
      if ((error = pipe_iosleep(p, &p->wantr, timo)) != 0) {
        locked = 0;
        break;
      }
    }
  }
  if (locked)
    pipe_iounlock(p);
  if (p->cnt < MINPIPESIZE && p->wantw) {
    p->wantw = 0;
    wakeup(&p->wantw);
  }
  if (p->size - p->cnt >= FNU_PIPE_ATOMIC)
    selwakeup(&p->wsel);
  mtx_leave(&p->lock);

  if (uio->uio_resid != orig) {
    time_now(&now);
    mtx_enter(&p->lock);
    p->atime = now;
    mtx_leave(&p->lock);
    return (int)(orig - uio->uio_resid);
  }
  return error != 0 ? -error : 0;
}

int piperead(struct pipe *p, struct uio *uio, int nonblock) {
  return piperead_timo(p, uio, nonblock, 0);
}

static struct {
  struct mutex lock;
  struct pipe *list;
} fifos;

void fifoinit(void) { mtx_init(&fifos.lock, "fifo"); }

// fifos.lock held
static struct pipe *fifo_find(uint dev, uint inum) {
  struct pipe *p;

  for (p = fifos.list; p != 0; p = p->fnext)
    if (p->fdev == dev && p->finum == inum)
      return p;
  return 0;
}

int fifo_open(uint dev, uint inum, int readable, int writable, int nonblock,
              struct pipe **out) {
  struct pipe *p, *spare;

  if ((spare = pipe_new("fifo")) == 0)
    return FNU_ERR_NOMEMORY;
  mtx_enter(&fifos.lock);
  if ((p = fifo_find(dev, inum)) == 0) {
    p = spare;
    spare = 0;
    p->fdev = dev;
    p->finum = inum;
    p->fnext = fifos.list;
    fifos.list = p;
  }
  p->frefs++;
  mtx_leave(&fifos.lock);
  if (spare != 0)
    pipefree(spare);

  mtx_enter(&p->lock);
  if (readable)
    p->readopen++;
  if (writable)
    p->writeopen++;
  wakeup(&p->wantr);
  wakeup(&p->wantw);
  selwakeup(&p->rsel);
  selwakeup(&p->wsel);
  if (writable && !readable && p->readopen == 0 && nonblock) {
    mtx_leave(&p->lock);
    fifo_close(p, readable, writable);
    return FNU_ERR_NODEVICE;
  }
  if (readable && !writable && !nonblock) {
    while (p->writeopen == 0) {
      if (proc_interrupted()) {
        mtx_leave(&p->lock);
        fifo_close(p, readable, writable);
        return FNU_ERR_INTERRUPTED;
      }
      msleep(&p->wantr, &p->lock, PSOCK | PCATCH, "pipe", 0);
    }
  }
  if (writable && !readable && !nonblock) {
    while (p->readopen == 0) {
      if (proc_interrupted()) {
        mtx_leave(&p->lock);
        fifo_close(p, readable, writable);
        return FNU_ERR_INTERRUPTED;
      }
      msleep(&p->wantw, &p->lock, PSOCK | PCATCH, "pipe", 0);
    }
  }
  mtx_leave(&p->lock);
  *out = p;
  return 0;
}

void fifo_close(struct pipe *p, int readable, int writable) {
  struct pipe **pp;
  int last = 0;

  mtx_enter(&fifos.lock);
  if (--p->frefs == 0) {
    for (pp = &fifos.list; *pp != 0; pp = &(*pp)->fnext)
      if (*pp == p) {
        *pp = p->fnext;
        break;
      }
    last = 1;
  }
  mtx_leave(&fifos.lock);

  mtx_enter(&p->lock);
  if (readable && p->readopen > 0)
    p->readopen--;
  if (writable && p->writeopen > 0)
    p->writeopen--;
  wakeup(&p->wantr);
  wakeup(&p->wantw);
  selwakeup(&p->rsel);
  selwakeup(&p->wsel);
  mtx_leave(&p->lock);
  if (last)
    pipefree(p);
}
