// MUTEX

#include "defs.h"
#include "mmu.h"
#include "proc.h"
#include "sched.h"
#include "x86.h"

#define SPIN_SECONDS 5
#define SPIN_TURNS (1ull << 30)

void mtx_init(struct mutex *m, const char *name) {
  m->name = name != 0 ? name : "mutex";
  m->owner = 0;
  m->pc = 0;
}

static ulong self(void) { return (ulong)curcpu(); }

static void stuck(struct mutex *m) {
  struct cpu_info *holder = (struct cpu_info *)m->owner;

  console_drop_lock();
  printf("mutex \"%s\" is held by cpu %d, taken at 0x%lx, wanted by cpu %d\n",
          m->name, holder != 0 ? (int)(holder - cpus) : -1, m->pc,
          (int)(curcpu() - cpus));
  panic("mutex held too long");
}

static ulong cas(volatile ulong *p, ulong old, ulong new) {
  return __sync_val_compare_and_swap(p, old, new);
}

void mtx_enter(struct mutex *m) {
  spinlock_enter();
  if (m->owner == self())
    panic("mtx_enter: %s already held", m->name);
  if (cas(&m->owner, 0, self()) != 0) {
    uint64_t hz = delay_tsc_hz(), start = hz != 0 ? rdtsc() : 0, turns = 0;

    do {
      while (m->owner != 0) {
        __asm __volatile("pause");
        if ((++turns & 0xfff) == 0 &&
            (hz != 0 ? rdtsc() - start > hz * SPIN_SECONDS
                     : turns > SPIN_TURNS))
          stuck(m);
      }
    } while (cas(&m->owner, 0, self()) != 0);
  }
  mb();
  m->pc = (ulong)__builtin_return_address(0);
}

int mtx_enter_try(struct mutex *m) {
  spinlock_enter();
  if (m->owner == self())
    panic("mtx_enter_try: %s already held", m->name);
  if (cas(&m->owner, 0, self()) != 0) {
    spinlock_exit();
    return 0;
  }
  mb();
  m->pc = (ulong)__builtin_return_address(0);
  return 1;
}

void mtx_leave(struct mutex *m) {
  if (m->owner != self())
    panic("mtx_leave: %s not held", m->name);
  mb();
  __asm __volatile("movq $0, %0" : "+m"(m->owner) :);
  spinlock_exit();
}

int mtx_owned(struct mutex *m) {
  int r;

  spinlock_enter();
  r = m->owner == self();
  spinlock_exit();
  return r;
}

int mtx_locked(struct mutex *m) { return m->owner != 0; }

void spinlock_enter(void) {
  int eflags;

  eflags = readeflags();
  cli();
  if (curcpu()->spinlock_count == 0)
    curcpu()->spinlock_intr = eflags & FL_IF;
  curcpu()->spinlock_count += 1;
}

void spinlock_exit(void) {
  struct cpu_info *c;

  if (readeflags() & FL_IF)
    panic("spinlock_exit - interruptible");
  c = curcpu();
  if (--c->spinlock_count < 0)
    panic("spinlock_exit");
  if (c->spinlock_count == 0) {
    if (c->notifying)
      sched_notify_flush(c);
    if (c->spinlock_intr)
      sti();
  }
}
