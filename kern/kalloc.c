#include "defs.h"
#include "hibernate.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "mutex.h"
#include "x86.h"

#define SIZE_2M 0x200000ULL

void page_free_range(void *vstart, void *vend);
static void contig_reserve(void);
static void bufmem_reserve(void);
extern char end[];

struct run {
  struct run *next;
};

struct {
  struct mutex lock;
  int use_lock;
  struct run *freelist;
  struct run *lowlist;
  int nfree;
} kmem;

int kalloc_nfree(void) { return kmem.nfree; }

static uintptr_t phys_temporary_top;
uintptr_t phys_top;
uintptr_t phys_map_top;
static uint64_t ram_bytes;

#define NCARVE (16 + FNU_MODULE_MAX + FNU_PCIROM_MAX)
static struct {
  uint64_t base;
  uint64_t end;
  int boot; // the loader's page tables: given back after hat_kern_setup
} carved[NCARVE];
static int ncarved;

static uint64_t carve_ram_below(uint64_t bytes, uint64_t align, uint64_t limit) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t best = 0;
  uint i;
  int k;

  if (bi == 0 || bytes == 0 || ncarved >= NCARVE)
    return 0;
  bytes = (bytes + align - 1) & ~(align - 1);
  for (i = 0; i < bi->memranges; i++) {
    uint64_t lo = bi->mem[i].base, hi = lo + bi->mem[i].length, at;

    if (bi->mem[i].type != FNU_MEM_RAM)
      continue;
    if (hi > phys_top)
      hi = phys_top;
    if (hi > limit)
      hi = limit;
    hi &= ~(align - 1);
    lo = (lo + align - 1) & ~(align - 1);
    while (hi > lo && hi - lo >= bytes) {
      at = hi - bytes;
      for (k = 0; k < ncarved; k++)
        if (carved[k].base < at + bytes && carved[k].end > at)
          break;
      if (k < ncarved) {
        hi = carved[k].base & ~(align - 1);
        continue;
      }
      if (at >= phys_temporary_top && at > best)
        best = at;
      break;
    }
  }
  if (best == 0)
    return 0;
  carved[ncarved].base = best;
  carved[ncarved].end = best + bytes;
  ncarved++;
  return best;
}

static uint64_t carve_ram(uint64_t bytes, uint64_t align) {
  return carve_ram_below(bytes, align, phys_top);
}

#define DMA_SLOT (64 * 1024)
#define DMA_SLOTS 64
static struct {
  struct mutex lock;
  uint64_t base;
  uint8_t run[DMA_SLOTS];
} dma;

void *dma_alloc(uint64_t bytes) {
  int n = (int)((bytes + DMA_SLOT - 1) / DMA_SLOT), i, j;

  if (dma.base == 0 || n == 0 || n > DMA_SLOTS)
    return 0;
  mtx_enter(&dma.lock);
  for (i = 0; i + n <= DMA_SLOTS; i++) {
    for (j = 0; j < n && dma.run[i + j] == 0; j++)
      ;
    if (j < n) {
      i += j;
      continue;
    }
    dma.run[i] = (uint8_t)n;
    for (j = 1; j < n; j++)
      dma.run[i + j] = 0xff;
    mtx_leave(&dma.lock);
    return P2V(dma.base + (uint64_t)i * DMA_SLOT);
  }
  mtx_leave(&dma.lock);
  return 0;
}

void dma_free(void *p) {
  uint64_t off = (uint64_t)V2P(p) - dma.base;
  int i = (int)(off / DMA_SLOT), n, j;

  if (p == 0 || off >= (uint64_t)DMA_SLOTS * DMA_SLOT || dma.run[i] == 0 || dma.run[i] == 0xff)
    return;
  mtx_enter(&dma.lock);
  n = dma.run[i];
  for (j = 0; j < n; j++)
    dma.run[i + j] = 0;
  mtx_leave(&dma.lock);
}

static uint64_t msgbuf_phys;

static void carve_msgbuf(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t bytes = PGROUNDUP(msgbuf_bytes()), at = phys_top - bytes;
  uint i;

  if (phys_top < bytes || ncarved >= NCARVE)
    return;
  for (i = 0; i < bi->memranges; i++)
    if (bi->mem[i].type == FNU_MEM_RAM && bi->mem[i].base <= at &&
        bi->mem[i].base + bi->mem[i].length >= phys_top) {
      carved[ncarved].base = at;
      carved[ncarved].end = phys_top;
      ncarved++;
      msgbuf_phys = at;
      return;
    }
}

