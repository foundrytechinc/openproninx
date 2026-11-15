// FUTEXES
#include "defs.h"
#include "inc/abi.h"
#include "memlayout.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"
#include "uvm.h"

struct futex {
  struct futex *next;
  struct futex_queue *queue;
  const void *key;
  uint64_t off;
  struct proc *proc;
};

#define FUTEX_QUEUES 64 // FUTEX_SLPQUES_SIZE

static struct futex_queue {
  struct mutex lock;
  struct futex *head;
} queues[FUTEX_QUEUES];

void futex_init(void) {
  int i;

  for (i = 0; i < FUTEX_QUEUES; i++)
    mtx_init(&queues[i].lock, "futex");
}

static struct futex_queue *queue_of(const void *key, uint64_t off) {
  uint64_t h = (off >> 2) ^ ((uintptr_t)key >> 4);

  h ^= h >> 6;
  return &queues[h % FUTEX_QUEUES];
}

static void key_of(struct proc *p, uintptr_t va, int op, struct futex *f) {
  vm_futex_key(p, va, (op & FNU_FUTEX_PRIVATE) != 0, &f->key, &f->off);
}

static void dequeue(struct futex_queue *q, struct futex *f) {
  struct futex **pp;

  for (pp = &q->head; *pp != 0; pp = &(*pp)->next)
    if (*pp == f) {
      *pp = f->next;
      return;
    }
}

static int unwait(struct futex *f) {
  struct futex_queue *q = f->queue;
  int was;

  for (;;) {
    mtx_enter(&q->lock);
    if (f->queue == q)
      break;
    mtx_leave(&q->lock);
    q = f->queue;
  }
  was = f->proc != 0;
  if (was)
    dequeue(q, f);
  mtx_leave(&q->lock);
  return was;
}

static int futex_wait(struct proc *p, uintptr_t va, uint32_t val, uint64_t nsec,
                      int op) {
  struct futex f;
  uint32_t now;
  int error;

  if (nsec == 0)
    return FNU_ERR_TIMEDOUT;
  key_of(p, va, op, &f);
  f.proc = p;
  f.queue = queue_of(f.key, f.off);
  mtx_enter(&f.queue->lock);
  f.next = f.queue->head;
  f.queue->head = &f;
  mtx_leave(&f.queue->lock);

  if (copyin(p->p_p->pgdir, &now, va, sizeof(now)) < 0) {
    unwait(&f);
    return FNU_ERR_FAULT;
  }
  if (now != val) {
    unwait(&f);
    return FNU_ERR_WOULDBLOCK;
  }
  sleep_setup(&f, PWAIT | PCATCH, "futex");
  error = sleep_finish(nsec == FNU_FUTEX_FOREVER ? INFSLP : nsec, f.proc != 0);
  if (error != 0 || f.proc != 0) {
    if (!unwait(&f))
      error = 0;
    else if (error == FNU_ERR_WOULDBLOCK)
      error = FNU_ERR_TIMEDOUT;
  }
  return error;
}

// with the queue's lock held
static int wake_some(struct futex_queue *q, const void *key, uint64_t off,
                     uint32_t n) {
  struct futex **pp = &q->head, *f;
  int count = 0;

  while ((f = *pp) != 0 && (uint32_t)count < n) {
    if (f->key != key || f->off != off) {
      pp = &f->next;
      continue;
    }
    *pp = f->next;
    f->proc = 0;
    wakeup(f);
    count++;
  }
  return count;
}

int futex_wake(struct proc *p, uintptr_t va, uint32_t n, int op) {
  struct futex key;
  struct futex_queue *q;
  int count;

  key_of(p, va, op, &key);
  q = queue_of(key.key, key.off);
  mtx_enter(&q->lock);
  count = wake_some(q, key.key, key.off, n);
  mtx_leave(&q->lock);
  return count;
}

static int futex_requeue(struct proc *p, uintptr_t va, uint32_t n, uintptr_t to,
                         uint32_t m, int op) {
  struct futex from, dest, **pp, *f;
  struct futex_queue *q, *nq;
  int count;

  key_of(p, va, op, &from);
  key_of(p, to, op, &dest);
  q = queue_of(from.key, from.off);
  nq = queue_of(dest.key, dest.off);
  if (q < nq) {
    mtx_enter(&q->lock);
    mtx_enter(&nq->lock);
  } else {
    mtx_enter(&nq->lock);
    if (q != nq)
      mtx_enter(&q->lock);
  }
  count = wake_some(q, from.key, from.off, n);
  for (pp = &q->head; (f = *pp) != 0 && m > 0;) {
    if (f->key != from.key || f->off != from.off) {
      pp = &f->next;
      continue;
    }
    *pp = f->next;
    f->key = dest.key;
    f->off = dest.off;
    f->queue = nq;
    f->next = nq->head;
    nq->head = f;
    m--;
    if (q == nq && pp == &q->head)
      pp = &f->next;
  }
  if (q != nq)
    mtx_leave(&q->lock);
  mtx_leave(&nq->lock);
  return count;
}

int sys_futex(struct proc *p, const register_t *uap, register_t *retval) {
  uint64_t word, nsec, to;
  int op, val, r;

  word = uap[0];
  op = (int)uap[1];
  val = (int)uap[2];
  nsec = uap[3];
  to = uap[4];
  if ((word & 3) != 0 || word >= USERTOP)
    return FNU_ERR_INVALID;
  switch (op & FNU_FUTEX_OP_MASK) {
  case FNU_FUTEX_WAIT:
    return futex_wait(p, (uintptr_t)word, (uint32_t)val, nsec, op);
  case FNU_FUTEX_WAKE:
    *retval = futex_wake(p, (uintptr_t)word, (uint32_t)val, op);
    return 0;
  case FNU_FUTEX_REQUEUE:
    if ((to & 3) != 0 || to >= USERTOP)
      return FNU_ERR_INVALID;
    r = futex_requeue(p, (uintptr_t)word, (uint32_t)val, (uintptr_t)to,
                      nsec > 0xffffffffu ? 0xffffffffu : (uint32_t)nsec, op);
    *retval = r;
    return 0;
  }
  return FNU_ERR_INVALID;
}
