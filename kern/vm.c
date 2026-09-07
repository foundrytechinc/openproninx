#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "spinlock.h"
#include "trap.h"
#include "x86.h"

extern char data[]; // defined by kernel.ld
extern char end[];

static uintptr_t iomap_next = IOMAP_BASE;
pte_t *kpgdir;      // for use in scheduler()

extern uintptr_t phys_top;
extern uintptr_t phys_map_top;

#define SIZE_2M 0x200000ULL
#define SIZE_1G 0x40000000ULL

static int have_1g;

static int cpu_has_1g(void) {
  uint32_t a, b, c, d;

  cpuid_count(0x80000000u, 0, &a, &b, &c, &d);
  if (a < 0x80000001u)
    return 0;
  cpuid_count(0x80000001u, 0, &a, &b, &c, &d);
  return (d & (1u << 26)) != 0;
}

#define TSS_LO(p) ((uint32_t)((uintptr_t)(p) & 0xffffffff))
#define TSS_HI(p) ((uint32_t)((uintptr_t)(p) >> 32))

// a fault on an unusable stack triples. these four get their own.
static char ist_stack[NCPU][NIST][ISTSTACKSIZE] __attribute__((aligned(16)));

// kernel segment descriptors, tss and ist. once per cpu.
void seginit(void) {
  struct cpu *c;
  char (*ist)[ISTSTACKSIZE];

  // Configure Page Attribute Table (PAT):
  // PA0=WB(06), PA1=WC(01), PA2=UC-(07), PA3=UC(00)
  // PA4=WB(06), PA5=WC(01), PA6=UC-(07), PA7=UC(00)
  wrmsr(0x277, 0x0007010600070106ULL);

  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  ist = ist_stack[cpuid()];
  c->ts.ist1_31_0 = TSS_LO(ist[IST_DBLFLT - 1] + ISTSTACKSIZE);
  c->ts.ist1_63_32 = TSS_HI(ist[IST_DBLFLT - 1] + ISTSTACKSIZE);
  c->ts.ist2_31_0 = TSS_LO(ist[IST_NMI - 1] + ISTSTACKSIZE);
  c->ts.ist2_63_32 = TSS_HI(ist[IST_NMI - 1] + ISTSTACKSIZE);
  c->ts.ist3_31_0 = TSS_LO(ist[IST_MCHK - 1] + ISTSTACKSIZE);
  c->ts.ist3_63_32 = TSS_HI(ist[IST_MCHK - 1] + ISTSTACKSIZE);
  c->ts.ist4_31_0 = TSS_LO(ist[IST_DEBUG - 1] + ISTSTACKSIZE);
  c->ts.ist4_63_32 = TSS_HI(ist[IST_DEBUG - 1] + ISTSTACKSIZE);
  c->ts.iomb = (uint16_t)sizeof(c->ts);

  lgdt(c->gdt, sizeof(c->gdt));
  // uefi hands the kernel cs 0x38, past the tss descriptor in our table,
  // so the first iretq back into kernel code faults
  __asm__ volatile("pushq %0\n\t"
                   "leaq 1f(%%rip), %%rax\n\t"
                   "pushq %%rax\n\t"
                   "lretq\n"
                   "1:\n\t"
                   "movl %1, %%eax\n\t"
                   "movl %%eax, %%ds\n\t"
                   "movl %%eax, %%es\n\t"
                   "movl %%eax, %%ss\n\t"
                   "movl %%eax, %%fs\n\t"
                   "movl %%eax, %%gs\n\t"
                   :
                   : "i"(SEG_KCODE << 3), "i"(SEG_KDATA << 3)
                   : "rax", "memory");
  *((struct tssdesc *)(&c->gdt[SEG_TSS])) =
      TSSDESC64(STS_T64A, &c->ts, sizeof(c->ts) - 1, 0);
  ltr(SEG_TSS << 3);
}

static uint ptx(const void *va, int level) {
  switch (level) {
  case 4:
    return PTX4(va);
  case 3:
    return PTX3(va);
  case 2:
    return PTX2(va);
  case 1:
    return PTX1(va);
  default:
    panic("ptx: level");
  }
}