uint64_t msgbuf_reserved(void) { return msgbuf_phys; }

static uint64_t piglet_phys;

static void carve_piglet(void) {
  if (ram_bytes >= HIB_MIN_MEMORY)
    piglet_phys = carve_ram_below(HIB_PIGLET, HIB_CHUNK, 0x100000000ULL);
}

uint64_t hibernate_piglet(void) { return piglet_phys; }

static void carve_modules(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint i;

  for (i = 0; bi != 0 && i < bi->modules && ncarved < NCARVE; i++) {
    carved[ncarved].base = PGROUNDDOWN(bi->module[i].base);
    carved[ncarved].end = PGROUNDUP(bi->module[i].base + bi->module[i].size);
    ncarved++;
  }
  for (i = 0; bi != 0 && i < bi->pciroms && ncarved < NCARVE; i++) {
    carved[ncarved].base = PGROUNDDOWN(bi->pcirom[i].base);
    carved[ncarved].end = PGROUNDUP(bi->pcirom[i].base + bi->pcirom[i].size);
    ncarved++;
  }
}

// THE LOADER'S PAGE TABLES
// still under cr3 while page_init frees what lies past the kernel
static void carve_table(uint64_t pa, uint64_t lo, uint64_t hi) {
  int k;

  if (pa < lo || pa >= hi)
    return;
  for (k = 0; k < ncarved; k++)
    if (carved[k].boot && pa >= carved[k].base && pa < carved[k].end)
      return;
  if (ncarved > 0 && carved[ncarved - 1].boot && carved[ncarved - 1].end == pa) {
    carved[ncarved - 1].end += PGSIZE;
    return;
  }
  if (ncarved >= NCARVE)
    panic("page_init: the loader's page tables are all over memory");
  carved[ncarved].base = pa;
  carved[ncarved].end = pa + PGSIZE;
  carved[ncarved].boot = 1;
  ncarved++;
}

static void carve_boot_tables(uint64_t lo, uint64_t hi) {
  uint64_t l4 = PTE_ADDR(rcr3()), l3, l2, *p4, *p3, *p2;
  int i, j, k;

  carve_table(l4, lo, hi);
  p4 = (uint64_t *)P2V(l4);
  for (i = 0; i < 512; i++) {
    if (!(p4[i] & PTE_P))
      continue;
    carve_table(l3 = PTE_ADDR(p4[i]), lo, hi);
    p3 = (uint64_t *)P2V(l3);
    for (j = 0; j < 512; j++) {
      if (!(p3[j] & PTE_P) || (p3[j] & PTE_PS))
        continue;
      carve_table(l2 = PTE_ADDR(p3[j]), lo, hi);
      p2 = (uint64_t *)P2V(l2);
      for (k = 0; k < 512; k++)
        if ((p2[k] & PTE_P) && !(p2[k] & PTE_PS))
          carve_table(PTE_ADDR(p2[k]), lo, hi);
    }
  }
}

// after hat_kern_setup nothing walks them
void page_boot_tables_free(void) {
  int k;

  for (k = 0; k < ncarved; k++)
    if (carved[k].boot) {
      page_free_range((void *)P2V(carved[k].base), (void *)P2V(carved[k].end));
      carved[k].base = carved[k].end = 0;
      carved[k].boot = 0;
    }
}

static void free_uncarved(uint64_t lo, uint64_t hi) {
  int k;

  for (k = 0; k < ncarved; k++) {
    if (carved[k].end <= lo || carved[k].base >= hi)
      continue;
    if (carved[k].base > lo)
      free_uncarved(lo, carved[k].base);
    if (carved[k].end < hi)
      free_uncarved(carved[k].end, hi);
    return;
  }
  if (lo < hi)
    page_free_range((void *)P2V(lo), (void *)P2V(hi));
}

