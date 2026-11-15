// TASKS: work for a thread, out of interrupt context
#include "defs.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"
#include "task.h"

struct taskq {
  struct mutex lock;
  struct task *head, **tail;
};

static struct taskq sys = {MUTEX_INITIALIZER("systq"), 0, &sys.head};
struct taskq *const systq = &sys;

void task_set(struct task *t, void (*fn)(void *), void *arg) {
  t->next = 0;
  t->fn = fn;
  t->arg = arg;
  t->queued = 0;
}

int task_add(struct taskq *tq, struct task *t) {
  int added = 0;

  mtx_enter(&tq->lock);
  if (!t->queued) {
    t->queued = 1;
    t->next = 0;
    *tq->tail = t;
    tq->tail = &t->next;
    added = 1;
  }
  mtx_leave(&tq->lock);
  if (added)
    wakeup(tq);
  return added;
}

int task_del(struct taskq *tq, struct task *t) {
  struct task **pp;
  int removed = 0;

  mtx_enter(&tq->lock);
  for (pp = &tq->head; *pp != 0; pp = &(*pp)->next)
    if (*pp == t) {
      if ((*pp = t->next) == 0)
        tq->tail = pp;
      t->queued = 0;
      removed = 1;
      break;
    }
  mtx_leave(&tq->lock);
  return removed;
}

static void taskq_thread(void *arg) {
  struct taskq *tq = arg;
  struct task *t;

  for (;;) {
    mtx_enter(&tq->lock);
    while ((t = tq->head) == 0)
      msleep(tq, &tq->lock, PWAIT, "systq", 0);
    if ((tq->head = t->next) == 0)
      tq->tail = &tq->head;
    t->queued = 0;
    mtx_leave(&tq->lock);
    t->fn(t->arg);
  }
}

void taskq_start(void) {
  if (kthread_create(taskq_thread, systq, 0, "systq") != 0)
    panic("no systq thread");
}
