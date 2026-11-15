// MALLOC

#include "defs.h"
#include "malloc.h"
#include "mmu.h"

#define MAGIC 0x6d616c63u
#define HDR 16
#define NCLASS 8 // 16 ... 2048
#define BIG (PGSIZE / 2)

struct page {
  uint32_t magic;
  uint32_t class;
};

struct chunk {
  struct chunk *next;
};

static struct {
  struct mutex lock;
  struct chunk *list;
  int64_t inuse;
} cls[NCLASS];

static struct {
  struct mutex lock;
  int64_t inuse[M_LAST];
  int ready;
} stat;

void kmeminit(void) {
  int i;

  mtx_init(&stat.lock, "malloc stats");
  for (i = 0; i < NCLASS; i++)
    mtx_init(&cls[i].lock, "malloc");
  stat.ready = 1;
}

static int class_of(size_t n) {
  size_t size = 16;
  int c = 0;

  while (size < n) {
    size <<= 1;
    c++;
  }
  return c;
}

// lock held
static int grow(int c) {
  char *pg = page_alloc();
  struct page *h;
  size_t size = (size_t)16 << c, off;

  if (pg == 0)
    return -1;
  h = (struct page *)pg;
  h->magic = MAGIC;
  h->class = (uint32_t)c;
  for (off = HDR; off + size <= PGSIZE; off += size) {
    struct chunk *f = (struct chunk *)(pg + off);

    f->next = cls[c].list;
    cls[c].list = f;
  }
  return 0;
}

static void count(int type, int64_t bytes) {
  if (!stat.ready || type < 0 || type >= M_LAST)
    return;
  mtx_enter(&stat.lock);
  stat.inuse[type] += bytes;
  mtx_leave(&stat.lock);
}

void *malloc(size_t size, int type, int flags) {
  struct chunk *f;
  void *p;
  int c;

  if (size == 0)
    size = 1;
  if (size > BIG) {
    p = kheap_alloc(size, 0, flags);
    if (p == 0) {
      if (flags & (M_NOWAIT | M_CANFAIL))
        return 0;
      panic("malloc: out of memory");
    }
    if (flags & M_ZERO)
      memset(p, 0, size);
    count(type, (int64_t)size);
    return p;
  }
  c = class_of(size);
  mtx_enter(&cls[c].lock);
  if (cls[c].list == 0 && grow(c) < 0) {
    mtx_leave(&cls[c].lock);
    if (flags & (M_NOWAIT | M_CANFAIL))
      return 0;
    panic("malloc: out of memory");
  }
  f = cls[c].list;
  cls[c].list = f->next;
  cls[c].inuse++;
  mtx_leave(&cls[c].lock);
  if (flags & M_ZERO)
    memset(f, 0, (size_t)16 << c);
  count(type, (int64_t)16 << c);
  return f;
}

void free(void *p, int type) {
  struct page *h;
  struct chunk *f = p;
  int c;

  if (p == 0)
    return;
  if (kheap_owns(p)) {
    count(type, -(int64_t)kheap_free(p));
    return;
  }
  h = (struct page *)((uintptr_t)p & ~(uintptr_t)(PGSIZE - 1));
  if (h->magic != MAGIC || h->class >= NCLASS)
    panic("free: %p is not from malloc", p);
  c = (int)h->class;
  mtx_enter(&cls[c].lock);
  f->next = cls[c].list;
  cls[c].list = f;
  cls[c].inuse--;
  mtx_leave(&cls[c].lock);
  count(type, -((int64_t)16 << c));
}

int64_t malloc_inuse(void) {
  int64_t bytes = 0;
  int i;

  for (i = 0; i < NCLASS; i++)
    bytes += cls[i].inuse * (16 << i);
  return bytes;
}

int64_t malloc_inuse_type(int type) {
  return type >= 0 && type < M_LAST ? stat.inuse[type] : 0;
}
