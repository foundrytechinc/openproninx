#ifndef PRONINX_CALLOUT_H
#define PRONINX_CALLOUT_H

#include "inc/types.h"

struct callout {
  struct callout *c_next;
  struct callout **c_prev;
  uint64_t c_time;
  void (*c_func)(void *);
  void *c_arg;
  int c_flags;
};

#define CALLOUT_PENDING 0x1 // on the wheel

void callout_startup(void);
void softclock_start(void);
void callout_hardclock(int newticks);
void callout_init(struct callout *c);
void callout_reset(struct callout *c, int to_ticks, void (*fn)(void *),
                   void *arg);
int callout_stop_async(struct callout *c);
int callout_stop(struct callout *c);
int callout_pending(const struct callout *c);
int mstohz(int64_t ms);

#endif
