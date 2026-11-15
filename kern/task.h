#ifndef PRONINX_TASK_H
#define PRONINX_TASK_H

// TASKS
struct task {
  struct task *next;
  void (*fn)(void *);
  void *arg;
  int queued;
};

struct taskq;
extern struct taskq *const systq;

void task_set(struct task *t, void (*fn)(void *), void *arg);
int task_add(struct taskq *tq, struct task *t);
int task_del(struct taskq *tq, struct task *t);
void taskq_start(void);

#endif
