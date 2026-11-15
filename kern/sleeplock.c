#include "sleeplock.h"
#include "defs.h"
#include "proc.h"
#include "mutex.h"

void initsleeplock(struct sleeplock *lk, char *name) {
  mtx_init(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->waiters = 0;
  lk->pid = 0;
  lk->owner = 0;
}

int trysleep(struct sleeplock *lk) {
  mtx_enter(&lk->lk);
  if (lk->locked) {
    mtx_leave(&lk->lk);
    return 0;
  }
  lk->locked = 1;
  lk->pid = curproc->p_p->pid;
  lk->owner = curproc;
  atomic_fetchadd_int((uint *)&lk->owner->nsleeplocks, 1);
  mtx_leave(&lk->lk);
  return 1;
}

void acquiresleep(struct sleeplock *lk) {
  mtx_enter(&lk->lk);
  while (lk->locked) {
    lk->waiters++;
    if (lk->owner != 0)
      lend_priority(lk->owner, curproc);
    curproc->lockwait = lk;
    msleep(lk, &lk->lk, PLOCK, "sleeplk", 0);
    curproc->lockwait = 0;
    lk->waiters--;
  }
  lk->locked = 1;
  lk->pid = curproc->p_p->pid;
  lk->owner = curproc;
  atomic_fetchadd_int((uint *)&lk->owner->nsleeplocks, 1);
  mtx_leave(&lk->lk);
}

void releasesleep(struct sleeplock *lk) {
  struct proc *o;

  mtx_enter(&lk->lk);
  o = lk->owner;
  lk->locked = 0;
  lk->pid = 0;
  lk->owner = 0;
  if (lk->waiters != 0)
    wakeup(lk);
  mtx_leave(&lk->lk);
  if (o != 0 && (int)atomic_fetchadd_int((uint *)&o->nsleeplocks, (uint)-1) == 1)
    unlend_priority(o);
}

int holdingsleep(struct sleeplock *lk) {
  int r;

  mtx_enter(&lk->lk);
  r = lk->locked && lk->owner == curproc;
  mtx_leave(&lk->lk);
  return r;
}
