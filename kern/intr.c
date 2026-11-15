// INTERRUPT EVENTS
#include "defs.h"
#include "inc/abi.h"
#include "param.h"
#include "malloc.h"
#include "mutex.h"
#include "trap.h"
#include "x86.h"

#define NVEC 256 // what the IDT holds

struct intr_handler {
  struct intr_handler *next;
  int (*fn)(void *arg);
  void *arg;
  const char *name;
};

static struct {
  struct mutex lock;
  struct intr_handler *h[NVEC];
  uint8_t taken[NVEC];
  ulong count[NVEC];
  char countname[NVEC][32];
  char namename[NVEC][32];
} ev;

void intr_init(void) { mtx_init(&ev.lock, "intr"); }

int intr_vector_alloc(void) {
  int v;

  mtx_enter(&ev.lock);
  for (v = T_DEVICE0; v <= T_DEVICEN; v++)
    if (v != T_SYSCALL && !ev.taken[v]) {
      ev.taken[v] = 1;
      mtx_leave(&ev.lock);
      return v;
    }
  mtx_leave(&ev.lock);
  return -1;
}

void intr_vector_free(int v) {
  if (v < T_DEVICE0 || v > T_DEVICEN)
    return;
  mtx_enter(&ev.lock);
  ev.taken[v] = 0;
  mtx_leave(&ev.lock);
}

int intr_is_device_vector(int v) {
  return v >= T_DEVICE0 && v <= T_DEVICEN && v != T_SYSCALL;
}

static int64_t intr_count(void *arg) { return (int64_t)ev.count[(intptr_t)arg]; }

static int intr_names(void *arg, char *out, size_t max) {
  struct intr_handler *h;
  int v = (int)(intptr_t)arg;
  size_t n = 0;

  out[0] = 0;
  for (h = __atomic_load_n(&ev.h[v], __ATOMIC_ACQUIRE); h != 0;
       h = __atomic_load_n(&h->next, __ATOMIC_ACQUIRE)) {
    const char *s = h->name;

    if (h != ev.h[v] && n + 1 < max)
      out[n++] = '+';
    while (*s != 0 && n + 1 < max)
      out[n++] = *s++;
    out[n] = 0;
  }
  return (int)n;
}

// kern.intr.vecN.part
static void make_name(char *out, int v, const char *tail) {
  const char *p = "kern.intr.vec";
  int n = 0;

  while (*p != 0)
    out[n++] = *p++;
  if (v >= 100)
    out[n++] = (char)('0' + v / 100);
  if (v >= 10)
    out[n++] = (char)('0' + v / 10 % 10);
  out[n++] = (char)('0' + v % 10);
  while (*tail != 0)
    out[n++] = *tail++;
  out[n] = 0;
}

int intr_establish(int vector, int (*fn)(void *arg), void *arg, const char *name) {
  struct intr_handler *h, **pp;
  int first;

  if (vector < 0 || vector >= NVEC || fn == 0)
    return -1;
  h = malloc(sizeof(*h), M_DEVBUF, M_WAITOK | M_ZERO);
  h->fn = fn;
  h->arg = arg;
  h->name = name;
  mtx_enter(&ev.lock);
  first = ev.h[vector] == 0;
  for (pp = &ev.h[vector]; *pp != 0; pp = &(*pp)->next)
    ;
  __atomic_store_n(pp, h, __ATOMIC_RELEASE);
  mtx_leave(&ev.lock);
  if (first) {
    make_name(ev.countname[vector], vector, ".count");
    make_name(ev.namename[vector], vector, ".handlers");
    sysctl_add_quad_fn(ev.countname[vector], intr_count, (void *)(intptr_t)vector);
    sysctl_add_string_fn(ev.namename[vector], intr_names, (void *)(intptr_t)vector,
                         FNU_PLEDGE_VMINFO);
  }
  return 0;
}

int intr_dispatch(int vector) {
  struct intr_handler *h = __atomic_load_n(&ev.h[vector], __ATOMIC_ACQUIRE);
  int handled = 0;

  if (h == 0)
    return 0;
  atomic_fetchadd_long(&ev.count[vector], 1);
  for (; h != 0; h = __atomic_load_n(&h->next, __ATOMIC_ACQUIRE))
    if (h->fn(h->arg))
      handled++;
  return handled;
}
