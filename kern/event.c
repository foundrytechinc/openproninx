// EVENT QUEUES
#include "callout.h"
#include "defs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/error.h"
#include "inc/signal.h"
#include "inc/stat.h"
#include "malloc.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"

struct knote {
  struct knote *next;      // in its queue
  struct knote *onext;     // on what it watches
  struct eventq *eq;
  struct fnu_event ev;
  uint32_t fired;
  int64_t count;
  int64_t status;
  int64_t last;
  int disabled;
  struct file *fp;
  struct inode *ip;        // a file note's, held
  struct process *pr;
  struct callout co;
};

struct eventq {
  struct sleeplock lock;
  struct process *owner;
  struct knote *list;
  int count;
  int polling;
  struct selinfo sel;
};

static struct mutex ev_lock;

void eventinit(void) { mtx_init(&ev_lock, "event"); }

static void unlink_from(struct knote **head, struct knote *kn) {
  struct knote **pp;

  for (pp = head; *pp != 0; pp = &(*pp)->onext)
    if (*pp == kn) {
      *pp = kn->onext;
      return;
    }
}

// FILE
void knote_inode(struct inode *ip, uint32_t hint) {
  struct knote *kn;
  int any = 0;

  if (__atomic_load_n(&ip->klist, __ATOMIC_RELAXED) == 0)
    return;
  mtx_enter(&ev_lock);
  for (kn = ip->klist; kn != 0; kn = kn->onext) {
    any = kn->ev.filter != FNU_EVF_FILE;
    if (!any && (kn->ev.fflags & hint)) {
      kn->fired |= kn->ev.fflags & hint;
      any = 1;
    }
    if (any)
      selwakeup(&kn->eq->sel);
  }
  mtx_leave(&ev_lock);
}

// PROC
void knote_proc(struct process *pr, uint32_t hint, int64_t status) {
  struct knote *kn, *next;

  if (pr->klist == 0)
    return;
  mtx_enter(&ev_lock);
  for (kn = pr->klist; kn != 0; kn = next) {
    next = kn->onext;
    kn->fired |= hint;
    selwakeup(&kn->eq->sel);
    if (hint & FNU_NOTE_EXIT) {
      kn->status = status;
      kn->pr = 0;
      kn->onext = 0;
    }
  }
  if (hint & FNU_NOTE_EXIT)
    pr->klist = 0;
  mtx_leave(&ev_lock);
}

// SIGNAL
// proctree_lock held
void knote_signal(struct process *pr, int sig) {
  struct knote *kn;

  if (pr->sigklist == 0)
    return;
  mtx_enter(&ev_lock);
  for (kn = pr->sigklist; kn != 0; kn = kn->onext)
    if (kn->ev.ident == (uint64_t)sig) {
      kn->count++;
      selwakeup(&kn->eq->sel);
    }
  mtx_leave(&ev_lock);
}

static int timer_ticks(const struct knote *kn) {
  int64_t ms, d = kn->ev.data;

  switch (kn->ev.fflags & (FNU_NOTE_SECONDS | FNU_NOTE_MSECONDS | FNU_NOTE_USECONDS |
                           FNU_NOTE_NSECONDS)) {
  case FNU_NOTE_SECONDS: ms = d > __INT64_MAX__ / 1000 ? __INT64_MAX__ : d * 1000; break;
  case FNU_NOTE_USECONDS: ms = (d + 999) / 1000; break;
  case FNU_NOTE_NSECONDS: ms = (d + 999999) / 1000000; break;
  default: ms = d;
  }
  return mstohz(ms > 0x7fffffff ? 0x7fffffff : ms);
}

static void timer_fire(void *arg) {
  struct knote *kn = arg;

  mtx_enter(&ev_lock);
  kn->count++;
  selwakeup(&kn->eq->sel);
  mtx_leave(&ev_lock);
  if (!(kn->ev.flags & FNU_EV_ONESHOT))
    callout_reset(&kn->co, timer_ticks(kn), timer_fire, kn);
}

// lock held
static void detach(struct knote *kn) {
  struct inode *ip = 0;

  if (kn->ev.filter == FNU_EVF_TIMER)
    callout_stop(&kn->co);
  mtx_enter(&ev_lock);
  switch (kn->ev.filter) {
  case FNU_EVF_READ:
  case FNU_EVF_WRITE:
  case FNU_EVF_FILE:
    if (kn->ip != 0) {
      unlink_from(&kn->ip->klist, kn);
      ip = kn->ip;
    }
    break;
  case FNU_EVF_PROC:
    if (kn->pr != 0)
      unlink_from(&kn->pr->klist, kn);
    break;
  case FNU_EVF_SIGNAL:
    unlink_from(&kn->pr->sigklist, kn);
    break;
  }
  mtx_leave(&ev_lock);
  if (ip != 0)
    iput(ip);
}

