#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

#define SIZE_2M 0x200000ULL

void freerange(void *vstart, void *vend);
static void vmem_reserve(void);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

static uintptr_t phys_temporary_top = 4 * 1024 * 1024;
uintptr_t phys_top;
uintptr_t phys_map_top;
static uint64_t ram_bytes;

// phys_top: handed out. phys_map_top: still mapped.
static void detect_memory(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t top = 0, maptop = 0, hi;
  uint i;

  if (bi == 0)
    panic("kinit1: no boot information");

  for (i = 0; i < bi->memranges; i++) {
    if (bi->mem[i].type != FNU_MEM_RAM)
      continue;
    hi = bi->mem[i].base + bi->mem[i].length;
    ram_bytes += bi->mem[i].length;
    if (hi > top)
      top = hi;
  }
  if (top == 0)
    panic("kinit1: no usable memory");

  // firmware next to ram stays mapped, a far mmio hole does not
  maptop = top;
  for (i = 0; i < bi->memranges; i++) {
    hi = bi->mem[i].base + bi->mem[i].length;
    if (bi->mem[i].base <= top && hi > maptop)
      maptop = hi;
  }

  phys_top = (uintptr_t)(top / PGSIZE * PGSIZE);
  phys_map_top = (uintptr_t)((maptop + SIZE_2M - 1) / SIZE_2M * SIZE_2M);
  cprintf("MEM: %u MB ram, top 0x%x, mapping to 0x%x\n",
          (uint)(ram_bytes / (1024 * 1024)), (uint64_t)phys_top,
          (uint64_t)phys_map_top);
}

// the run of ram holding p, touching ranges counted as one
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

uint64_t get_free_ram(void) {
  struct run *r;
  uint64_t free_pages = 0;
  acquire(&kmem.lock);
  r = kmem.freelist;
  while (r) {
    free_pages++;
    r = r->next;
  }
  release(&kmem.lock);
  return free_pages * PGSIZE;
}

// two phases: kinit1 hands out only what entrypgdir maps, kinit2 the rest
// once the full page table is installed.
void kinit1(void *vstart) {
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  detect_memory();
  vmem_reserve();
  uintptr_t end_phys = PGROUNDUP((uintptr_t)V2P(vstart));
  uint64_t run = ram_run_end(end_phys);

  if (run == 0)
    panic("kinit1: the loader left the kernel outside ram");
  if (phys_temporary_top < end_phys + 4 * 1024 * 1024) {
    phys_temporary_top = end_phys + 4 * 1024 * 1024;
  }
  // uefi firmware parks its own pages just above the kernel
  if (phys_temporary_top > run) {
    phys_temporary_top = run;
  }
  if (phys_temporary_top <= end_phys + PGSIZE)
    panic("kinit1: no free memory next to the kernel");
  freerange(vstart, (void *)P2V(phys_temporary_top));
}

static char *gpu_backing_mem = 0;
static uint64_t gpu_backing_phys = 0;
static uintptr_t gpu_reserve_size = 0;

void *gpu_alloc_backing(uint32_t size, uint64_t *phys_out) {
  if (gpu_backing_mem && (size == 0 || size <= gpu_reserve_size)) {
    if (phys_out)
      *phys_out = gpu_backing_phys;
    return gpu_backing_mem;
  }
  return 0;
}

// Contiguous arena for the console: pixel shadow, cell buffers, scanlines.
// Sized from ram, never from a resolution. NetBSD hands rasops a shadow
// the same way and does without when it will not fit.
struct vmem_block {
  struct vmem_block *next;
  uint64_t size;                 // including this header
  int used;
};

static char *vmem_base;
static uint64_t vmem_size;
static struct vmem_block *vmem_head;

#define VMEM_ALIGN 4096
#define VMEM_HDR ((uint64_t)((sizeof(struct vmem_block) + 63) & ~63ULL))

static uintptr_t vmem_phys;

// carve the range now, write into it only after kvmalloc maps everything
static void vmem_reserve(void) {
  uint64_t arena = ram_bytes / 16;

  if (arena < 8 * 1024 * 1024)
    arena = 8 * 1024 * 1024;
  if (arena > 64 * 1024 * 1024)
    arena = 64 * 1024 * 1024;
  arena = (arena + SIZE_2M - 1) / SIZE_2M * SIZE_2M;
  if (phys_top <= (uintptr_t)phys_temporary_top + arena + 16 * 1024 * 1024)
    return;
  phys_top -= arena;
  vmem_phys = phys_top;
  vmem_size = arena;
}