static pte_t *__attribute__((noinline))
do_walk(pte_t *table, const void *va, int alloc, int level, int stop) {
  if (level == stop) {
    return &table[ptx(va, level)];
  }

  pte_t *next_table;
  pte_t *pte = &table[ptx(va, level)];
  if (*pte & PTE_P) {
    if (*pte & PTE_PS)
      return NULL;
    next_table = (pte_t *)P2V(PTE_ADDR(*pte));
  } else {
    if (!alloc || (next_table = (pte_t *)kalloc()) == 0) {
      return NULL;
    }
    memset(next_table, 0, PGSIZE);
    // generous here, the leaf entries narrow it
    *pte = V2P(next_table) | PTE_P | PTE_W | PTE_U;
  }
  return do_walk(next_table, va, alloc, level - 1, stop);
}

// the pte for va in pgdir. alloc!=0 builds the tables on the way down.
static pte_t *__attribute__((noinline)) walkpgdir(pte_t *pgdir, const void *va,
                                                  int alloc) {
  return do_walk(pgdir, va, alloc, 4, 1);
}

// map size bytes at va onto pa. neither need be page aligned.
static int __attribute__((noinline)) mappages(pte_t *pgdir, void *va, uint size,
                                              uintptr_t pa, int perm) {
  char *a, *last;
  pte_t *pte;

  a = (char *)PGROUNDDOWN((uintptr_t)va);
  last = (char *)PGROUNDDOWN(((uintptr_t)va) + size - 1);
  for (;;) {
    if ((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if (*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// map [pa, pa+size) at va with the largest pages that fit
int map_range(pte_t *pgdir, uintptr_t va, uintptr_t pa, uint64_t size,
              int perm) {
  pte_t *e;

  size = (size + PGSIZE - 1) & ~(uint64_t)(PGSIZE - 1);
  while (size > 0) {
    if (have_1g && ((va | pa) & (SIZE_1G - 1)) == 0 && size >= SIZE_1G) {
      if ((e = do_walk(pgdir, (void *)va, 1, 4, 3)) == 0)
        return -1;
      *e = pa | perm | PTE_P | PTE_PS;
      va += SIZE_1G, pa += SIZE_1G, size -= SIZE_1G;
    } else if (((va | pa) & (SIZE_2M - 1)) == 0 && size >= SIZE_2M) {
      if ((e = do_walk(pgdir, (void *)va, 1, 4, 2)) == 0)
        return -1;
      *e = pa | perm | PTE_P | PTE_PS;
      va += SIZE_2M, pa += SIZE_2M, size -= SIZE_2M;
    } else {
      if ((e = do_walk(pgdir, (void *)va, 1, 4, 1)) == 0)
        return -1;
      *e = pa | perm | PTE_P;
      va += PGSIZE, pa += PGSIZE, size -= PGSIZE;
    }
  }
  return 0;
}

// user half private, kernel half shared at the top level
pte_t *setupkvm(void) {
  pte_t *pgdir;
  int i;

  if ((pgdir = (pte_t *)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  for (i = NPTENTRIES / 2; i < NPTENTRIES; i++)
    pgdir[i] = kpgdir[i];
  return pgdir;
}

void kvmalloc(void) {
  uintptr_t kend = PGROUNDUP(V2P(end));

  have_1g = cpu_has_1g();

  if ((kpgdir = (pte_t *)kalloc()) == 0)
    panic("kvmalloc: out of memory");
  memset(kpgdir, 0, PGSIZE);

  // spans of real memory only. a cacheable alias of an uncached device
  // page is undefined.
  {
    const struct fnu_bootinfo *bi = bootinfo();
    uint64_t lo, hi, done = 0;
    uint n = 0, s;
    int mapped = 0, ram;

    while (n < bi->memranges) {
      hi = bi->mem[n].base + bi->mem[n].length;
      ram = 0;
      lo = 0;

      for (s = n; s < bi->memranges; s++) {
        if (s > n && bi->mem[s].base > hi)
          break;
        if (bi->mem[s].base + bi->mem[s].length > hi)
          hi = bi->mem[s].base + bi->mem[s].length;
        // start at the first real memory, flash sits just below ram
        if (!ram && bi->mem[s].type == FNU_MEM_RAM) {
          ram = 1;
          lo = bi->mem[s].base;
        }
      }
      n = s;
      if (!ram)
        continue;

      lo &= ~(SIZE_2M - 1);
      hi = (hi + SIZE_2M - 1) & ~(SIZE_2M - 1);
      if (hi > phys_map_top)
        hi = phys_map_top;
      // a large entry over an existing table would strand it
      if (lo < done)
        lo = done;
      if (lo >= hi)
        continue;
      if (map_range(kpgdir, DMAP_BASE + lo, lo, hi - lo, PTE_W) < 0)
        panic("kvmalloc: direct map");
      cprintf("VM: map 0x%x .. 0x%x\n", lo, hi);
      done = hi;
      mapped = 1;
    }
    if (!mapped)
      panic("kvmalloc: nothing to map");
  }
  if (map_range(kpgdir, KERNBASE, 0, EXTMEM, PTE_W) < 0 ||
      map_range(kpgdir, KERNLINK, EXTMEM, V2P(data) - EXTMEM, 0) < 0 ||
      map_range(kpgdir, (uintptr_t)data, V2P(data), kend - V2P(data), PTE_W) <
          0)
    panic("kvmalloc: kernel image");

  // claim the ioremap top level entry now so later mappings reach every pgdir
  if (do_walk(kpgdir, (void *)IOMAP_BASE, 1, 4, 3) == 0)
    panic("kvmalloc: iomap");

  cprintf("VM: direct map built with %s pages\n", have_1g ? "1G" : "2M");
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void switchkvm(void) {
  lcr3(V2P(kpgdir)); // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void switchuvm(struct proc *p) {
  if (p == NULL)
    panic("switchuvm: no process");
  if (p->kstack == NULL)
    panic("switchuvm: no kstack");
  if (p->pgdir == NULL)
    panic("switchuvm: no pgdir");

  pushcli();
  *((struct tssdesc *)(&mycpu()->gdt[SEG_TSS])) =
      TSSDESC64(STS_T64A, &mycpu()->ts, sizeof(mycpu()->ts) - 1, 0);
  mycpu()->ts.rsp0_31_0 =
      (uint32_t)((((uintptr_t)p->kstack) + KSTACKSIZE) & 0xffffffff);
  mycpu()->ts.rsp0_63_32 =
      (uint32_t)(((((uintptr_t)p->kstack) + KSTACKSIZE) >> 32) & 0xffffffff);

  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (uint16_t)sizeof(mycpu()->ts);

  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir)); // switch to process's address space
  popcli();
}

// load initcode at address 0. sz must be under a page.
void inituvm(pte_t *pgdir, char *init, size_t sz) {
  char *mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  if ((mem = kalloc()) == 0)
    panic("inituvm: out of memory");
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W | PTE_U);
  memmove(mem, init, sz);
}

// load one program segment. the pages it covers must already be mapped.
int loaduvm(pte_t *pgdir, char *addr, struct inode *ip, uint offset,
            size_t sz) {
  size_t n;
  uintptr_t pa;
  pte_t *pte;

  uintptr_t va, off;

  // a segment need not start on a page boundary
  for (size_t i = 0; i < sz; i += n) {
    va = (uintptr_t)addr + i;
    off = va % PGSIZE;
    if ((pte = walkpgdir(pgdir, (char *)(va - off), 0)) == 0) {
      panic("loaduvm: address should exist");
    }
    pa = PTE_ADDR(*pte);
    n = PGSIZE - off;
    if (n > sz - i) {
      n = sz - i;
    }
    if (readi(ip, (char *)P2V(pa) + off, offset + i, n) != (int)n) {
      return -1;
    }
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int allocuvm(pte_t *pgdir, size_t oldsz, size_t newsz) {
  char *mem;
  uintptr_t a;

  if (newsz >= KERNBASE)
    return 0;
  if (newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for (; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pgdir, (char *)a, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0) {
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// shrink the process to newsz. neither size need be page aligned, and
// oldsz may be past the end. returns the new size.
int deallocuvm(pte_t *pgdir, size_t oldsz, size_t newsz) {
  pte_t *pte;
  uintptr_t a, pa;

  if (newsz >= oldsz) {
    return oldsz;
  }

  a = PGROUNDUP(newsz);
  for (; a < oldsz; a += PGSIZE) {
    pte = walkpgdir(pgdir, (char *)a, 0);
    if (!pte) {
      // cannot skip whole table
      continue;
    }
    if (*pte & PTE_P) {
      pa = PTE_ADDR(*pte);
      if (pa == 0) {
        panic("deallocuvm");
      }
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }

  return newsz;
}

// frees only the page tables of the user half, the kernel half is shared
static void do_freevm(pte_t *table, int level) {
  int i;

  for (i = 0; i < NPTENTRIES; i++)
    if ((table[i] & PTE_P) && level > 1 && !(table[i] & PTE_PS))
      do_freevm((pte_t *)P2V(PTE_ADDR(table[i])), level - 1);
  kfree((char *)table);
}

void freevm(pte_t *pgdir, uintptr_t utop) {
  int i;

  if (pgdir == 0) {
    panic("freevm: no pgdir");
  }
  deallocuvm(pgdir, utop, 0);
  for (i = 0; i < NPTENTRIES / 2; i++)
    if (pgdir[i] & PTE_P)
      do_freevm((pte_t *)P2V(PTE_ADDR(pgdir[i])), 3);
  kfree((char *)pgdir);
}

// Given a parent process's page table, create a copy
// of it for a child.
pte_t *copyuvm(pte_t *pgdir, size_t sz) {
  pte_t *d;
  pte_t *pte;
  uintptr_t pa, i;
  uint flags;
  char *mem;

  if ((d = setupkvm()) == 0) {
    return NULL;
  }
  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walkpgdir(pgdir, (void *)i, 0)) == 0) {
      panic("copyuvm: pte should exist");
    }
    if (!(*pte & PTE_P)) {
      panic("copyuvm: page not present");
    }
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) {
      goto bad;
    }
    memmove(mem, (char *)P2V(pa), PGSIZE);
    if (mappages(d, (void *)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d, sz);
  return NULL;
}

// clear PTE_U, for the dead page under the user stack
void clearpteu(pte_t *pgdir, char *uva) {
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0) {
    panic("clearpteu");
  }
  *pte &= ~PTE_U;
}

// Map user virtual address to kernel address.
char *uva2ka(pte_t *pgdir, char *uva) {
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0 || (*pte & PTE_P) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  return (char *)P2V(PTE_ADDR(*pte));
}

// copy len bytes to user address va. uva2ka keeps it to PTE_U pages.
int copyout(pte_t *pgdir, uintptr_t va, void *p, size_t len) {
  char *buf, *pa0;
  size_t n;
  uintptr_t va0;

  buf = (char *)p;
  while (len > 0) {
    va0 = (uintptr_t)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char *)va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if (n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

int copyin(pte_t *pgdir, void *p, uintptr_t va, size_t len) {
  char *buf, *pa0;
  size_t n;
  uintptr_t va0;

  buf = (char *)p;
  while (len > 0) {
    va0 = (uintptr_t)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char *)va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if (n > len)
      n = len;
    memmove(buf, pa0 + (va - va0), n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

// runs before the cpu table exists, so no spinlock here
static void *iomap(uint64_t pa, uint64_t size, int perm) {
  uintptr_t off = pa & (PGSIZE - 1);
  uint64_t len = PGROUNDUP(size + off);
  uintptr_t va;

  if (size == 0 || len > IOMAP_SIZE)
    return 0;
  va = __sync_fetch_and_add(&iomap_next, len);
  if (va - IOMAP_BASE > IOMAP_SIZE - len)
    panic("ioremap: window exhausted");
  if (map_range(kpgdir, va, pa - off, len, perm) < 0)
    return 0;
  return (void *)(va + off);
}

void *ioremap(uint64_t pa, uint64_t size) {
  return iomap(pa, size, PTE_W | PTE_PCD);
}

void *ioremap_wc(uint64_t pa, uint64_t size) {
  return iomap(pa, size, PTE_W | PTE_PWT);
}
