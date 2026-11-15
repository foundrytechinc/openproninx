// CALLOUTS
#include "defs.h"
#include "callout.h"
#include "malloc.h"
#include "mutex.h"
#include "proc.h"

static struct {
  struct mutex lock;
  struct callout **wheel;
  uint64_t mask;
  uint64_t done;
  struct callout *running;
  int wanted;
} cw;

static int softclock_chan, running_chan;

static void bucket_add(struct callout *c) {
  struct callout **b = &cw.wheel[c->c_time & cw.mask];

  if ((c->c_next = *b) != 0)
    (*b)->c_prev = &c->c_next;
  c->c_prev = b;
  *b = c;
  c->c_flags |= CALLOUT_PENDING;
}

static void bucket_del(struct callout *c) {
  if (c->c_next != 0)
    c->c_next->c_prev = c->c_prev;
  *c->c_prev = c->c_next;
  c->c_next = 0;
  c->c_prev = 0;
  c->c_flags &= ~CALLOUT_PENDING;
}

void callout_init(struct callout *c) { memset(c, 0, sizeof(*c)); }

int mstohz(int64_t ms) {
  int64_t t = ms <= 0 ? 1 : (ms * HZ + 999) / 1000;

  return t > 0x7fffffff ? 0x7fffffff : (int)t;
}

int callout_pending(const struct callout *c) {
  return (c->c_flags & CALLOUT_PENDING) != 0;
}

void callout_reset(struct callout *c, int to_ticks, void (*fn)(void *),
                   void *arg) {
  if (cw.wheel == 0)
    panic("callout_reset before the wheel");
  mtx_enter(&cw.lock);
  if (c->c_flags & CALLOUT_PENDING)
    bucket_del(c);
  c->c_func = fn;
  c->c_arg = arg;
  c->c_time = ticks + (uint64_t)(to_ticks > 0 ? to_ticks : 1);
  bucket_add(c);
  mtx_leave(&cw.lock);
}

int callout_stop_async(struct callout *c) {
  int r = 0;

  mtx_enter(&cw.lock);
  if (c->c_flags & CALLOUT_PENDING) {
    bucket_del(c);
    r = 1;
  }
  mtx_leave(&cw.lock);
  return r;
}

int callout_stop(struct callout *c) {
  int r = callout_stop_async(c), busy;

  for (;;) {
    sleep_setup(&running_chan, PWAIT, "costop");
    mtx_enter(&cw.lock);
    busy = cw.running == c;
    mtx_leave(&cw.lock);
    sleep_finish(INFSLP, busy);
    if (!busy)
      return r;
  }
}

void callout_hardclock(int newticks) {
  int wake;

  if (cw.wheel == 0)
    return;
  mtx_enter(&cw.lock);
  wake = newticks > 1 || cw.wheel[ticks & cw.mask] != 0 || cw.wanted;
  if (wake)
    cw.wanted = 1;
  mtx_leave(&cw.lock);
  if (wake)
    wakeup(&softclock_chan);
}

static struct callout *due_at(uint64_t t, uint64_t upto) {
  struct callout *c;

  for (c = cw.wheel[t & cw.mask]; c != 0; c = c->c_next)
    if (c->c_time <= upto) {
      bucket_del(c);
      return c;
    }
  return 0;
}

static void softclock(void *arg __unused) {
  struct callout *c;
  uint64_t upto, t;
  int idle;

  for (;;) {
    sleep_setup(&softclock_chan, PWAIT, "softclock");
    mtx_enter(&cw.lock);
    idle = !cw.wanted && cw.done >= ticks;
    mtx_leave(&cw.lock);
    sleep_finish(INFSLP, idle);

    mtx_enter(&cw.lock);
    cw.wanted = 0;
    upto = ticks;
    t = upto - cw.done > cw.mask ? upto - cw.mask : cw.done + 1;
    for (; t <= upto; t++) {
      while ((c = due_at(t, upto)) != 0) {
        cw.running = c;
        mtx_leave(&cw.lock);
        c->c_func(c->c_arg);
        mtx_enter(&cw.lock);
        cw.running = 0;
        mtx_leave(&cw.lock);
        wakeup(&running_chan);
        mtx_enter(&cw.lock);
      }
    }
    cw.done = upto;
    mtx_leave(&cw.lock);
  }
}

void callout_startup(void) {
  uint64_t n = 1;

  while (n < (uint64_t)ncallout + 16)
    n <<= 1;
  mtx_init(&cw.lock, "callout");
  cw.wheel = malloc(n * sizeof(*cw.wheel), M_DEVBUF, M_WAITOK | M_ZERO);
  cw.mask = n - 1;
  cw.done = ticks;
}

void softclock_start(void) {
  if (kthread_create(softclock, 0, 0, "softclock") != 0)
    panic("no softclock thread");
}

// maxpps a second
int ppsratecheck(uint64_t *last, int *curpps, int maxpps) {
  uint64_t now = ticks;

  if (*last == 0 || now - *last >= HZ) {
    *last = now;
    *curpps = 1;
    return 1;
  }
  if (*curpps < maxpps) {
    (*curpps)++;
    return 1;
  }
  return 0;
}