static void vmem_init(char *base, uint64_t size) {
  vmem_base = base;
  vmem_size = size;
  vmem_head = (struct vmem_block *)base;
  vmem_head->next = 0;
  vmem_head->size = size;
  vmem_head->used = 0;
}

void vmem_setup(void) {
  if (vmem_phys != 0)
    vmem_init((char *)P2V(vmem_phys), vmem_size);
}

uint64_t vmem_capacity(void) { return vmem_size; }

void *vmem_alloc(uint64_t bytes) {
  struct vmem_block *b, *split;
  uint64_t need;

  if (vmem_base == 0 || bytes == 0)
    return 0;
  need = (VMEM_HDR + bytes + VMEM_ALIGN - 1) & ~(uint64_t)(VMEM_ALIGN - 1);
  for (b = vmem_head; b != 0; b = b->next) {
    if (b->used || b->size < need)
      continue;
    if (b->size >= need + VMEM_ALIGN * 2) {
      split = (struct vmem_block *)((char *)b + need);
      split->next = b->next;
      split->size = b->size - need;
      split->used = 0;
      b->next = split;
      b->size = need;
    }
    b->used = 1;
    return (char *)b + VMEM_HDR;
  }
  return 0;
}

void vmem_free(void *p) {
  struct vmem_block *b, *n;

  if (p == 0 || vmem_base == 0)
    return;
  b = (struct vmem_block *)((char *)p - VMEM_HDR);
  b->used = 0;
  for (b = vmem_head; b != 0; b = b->next) {
    while ((n = b->next) != 0 && !b->used && !n->used) {
      b->size += n->size;
      b->next = n->next;
    }
  }
}

void kinit2(void) {
  static const uintptr_t want[2] = {16 * 1024 * 1024, 4 * 1024 * 1024};
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t lo, hi;
  uint i;

  for (i = 0; i < NELEM(want); i++) {
    if (phys_top <= (uintptr_t)phys_temporary_top + want[i] * 2)
      continue;
    phys_top -= want[i];
    gpu_reserve_size = want[i];
    gpu_backing_phys = phys_top;
    gpu_backing_mem = (char *)P2V(gpu_backing_phys);
    memset(gpu_backing_mem, 0, want[i]);
    break;
  }

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
      freerange((void *)P2V(lo), (void *)P2V(hi));
  }
  kmem.use_lock = 1;
  cprintf("KMEM: %u MB free, first page phys 0x%x, phys_top 0x%x\n",
          (uint)(get_free_ram() / (1024 * 1024)),
          (uint64_t)(kmem.freelist ? V2P(kmem.freelist) : 0),
          (uint64_t)phys_top);
}

void freerange(void *vstart, void *vend) {
  char *p;
  p = (char *)PGROUNDUP((uintptr_t)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE) {
    kfree(p);
  }
}

// free one page. also used to seed the allocator at boot.
void kfree(char *v) {
  struct run *r;

  if ((uintptr_t)v % PGSIZE || V2P(v) < V2P(end) || V2P(v) >= phys_top) {
    cprintf("kfree: failed for 0x%p\n", v);
    panic("kfree");
  }

  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }
  r = (struct run *)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if (kmem.use_lock) {
    release(&kmem.lock);
  }
}

// one 4096-byte page of physical memory, or 0. a free page holds the next
// link in its first bytes.
static int freelist_sane(struct run *r) {
  uintptr_t va = (uintptr_t)r;

  if (va % PGSIZE || va < DMAP_BASE)
    return 0;
  return V2P(r) >= V2P(end) && V2P(r) < phys_top;
}

char *kalloc(void) {
  struct run *r;

  if (kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    if (r->next != 0 && !freelist_sane(r->next)) {
      cprintf("kalloc: free list damaged at 0x%p, link 0x%p\n", r, r->next);
      panic("kalloc");
    }
    kmem.freelist = r->next;
  }
  if (kmem.use_lock)
    release(&kmem.lock);
  return (char *)r;
}
