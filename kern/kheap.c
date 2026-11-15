// THE KERNEL HEAP

#include "defs.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"

struct kspan {
  struct kspan *next;
  uintptr_t va;
  uint64_t len;   // the guard included
  uint64_t guard; // unmapped, at the bottom
  char *pages;
};

#define KHEAP_HASH 256
#define KHASH(va) (((va) / PGSIZE) % KHEAP_HASH)

static struct {
  struct mutex lock;
  struct kspan *free; // by address
  struct kspan *used[KHEAP_HASH];
  struct kspan *stale;
  int ready;
} kheap;

static struct kspan first;
static int reclaim_chan;

void kheap_init(void) {
  mtx_init(&kheap.lock, "kheap");
  first.va = KHEAP_BASE;
  first.len = KHEAP_SIZE;
  kheap.free = &first;
  kheap.ready = 1;
}

int kheap_owns(const void *p) {
  return (uintptr_t)p - KHEAP_BASE < KHEAP_SIZE;
}

// first fit; kheap.lock held
static int carve(struct kspan *s) {
  struct kspan **pp, *f;

  for (pp = &kheap.free; (f = *pp) != 0; pp = &f->next) {
    if (f->len < s->len)
      continue;
    s->va = f->va;
    f->va += s->len;
    f->len -= s->len;
    if (f->len == 0) {
      *pp = f->next;
      if (f != &first)
        free(f, M_VM);
    }
    return 0;
  }
  return -1;
}

// kheap.lock held
static void give_back(struct kspan *s) {
  struct kspan **pp, *f;

  for (pp = &kheap.free; (f = *pp) != 0 && f->va < s->va; pp = &f->next)
    ;
  s->next = f;
  *pp = s;
  if (f != 0 && s->va + s->len == f->va) {
    s->len += f->len;
    s->next = f->next;
    if (f != &first)
      free(f, M_VM);
  }
  for (f = kheap.free; f != 0 && f->next != s; f = f->next)
    ;
  if (f != 0 && f->va + f->len == s->va) {
    f->len += s->len;
    f->next = s->next;
    free(s, M_VM);
  }
}

static void unload(struct kspan *s) {
  uintptr_t va;
  char *pg;

  s->pages = 0;
  for (va = s->va + s->guard; va < s->va + s->len; va += PGSIZE) {
    mtx_enter(&kheap.lock);
    pg = hat_kunmap(va);
    mtx_leave(&kheap.lock);
    if (pg != 0) {
      *(char **)pg = s->pages;
      s->pages = pg;
    }
  }
}

static void stale_add(struct kspan *s) {
  int first;

  mtx_enter(&kheap.lock);
  first = kheap.stale == 0;
  s->next = kheap.stale;
  kheap.stale = s;
  mtx_leave(&kheap.lock);
  if (first)
    wakeup(&reclaim_chan);
}

static void settle(void) {
  struct kspan *s, *next;
  char *pg;

  mtx_enter(&kheap.lock);
  s = kheap.stale;
  kheap.stale = 0;
  mtx_leave(&kheap.lock);
  if (s == 0)
    return;
  tlb_shootdown();
  for (; s != 0; s = next) {
    next = s->next;
    while ((pg = s->pages) != 0) {
      s->pages = *(char **)pg;
      page_free(pg);
    }
    mtx_enter(&kheap.lock);
    give_back(s);
    mtx_leave(&kheap.lock);
  }
}

void kheap_reclaimer(void *arg __unused) {
  int idle;

  for (;;) {
    sleep_setup(&reclaim_chan, PVM, "kheap");
    mtx_enter(&kheap.lock);
    idle = kheap.stale == 0;
    mtx_leave(&kheap.lock);
    sleep_finish(INFSLP, idle);
    settle();
  }
}

void *kheap_alloc(size_t bytes, size_t guard, int flags) {
  struct kspan *s;
  uintptr_t va;
  char *pg;
  int r;

  if (!kheap.ready || bytes == 0 ||
      (s = malloc(sizeof(*s), M_VM, (flags & M_NOWAIT) | M_ZERO)) == 0)
    return 0;
  s->guard = PGROUNDUP(guard);
  s->len = s->guard + PGROUNDUP(bytes);
  mtx_enter(&kheap.lock);
  r = carve(s);
  mtx_leave(&kheap.lock);
  if (r < 0) {
    free(s, M_VM);
    return 0;
  }
  for (va = s->va + s->guard; va < s->va + s->len; va += PGSIZE) {
    pg = (flags & M_NOWAIT) ? page_alloc() : page_alloc_wait(0);
    if (pg == 0 && !(flags & M_NOWAIT)) {
      settle();
      pg = page_alloc_wait(0);
    }
    if (pg != 0) {
      mtx_enter(&kheap.lock);
      r = hat_kmap(va, pg);
      mtx_leave(&kheap.lock);
    }
    if (pg == 0 || r < 0) {
      if (pg != 0)
        page_free(pg);
      unload(s);
      stale_add(s);
      return 0;
    }
  }
  va = s->va + s->guard;
  mtx_enter(&kheap.lock);
  s->next = kheap.used[KHASH(va)];
  kheap.used[KHASH(va)] = s;
  mtx_leave(&kheap.lock);
  return (void *)(s->va + s->guard);
}

// the bytes it held
size_t kheap_free(void *p) {
  struct kspan **pp, *s;
  size_t len;

  mtx_enter(&kheap.lock);
  for (pp = &kheap.used[KHASH((uintptr_t)p)]; (s = *pp) != 0; pp = &s->next)
    if (s->va + s->guard == (uintptr_t)p)
      break;
  if (s == 0)
    panic("kheap_free: %p was not handed out", p);
  *pp = s->next;
  mtx_leave(&kheap.lock);
  len = s->len - s->guard;
  unload(s);
  stale_add(s);
  return len;
}