static void drop(struct eventq *eq, struct knote *kn) {
  struct knote **pp;

  for (pp = &eq->list; *pp != 0; pp = &(*pp)->next)
    if (*pp == kn) {
      *pp = kn->next;
      break;
    }
  detach(kn);
  eq->count--;
  free(kn, M_FILE);
}

static struct knote *find(struct eventq *eq, uint64_t ident, int filter) {
  struct knote *kn;

  for (kn = eq->list; kn != 0; kn = kn->next)
    if (kn->ev.ident == ident && kn->ev.filter == filter)
      return kn;
  return 0;
}

// proctree_lock held
static int proc_attach(struct process *pr, void *arg) {
  struct knote *kn = arg;

  if (pr->ps_flags & PS_ZOMBIE)
    return -1;
  mtx_enter(&ev_lock);
  kn->pr = pr;
  kn->onext = pr->klist;
  pr->klist = kn;
  mtx_leave(&ev_lock);
  return 0;
}

static int attach(struct eventq *eq, struct knote *kn) {
  struct file *f;

  switch (kn->ev.filter) {
  case FNU_EVF_READ:
  case FNU_EVF_WRITE:
  case FNU_EVF_FILE:
    if (kn->ev.ident > 0x7fffffff ||
        (f = fd_getfile(eq->owner, (int)kn->ev.ident)) == 0)
      return FNU_ERR_BADHANDLE;
    kn->fp = f;
    if (kn->ev.filter == FNU_EVF_FILE &&
        (f->ip == 0 || (f->type != FD_INODE && f->type != FD_PIPE))) {
      FRELE(f);
      return FNU_ERR_INVALID;
    }
    if (kn->ev.filter == FNU_EVF_FILE ||
        (f->type == FD_INODE && f->ip != 0 &&
         (f->ip->type == FNU_TYPE_FILE || f->ip->type == FNU_TYPE_DIR))) {
      kn->ip = idup(f->ip);
      mtx_enter(&ev_lock);
      kn->onext = kn->ip->klist;
      kn->ip->klist = kn;
      mtx_leave(&ev_lock);
    }
    FRELE(f);
    return 0;
  case FNU_EVF_PROC:
    if (kn->ev.fflags & ~(FNU_NOTE_EXIT | FNU_NOTE_FORK | FNU_NOTE_EXEC))
      return FNU_ERR_INVALID;
    if (kn->ev.ident > 0x7fffffff ||
        process_with((pid_t)kn->ev.ident, proc_attach, kn) != 0)
      return FNU_ERR_NOPROCESS;
    return 0;
  case FNU_EVF_SIGNAL:
    if (kn->ev.ident == 0 || kn->ev.ident >= FNU_SIG_COUNT)
      return FNU_ERR_INVALID;
    mtx_enter(&ev_lock);
    kn->pr = eq->owner;
    kn->onext = eq->owner->sigklist;
    eq->owner->sigklist = kn;
    mtx_leave(&ev_lock);
    return 0;
  case FNU_EVF_TIMER:
    if (kn->ev.data < 0)
      return FNU_ERR_INVALID;
    callout_init(&kn->co);
    callout_reset(&kn->co, timer_ticks(kn), timer_fire, kn);
    return 0;
  case FNU_EVF_USER:
    // userspace wakeup (nginx EVFILT_USER); ident is a private id
    return 0;
  default:
    return FNU_ERR_INVALID;
  }
}