static void detect_memory(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t top = 0, maptop = 0, hi;
  uint i;

  if (bi == 0)
    panic("page_init: no boot information");

  for (i = 0; i < bi->memranges; i++) {
    if (bi->mem[i].type != FNU_MEM_RAM)
      continue;
    hi = bi->mem[i].base + bi->mem[i].length;
    ram_bytes += bi->mem[i].length;
    if (hi > top)
      top = hi;
  }
  if (top == 0)
    panic("page_init: no usable memory");

  maptop = top;
  for (i = 0; i < bi->memranges; i++) {
    hi = bi->mem[i].base + bi->mem[i].length;
    if (bi->mem[i].base <= top && hi > maptop)
      maptop = hi;
  }

  phys_top = (uintptr_t)(top / PGSIZE * PGSIZE);
  phys_map_top = (uintptr_t)((maptop + SIZE_2M - 1) / SIZE_2M * SIZE_2M);
  printf("MEM: %u MB ram\n", (uint)(ram_bytes / (1024 * 1024)));
}

static uint64_t ram_run_end(uint64_t p) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t end = 0, hi;
  uint i;
  int grew;

  for (i = 0; i < bi->memranges; i++) {
    if (bi->mem[i].type != FNU_MEM_RAM)
      continue;
    hi = bi->mem[i].base + bi->mem[i].length;
    if (p >= bi->mem[i].base && p < hi) {
      end = hi;
      break;
    }
  }
  for (grew = end != 0; grew;) {
    grew = 0;
    for (i = 0; i < bi->memranges; i++) {
      if (bi->mem[i].type != FNU_MEM_RAM || bi->mem[i].base > end)
        continue;
      hi = bi->mem[i].base + bi->mem[i].length;
      if (hi > end) {
        end = hi;
        grew = 1;
      }
    }
  }
  return end;
}

uint64_t get_total_ram(void) { return ram_bytes; }

uint64_t get_free_ram(void) { return (uint64_t)kmem.nfree * PGSIZE; }

