#include "defs.h"
#include "file.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"
#include "x86.h"
#include "inc/abi.h"
#include "inc/stat.h"

// SELECT
// waiters sleep on their own generation
// collision: wake everybody
static struct mutex sellock = MUTEX_INITIALIZER("select");
static struct proc *selq;

// sellock held
static int selq_has(struct proc *p) {
  struct proc *q;

  for (q = selq; q != 0; q = q->selnext)
    if (q == p)
      return 1;
  return 0;
}

static void selnotify(struct proc *p) {
  atomic_fetchadd_int(&p->selgen, 1);
  wakeup(&p->selgen);
}

void selscan_begin(void) {
  struct proc *p = curproc;

  mtx_enter(&sellock);
  p->selnext = selq;
  selq = p;
  p->selscan = 1;
  mtx_leave(&sellock);
}

void selscan_end(void) {
  struct proc *p = curproc, **pp;

  mtx_enter(&sellock);
  for (pp = &selq; *pp != 0; pp = &(*pp)->selnext)
    if (*pp == p) {
      *pp = p->selnext;
      break;
    }
  p->selnext = 0;
  p->selscan = 0;
  mtx_leave(&sellock);
}

uint selscan_gen(void) { return atomic_fetchadd_int(&curproc->selgen, 0); }

void selrecord(struct selinfo *sip) {
  struct proc *p = curproc;

  if (p == 0 || !p->selscan)
    return;
  mtx_enter(&sellock);
  if (sip->si_proc != p) {
    if (sip->si_proc != 0 && selq_has(sip->si_proc))
      sip->si_coll = 1;
    else
      sip->si_proc = p;
  }
  mtx_leave(&sellock);
}

void selwakeup(struct selinfo *sip) {
  struct proc *p;

  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (__atomic_load_n(&sip->si_proc, __ATOMIC_RELAXED) == 0)
    return;
  mtx_enter(&sellock);
  if (sip->si_coll) {
    sip->si_coll = 0;
    for (p = selq; p != 0; p = p->selnext)
      selnotify(p);
  } else if ((p = sip->si_proc) != 0 && selq_has(p))
    selnotify(p);
  sip->si_proc = 0;
  mtx_leave(&sellock);
}

uint64_t selsleep(uint seen, uint64_t left, int forever) {
  struct proc *p = curproc;
  uint64_t t0 = ticks, gone;

  sleep_setup(&p->selgen, PSOCK | PCATCH, "select");
  sleep_finish(forever ? INFSLP
                       : (left > 0x7fffffff ? 0x7fffffff : left) * (1000000000ull / HZ),
               atomic_fetchadd_int(&p->selgen, 0) == seen);
  gone = ticks - t0;
  return gone >= left ? 0 : left - gone;
}

int file_ready(struct file *f, int want) {
  if (f == 0)
    return 0;
  if (f->type == FD_EVENTQ)
    return (want & FNU_SEL_READ) && eventq_ready(f->eq) ? FNU_SEL_READ : 0;
  if (f->type == FD_PIPE)
    return pipe_ready(f->pipe, want);
  if (f->type == FD_SOCKET)
    return soready(f->sock, want);
  if (f->type == FD_INODE && f->ip != 0 && f->ip->type == FNU_TYPE_DEVICE &&
      f->ip->major >= 0 && f->ip->major < NDEV && devsw[f->ip->major].poll != 0)
    return devsw[f->ip->major].poll(f->ip, want);
  return want & (FNU_READY_IN | FNU_READY_OUT);
}

int fnu_ready_wait(struct fnu_ready *list, int n, uint64_t left, int forever) {
  struct proc *p = curproc;
  int i, count;

  selscan_begin();
  for (;;) {
    uint seen = selscan_gen();

    count = 0;
    for (i = 0; i < n; i++) {
      struct file *f;
      int want = list[i].want & (FNU_READY_IN | FNU_READY_OUT | FNU_READY_PRI);

      list[i].got = 0;
      if (list[i].fd < 0)
        continue;
      if ((f = fd_getfile(p->p_p, list[i].fd)) == 0) {
        list[i].got = FNU_READY_BAD;
        count++;
        continue;
      }
      list[i].got = (uint16)file_ready(f, want | FNU_READY_HUP);
      FRELE(f);
      if (list[i].got != 0)
        count++;
    }
    if (count > 0 || (!forever && left == 0))
      break;
    if (proc_interrupted()) {
      count = -FNU_ERR_INTERRUPTED;
      break;
    }
    left = selsleep(seen, left, forever);
  }
  selscan_end();
  return count;
}