static int change(struct eventq *eq, const struct fnu_event *c) {
  struct knote *kn = find(eq, c->ident, c->filter);
  int error;

  if (c->flags & ~(FNU_EV_ADD | FNU_EV_DELETE | FNU_EV_ENABLE | FNU_EV_DISABLE |
                   FNU_EV_ONESHOT | FNU_EV_CLEAR | FNU_EV_RECEIPT | FNU_EV_DISPATCH))
    return FNU_ERR_INVALID;
  if (c->flags & FNU_EV_DELETE) {
    if (kn == 0)
      return FNU_ERR_NOTFOUND;
    drop(eq, kn);
    return 0;
  }
  if (kn == 0) {
    if (!(c->flags & FNU_EV_ADD))
      return FNU_ERR_NOTFOUND;
    if (eq->count >= FNU_EVENTS_MAX)
      return FNU_ERR_NOSPACE;
    kn = malloc(sizeof(*kn), M_FILE, M_WAITOK | M_ZERO);
    kn->eq = eq;
    kn->ev = *c;
    kn->ev.flags &= FNU_EV_ONESHOT | FNU_EV_CLEAR | FNU_EV_DISPATCH;
    if ((error = attach(eq, kn)) != 0) {
      free(kn, M_FILE);
      return error;
    }
    kn->next = eq->list;
    eq->list = kn;
    eq->count++;
  } else if (c->flags & FNU_EV_ADD) {
    mtx_enter(&ev_lock);
    kn->ev.fflags = c->fflags;
    kn->ev.udata = c->udata;
    kn->ev.flags = c->flags & (FNU_EV_ONESHOT | FNU_EV_CLEAR | FNU_EV_DISPATCH);
    mtx_leave(&ev_lock);
    if (kn->ev.filter == FNU_EVF_TIMER) {
      kn->ev.data = c->data;
      callout_reset(&kn->co, timer_ticks(kn), timer_fire, kn);
    }
  }
  if (c->flags & FNU_EV_DISABLE)
    kn->disabled = 1;
  if (c->flags & FNU_EV_ENABLE)
    kn->disabled = 0;
  // EVFILT_USER: NOTE_TRIGGER increments and wakes the waiter
  if (kn->ev.filter == FNU_EVF_USER && (c->fflags & FNU_NOTE_TRIGGER)) {
    mtx_enter(&ev_lock);
    kn->count++;
    mtx_leave(&ev_lock);
    selwakeup(&eq->sel);
  }
  return 0;
}

static int fd_state(struct eventq *eq, struct knote *kn, int64_t *data, int *eof) {
  int write = kn->ev.filter == FNU_EVF_WRITE, ready;
  struct file *f;

  *eof = 0;
  if ((f = fd_getfile(eq->owner, (int)kn->ev.ident)) == 0)
    return -1;
  if (f != kn->fp) {
    FRELE(f);
    return -1;
  }
  if (f->type == FD_PIPE)
    *data = pipe_avail(f->pipe, write, eof);
  else if (f->type == FD_SOCKET)
    *data = soavail(f->sock, write, eof);
  else if (f->type == FD_INODE && f->ip != 0 &&
           (f->ip->type == FNU_TYPE_FILE || f->ip->type == FNU_TYPE_DIR))
    *data = write ? 0 : (int64_t)f->ip->size - (int64_t)f->off;
  else {
    ready = file_ready(f, write ? FNU_READY_OUT : FNU_READY_IN);
    *data = ready != 0;
  }
  FRELE(f);
  if (f->type == FD_INODE && f->ip != 0 && f->ip->type == FNU_TYPE_FILE)
    return write || *data != 0;
  return *data > 0 || *eof;
}

static int look(struct eventq *eq, struct knote *kn, struct fnu_event *out) {
  struct fnu_event e = kn->ev;
  int64_t data = 0;
  int eof = 0, active = 0;

  e.flags = kn->ev.flags;
  switch (kn->ev.filter) {
  case FNU_EVF_READ:
  case FNU_EVF_WRITE:
    if ((active = fd_state(eq, kn, &data, &eof)) < 0)
      return -1;
    if (active && (kn->ev.flags & FNU_EV_CLEAR) && data == kn->last && !eof)
      active = 0;
    e.data = data;
    if (eof)
      e.flags |= FNU_EV_EOF;
    break;
  default:
    mtx_enter(&ev_lock);
    if (kn->ev.filter == FNU_EVF_FILE || kn->ev.filter == FNU_EVF_PROC) {
      active = (kn->fired & (kn->ev.fflags | FNU_NOTE_EXIT)) != 0;
      e.fflags = kn->fired & kn->ev.fflags;
      e.data = kn->status;
      if (kn->fired & FNU_NOTE_EXIT)
        e.flags |= FNU_EV_EOF | FNU_EV_ONESHOT;
      if (active && out != 0)
        kn->fired = 0;
    } else {
      active = kn->count > 0;
      e.data = kn->count;
      if (active && out != 0)
        kn->count = 0;
    }
    mtx_leave(&ev_lock);
  }
  if (active && out != 0) {
    e.flags &= FNU_EV_ONESHOT | FNU_EV_CLEAR | FNU_EV_DISPATCH | FNU_EV_EOF;
    *out = e;
    kn->last = data;
    if (kn->ev.flags & FNU_EV_DISPATCH)
      kn->disabled = 1;
  }
  return active;
}

// lock held
static int collect(struct eventq *eq, struct fnu_event *out, int n) {
  struct knote *kn, *next;
  int got = 0, r;

  for (kn = eq->list; kn != 0 && got < n; kn = next) {
    next = kn->next;
    if (kn->disabled)
      continue;
    if ((r = look(eq, kn, &out[got])) < 0) {
      drop(eq, kn);
      continue;
    }
    if (r == 0)
      continue;
    if (out[got].flags & FNU_EV_ONESHOT)
      drop(eq, kn);
    got++;
  }
  return got;
}