void page_init(void *vstart) {
  mtx_init(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  detect_memory();
  phys_temporary_top = PGROUNDUP((uintptr_t)V2P(vstart));
  carve_msgbuf();
  contig_reserve();
  bufmem_reserve();
  carve_modules();
  carve_piglet();
  mtx_init(&dma.lock, "dma");
  dma.base = carve_ram_below((uint64_t)DMA_SLOTS * DMA_SLOT, DMA_SLOT, 0x100000000ULL);
  uintptr_t end_phys = phys_temporary_top;
  uint64_t run = ram_run_end(end_phys);

  if (run == 0)
    panic("page_init: the loader left the kernel outside ram");
  // until hat_kern_setup only the loader's map
  phys_temporary_top = run < FNU_BOOT_MAPPED ? run : FNU_BOOT_MAPPED;
  if (phys_temporary_top <= end_phys)
    panic("page_init: the kernel ends past the loader's map");
  carve_boot_tables(end_phys, phys_temporary_top);
  free_uncarved(end_phys, phys_temporary_top);
  if (kmem.nfree == 0)
    panic("page_init: no free memory next to the kernel");
}

// THE CONTIGUOUS ARENA
struct contig_block {
  struct contig_block *next;
  uint64_t size;
  int used;
};

static char *contig_base;
static uint64_t contig_size;
static struct contig_block *contig_head;

#define CONTIG_ALIGN 4096
#define CONTIG_HDR ((uint64_t)((sizeof(struct contig_block) + 63) & ~63ULL))

static uintptr_t contig_phys;
static struct mutex contig_lock;
static int contig_locking;

// THE BUFFER CACHE'S OWN MEMORY
static uintptr_t bufmem_phys;
static uint64_t bufmem_size;

static void bufmem_reserve(void) {
  uint64_t want = ram_bytes / 10, at;

  if (want < 4 * 1024 * 1024)
    want = 4 * 1024 * 1024;
  if (want > 256 * 1024 * 1024)
    want = 256 * 1024 * 1024;
  want = (want + SIZE_2M - 1) / SIZE_2M * SIZE_2M;
  if ((at = carve_ram(want, SIZE_2M)) == 0)
    return;
  bufmem_phys = (uintptr_t)at;
  bufmem_size = want;
}

void bufmem_region(char **base, uint64_t *size) {
  *base = bufmem_phys != 0 ? (char *)P2V(bufmem_phys) : 0;
  *size = bufmem_phys != 0 ? bufmem_size : 0;
}

static void contig_reserve(void) {
  uint64_t arena = ram_bytes / 16, at;

  if (arena < 8 * 1024 * 1024)
    arena = 8 * 1024 * 1024;
  if (arena > 64 * 1024 * 1024)
    arena = 64 * 1024 * 1024;
  arena = (arena + SIZE_2M - 1) / SIZE_2M * SIZE_2M;
  if ((at = carve_ram(arena, SIZE_2M)) == 0)
    return;
  contig_phys = (uintptr_t)at;
  contig_size = arena;
}

static void contig_init(char *base, uint64_t size) {
  contig_base = base;
  contig_size = size;
  contig_head = (struct contig_block *)base;
  contig_head->next = 0;
  contig_head->size = size;
  contig_head->used = 0;
}

void contig_setup(void) {
  mtx_init(&contig_lock, "contig");
  if (contig_phys != 0)
    contig_init((char *)P2V(contig_phys), contig_size);
}

uint64_t contig_capacity(void) { return contig_size; }

int contig_owns(void *p) {
  return contig_base != 0 && (char *)p >= contig_base && (char *)p < contig_base + contig_size;
}

void *contig_alloc(uint64_t bytes) {
  struct contig_block *b, *split;
  uint64_t need;

  if (contig_base == 0 || bytes == 0)
    return 0;
  need = (CONTIG_HDR + bytes + CONTIG_ALIGN - 1) & ~(uint64_t)(CONTIG_ALIGN - 1);
  if (contig_locking)
    mtx_enter(&contig_lock);
  for (b = contig_head; b != 0; b = b->next) {
    if (b->used || b->size < need)
      continue;
    if (b->size >= need + CONTIG_ALIGN * 2) {
      split = (struct contig_block *)((char *)b + need);
      split->next = b->next;
      split->size = b->size - need;
      split->used = 0;
      b->next = split;
      b->size = need;
    }
    b->used = 1;
    if (contig_locking)
      mtx_leave(&contig_lock);
    return (char *)b + CONTIG_HDR;
  }
  if (contig_locking)
    mtx_leave(&contig_lock);
  return 0;
}

void contig_free(void *p) {
  struct contig_block *b, *n;

  if (p == 0 || contig_base == 0)
    return;
  if (contig_locking)
    mtx_enter(&contig_lock);
  b = (struct contig_block *)((char *)p - CONTIG_HDR);
  b->used = 0;
  for (b = contig_head; b != 0; b = b->next) {
    while ((n = b->next) != 0 && !b->used && !n->used) {
      b->size += n->size;
      b->next = n->next;
    }
  }
  if (contig_locking)
    mtx_leave(&contig_lock);
}

void contig_lock_enable(void) { contig_locking = 1; }

// HIBERNATE
void contig_zero_free(void) {
  struct contig_block *b;

  for (b = contig_head; b != 0; b = b->next)
    if (!b->used && b->size > CONTIG_HDR)
      memset((char *)b + CONTIG_HDR, 0, b->size - CONTIG_HDR);
}

void page_init_rest(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t lo, hi;
  uint i;

  for (i = 0; i < bi->memranges; i++) {
    if (bi->mem[i].type != FNU_MEM_RAM)
      continue;
    lo = bi->mem[i].base;
    hi = lo + bi->mem[i].length;
    if (lo < phys_temporary_top)
      lo = phys_temporary_top;
    if (hi > phys_top)
      hi = phys_top;
    if (lo < hi)
      free_uncarved(lo, hi);
  }
  kmem.use_lock = 1;
  printf("KMEM: %u MB free\n", (uint)(get_free_ram() / (1024 * 1024)));
}

void page_free_range(void *vstart, void *vend) {
  char *p;
  p = (char *)PGROUNDUP((uintptr_t)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE) {
    page_free(p);
  }
}

void page_free(char *v) {
  struct run *r;

  if ((uintptr_t)v % PGSIZE || V2P(v) < V2P(end) || V2P(v) >= phys_top) {
    printf("page_free: failed for %p\n", v);
    panic("page_free");
  }

  if (kmem.use_lock) {
    mtx_enter(&kmem.lock);
  }
  r = (struct run *)v;
  if (V2P(v) < 0x100000000ULL) {
    r->next = kmem.lowlist;
    kmem.lowlist = r;
  } else {
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  kmem.nfree++;
  if (kmem.use_lock) {
    mtx_leave(&kmem.lock);
  }
}

static int freelist_sane(struct run *r) {
  uintptr_t va = (uintptr_t)r;

  if (va % PGSIZE || va < DMAP_BASE)
    return 0;
  return V2P(r) >= V2P(end) && V2P(r) < phys_top;
}

static char *take(struct run **list) {
  struct run *r = *list;

  if (r) {
    if (r->next != 0 && !freelist_sane(r->next)) {
      printf("page_alloc: free list damaged at %p, link %p\n", r, r->next);
      panic("page_alloc");
    }
    *list = r->next;
    kmem.nfree--;
  }
  return (char *)r;
}

// last pages: page daemon. few above: kernel. processes get none
int page_reserve_daemon = 4, page_reserve_kernel = 8;
struct proc *page_daemon;

// reserves hold once the daemon runs; before that the boot eats everything
static char *page_take(int user) {
  int guard = page_daemon != 0 && curproc != page_daemon;
  char *p;

  if (kmem.use_lock)
    mtx_enter(&kmem.lock);
  if (guard &&
      kmem.nfree <= (user ? page_reserve_kernel : page_reserve_daemon)) {
    if (kmem.use_lock)
      mtx_leave(&kmem.lock);
    return 0;
  }
  p = take(&kmem.freelist);
  if (p == 0)
    p = take(&kmem.lowlist);
  if (kmem.use_lock)
    mtx_leave(&kmem.lock);
  return p;
}

char *page_alloc(void) { return page_take(0); }

char *page_alloc_user(void) { return page_take(1); }

// HIBERNATE
static void map_list(struct run *r, uint8_t *map, uint64_t pages) {
  for (; r != 0; r = r->next) {
    uint64_t pg = V2P(r) / PGSIZE;

    if (pg < pages)
      map[pg / 8] |= (uint8_t)(1 << (pg % 8));
  }
}

void kalloc_freemap(uint8_t *map, uint64_t pages) {
  memset(map, 0, (pages + 7) / 8);
  if (kmem.use_lock)
    mtx_enter(&kmem.lock);
  map_list(kmem.freelist, map, pages);
  map_list(kmem.lowlist, map, pages);
  if (kmem.use_lock)
    mtx_leave(&kmem.lock);
}

void kalloc_rebuild(const uint8_t *map, uint64_t pages) {
  uint64_t pg;

  if (kmem.use_lock)
    mtx_enter(&kmem.lock);
  kmem.freelist = kmem.lowlist = 0;
  kmem.nfree = 0;
  for (pg = pages; pg-- > 0;) {
    struct run *r;

    if (!(map[pg / 8] & (1 << (pg % 8))))
      continue;
    r = (struct run *)P2V(pg * PGSIZE);
    if (pg * PGSIZE < 0x100000000ULL) {
      r->next = kmem.lowlist;
      kmem.lowlist = r;
    } else {
      r->next = kmem.freelist;
      kmem.freelist = r;
    }
    kmem.nfree++;
  }
  if (kmem.use_lock)
    mtx_leave(&kmem.lock);
}

static int in_run(struct run *r, uint64_t lo, uint64_t hi) {
  return V2P(r) >= lo && V2P(r) < hi;
}

static uint64_t count_run(struct run *r, uint64_t lo, uint64_t hi) {
  uint64_t n = 0;

  for (; r != 0; r = r->next)
    n += in_run(r, lo, hi);
  return n;
}

static void unlink_run(struct run **pp, uint64_t lo, uint64_t hi) {
  while (*pp != 0)
    if (in_run(*pp, lo, hi)) {
      *pp = (*pp)->next;
      kmem.nfree--;
    } else
      pp = &(*pp)->next;
}

// take the run at once or something else grabs it
int kalloc_claim(uint64_t pa, uint64_t npages) {
  uint64_t hi = pa + npages * PGSIZE;
  int r = -1;

  if (kmem.use_lock)
    mtx_enter(&kmem.lock);
  if (count_run(kmem.freelist, pa, hi) + count_run(kmem.lowlist, pa, hi) ==
      npages) {
    unlink_run(&kmem.freelist, pa, hi);
    unlink_run(&kmem.lowlist, pa, hi);
    r = 0;
  }
  if (kmem.use_lock)
    mtx_leave(&kmem.lock);
  return r;
}

char *kalloc_low(void) {
  char *p;

  if (kmem.use_lock)
    mtx_enter(&kmem.lock);
  p = take(&kmem.lowlist);
  if (kmem.use_lock)
    mtx_leave(&kmem.lock);
  return p;
}