int eventq_ready(struct eventq *eq) {
  struct knote *kn;
  int any = 0;

  selrecord(&eq->sel);
  if (!trysleep(&eq->lock))
    return 0;
  if (!eq->polling) {
    eq->polling = 1;
    for (kn = eq->list; kn != 0 && !any; kn = kn->next)
      any = !kn->disabled && look(eq, kn, 0) > 0;
    eq->polling = 0;
  }
  releasesleep(&eq->lock);
  return any;
}

void eventq_close(struct eventq *eq) {
  acquiresleep(&eq->lock);
  while (eq->list != 0)
    drop(eq, eq->list);
  releasesleep(&eq->lock);
  free(eq, M_FILE);
}

int eventq_count(struct eventq *eq) { return eq->count; }

int sys_eventq(struct proc *p, const register_t *uap, register_t *retval) {
  struct eventq *eq;
  struct file *f;
  int fd;

  if ((f = filealloc()) == 0)
    return FNU_ERR_SYSTEMFULL;
  eq = malloc(sizeof(*eq), M_FILE, M_WAITOK | M_ZERO);
  initsleeplock(&eq->lock, "eventq");
  eq->owner = p->p_p;
  f->type = FD_EVENTQ;
  f->eq = eq;
  f->readable = 1;
  if ((fd = fd_alloc_at(p->p_p, f, 0, (int)uap[0] & FNU_FD_CLOSEEXEC)) < 0) {
    fileclose(f);
    return FNU_ERR_TOOMANYOPEN;
  }
  *retval = fd;
  return 0;
}

int sys_events(struct proc *p, const register_t *uap, register_t *retval) {
  int nchanges = (int)uap[2], room = (int)uap[4], i, got = 0, error = 0;
  uint64_t uchanges = uap[1], uout = uap[3], uto = uap[5], left = 0;
  struct fnu_event c, *out = 0;
  struct fnu_timespec to;
  struct eventq *eq;
  struct file *f;
  int forever = uto == 0;
  uint seen;

  if (nchanges < 0 || room < 0 || nchanges > FNU_EVENTS_MAX || room > FNU_EVENTS_MAX)
    return FNU_ERR_INVALID;
  if ((f = fd_getfile(p->p_p, (int)uap[0])) == 0)
    return FNU_ERR_BADHANDLE;
  if (f->type != FD_EVENTQ) {
    FRELE(f);
    return FNU_ERR_INVALID;
  }
  eq = f->eq;
  if (uto != 0) {
    if (copyin(p->p_p->pgdir, &to, (uintptr_t)uto, sizeof(to)) < 0) {
      FRELE(f);
      return FNU_ERR_FAULT;
    }
    if (to.sec < 0 || to.nsec < 0 || to.nsec >= 1000000000) {
      FRELE(f);
      return FNU_ERR_INVALID;
    }
    forever = (uint64_t)to.sec > ~(uint64_t)0 / HZ - 1;
    left = (uint64_t)to.sec * HZ +
           (uint64_t)(to.nsec + (1000000000 / HZ) - 1) / (1000000000 / HZ);
  }
  if (room > 0)
    out = malloc((size_t)room * sizeof(*out), M_TEMP, M_WAITOK);
  acquiresleep(&eq->lock);
  for (i = 0; i < nchanges; i++) {
    if (copyin(p->p_p->pgdir, &c, (uintptr_t)(uchanges + (uint64_t)i * sizeof(c)),
               sizeof(c)) < 0) {
      error = FNU_ERR_FAULT;
      break;
    }
    error = change(eq, &c);
    if (error != 0 || (c.flags & FNU_EV_RECEIPT)) {
      if (got >= room)
        break;
      out[got] = c;
      out[got].flags = FNU_EV_ERROR;
      out[got].data = error;
      got++;
      error = 0;
    }
  }
  selscan_begin();
  while (error == 0 && got == 0 && room > 0) {
    seen = selscan_gen();
    selrecord(&eq->sel);
    if ((got = collect(eq, out, room)) > 0 || (!forever && left == 0))
      break;
    releasesleep(&eq->lock);
    if (proc_interrupted()) {
      acquiresleep(&eq->lock);
      error = FNU_ERR_INTERRUPTED;
      break;
    }
    left = selsleep(seen, left, forever);
    acquiresleep(&eq->lock);
  }
  selscan_end();
  releasesleep(&eq->lock);
  FRELE(f);
  p->sig_restart = 0;
  if (got > 0 && copyout(p->p_p->pgdir, (uintptr_t)uout, out,
                         (size_t)got * sizeof(*out)) < 0)
    error = FNU_ERR_FAULT;
  if (out != 0)
    free(out, M_TEMP);
  if (error != 0)
    return error;
  *retval = got;
  return 0;
}
