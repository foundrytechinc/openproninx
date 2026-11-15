#include "defs.h"
#include "elf.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"
#include "trap.h"
#include "x86.h"

extern char data[];
extern char end[];

static ulong iomap_next = IOMAP_BASE;
pte_t *kernel_pgdir;

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

static char bsp_ist[NIST][ISTSTACKSIZE] __aligned(16);

// once per cpu
int nx_ready;

pte_t vm_nx(void) { return nx_ready ? PTE_NX : 0; }

void nxinit(void) {
  uint32_t eax, ebx, ecx, edx;

  cpuid_count(0x80000001, 0, &eax, &ebx, &ecx, &edx);
  if (!(edx & (1u << 20)))
    return;
  wrmsr(0xc0000080, rdmsr(0xc0000080) | (1ull << 11)); // EFER.NXE
  nx_ready = 1;
}

int smap_on;

static void smap_init(void) {
  uint32_t eax, ebx, ecx, edx;

  cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
  if (ebx & (1u << 7))
    lcr4(rcr4() | CR4_SMEP);
  if (ebx & (1u << 20)) {
    lcr4(rcr4() | CR4_SMAP);
    smap_on = 1;
  }
}

void gdt_init(void) {
  struct cpu_info *c;
  char (*ist)[ISTSTACKSIZE];

  wrmsr(0x277, 0x0007010600070106ULL);
  lcr0(rcr0() | CR0_WP);
  smap_init();
  nxinit();

  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  ist = c->ist != 0 ? (char (*)[ISTSTACKSIZE])c->ist : bsp_ist;
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
  __asm __volatile("pushq %0\n\t"
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
  wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
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


static pte_t *__noinline
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
    if (!alloc ||
        (next_table = (pte_t *)(alloc == WALK_NOSLEEP ? page_alloc()
                                                      : page_alloc_wait(0))) == 0) {
      return NULL;
    }
    memset(next_table, 0, PGSIZE);
    *pte = V2P(next_table) | PTE_P | PTE_W | PTE_U;
  }
  return do_walk(next_table, va, alloc, level - 1, stop);
}

pte_t *__noinline hat_pte(pte_t *pgdir, const void *va,
                                                  int alloc) {
  return do_walk(pgdir, va, alloc, 4, 1);
}

static int __noinline hat_map(pte_t *pgdir, void *va, uint size,
                                              uintptr_t pa, pte_t perm) {
  char *a, *last;
  pte_t *pte;

  a = (char *)PGROUNDDOWN((uintptr_t)va);
  last = (char *)PGROUNDDOWN(((uintptr_t)va) + size - 1);
  for (;;) {
    if ((pte = hat_pte(pgdir, a, 1)) == 0)
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

int map_range(pte_t *pgdir, uintptr_t va, uintptr_t pa, uint64_t size,
              pte_t perm) {
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

pte_t *hat_alloc(void) {
  pte_t *pgdir;
  int i;

  if ((pgdir = (pte_t *)page_alloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  for (i = NPTENTRIES / 2; i < NPTENTRIES; i++)
    pgdir[i] = kernel_pgdir[i];
  return pgdir;
}

void hat_kern_setup(void) {
  uintptr_t kend = PGROUNDUP(V2P(end));

  have_1g = cpu_has_1g();

  if ((kernel_pgdir = (pte_t *)page_alloc()) == 0)
    panic("hat_kern_setup: out of memory");
  memset(kernel_pgdir, 0, PGSIZE);

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
      if (lo < done)
        lo = done;
      if (lo >= hi)
        continue;
      if (map_range(kernel_pgdir, DMAP_BASE + lo, lo, hi - lo, PTE_W | vm_nx()) < 0)
        panic("hat_kern_setup: direct map");
      done = hi;
      mapped = 1;
    }
    if (!mapped)
      panic("hat_kern_setup: nothing to map");
  }
  if (map_range(kernel_pgdir, KERNBASE, 0, EXTMEM, PTE_W | vm_nx()) < 0 ||
      map_range(kernel_pgdir, KERNLINK, EXTMEM, V2P(data) - EXTMEM, 0) < 0 ||
      map_range(kernel_pgdir, (uintptr_t)data, V2P(data),
                kend - V2P(data), PTE_W | vm_nx()) < 0)
    panic("hat_kern_setup: kernel image");

  if (do_walk(kernel_pgdir, (void *)IOMAP_BASE, 1, 4, 3) == 0 ||
      do_walk(kernel_pgdir, (void *)KHEAP_BASE, 1, 4, 3) == 0)
    panic("hat_kern_setup: iomap");

  hat_switch(kernel_pgdir);
}

void hat_switch(pte_t *pgdir) {
  uint64_t pa = V2P(pgdir);

  if (rcr3() != pa)
    lcr3(pa);
}

void tss_kstack(struct proc *p) {
  uintptr_t top = (uintptr_t)p->kstack + KSTACKSIZE;

  curcpu()->ts.rsp0_31_0 = TSS_LO(top);
  curcpu()->ts.rsp0_63_32 = TSS_HI(top);
}

int hat_memload(pte_t *pgdir, uintptr_t va, char *mem, pte_t perm) {
  return hat_map(pgdir, (void *)va, PGSIZE, V2P(mem), perm | PTE_U);
}

static void table_free(pte_t *table, int level) {
  int i;

  for (i = 0; i < NPTENTRIES; i++) {
    if (level == 1) {
      if (table[i] & PTE_SWAPPED)
        swap_free_slot(table[i]);
      else if (table[i] & PTE_P)
        page_free(P2V(PTE_ADDR(table[i])));
    } else if ((table[i] & PTE_P) && !(table[i] & PTE_PS))
      table_free((pte_t *)P2V(PTE_ADDR(table[i])), level - 1);
  }
  page_free((char *)table);
}

void hat_free(pte_t *pgdir) {
  int i;

  if (pgdir == 0)
    panic("hat_free: no tables");
  for (i = 0; i < NPTENTRIES / 2; i++)
    if (pgdir[i] & PTE_P)
      table_free((pte_t *)P2V(PTE_ADDR(pgdir[i])), 3);
  page_free((char *)pgdir);
}


int map_sigcode(pte_t *pgdir, uintptr_t va, const char *code, uint len) {
  char *mem;

  if (len > PGSIZE)
    return -1;
  if ((mem = page_alloc()) == 0)
    return -1;
  memset(mem, 0xcc, PGSIZE);
  memmove(mem, code, len);
  if (hat_map(pgdir, (void *)va, PGSIZE, V2P(mem), PTE_U) < 0) {
    page_free(mem);
    return -1;
  }
  return 0;
}

uint64_t hat_unload(pte_t *pgdir, uintptr_t va, size_t len) {
  uint64_t n = 0;
  uintptr_t a;
  pte_t *pte;

  for (a = PGROUNDDOWN(va); a < va + len; a += PGSIZE) {
    if ((pte = hat_pte(pgdir, (void *)a, 0)) == 0)
      continue;
    if (*pte & PTE_SWAPPED) {
      swap_free_slot(*pte);
      *pte = 0;
      continue;
    }
    if (!(*pte & PTE_P))
      continue;
    page_free(P2V(PTE_ADDR(*pte)));
    *pte = 0;
    n++;
  }
  return n;
}

static uint64_t resident(const pte_t *table, int level) {
  uint64_t n = 0;
  int i;

  for (i = 0; i < NPTENTRIES; i++) {
    if (!(table[i] & PTE_P) || !(table[i] & PTE_U))
      continue;
    if (level == 1)
      n++;
    else if (table[i] & PTE_PS)
      n += (uint64_t)1 << (9 * (level - 1));
    else
      n += resident((const pte_t *)P2V(PTE_ADDR(table[i])), level - 1);
  }
  return n;
}

uint64_t hat_resident(pte_t *pgdir) {
  uint64_t n = 0;
  int i;

  for (i = 0; i < NPTENTRIES / 2; i++)
    if (pgdir[i] & PTE_P)
      n += resident((const pte_t *)P2V(PTE_ADDR(pgdir[i])), 3);
  return n;
}

int hat_chgprot(pte_t *pgdir, uintptr_t va, size_t len, int prot) {
  uintptr_t a;
  pte_t *pte;

  for (a = PGROUNDDOWN(va); a < va + len; a += PGSIZE) {
    if ((pte = hat_pte(pgdir, (void *)a, 0)) == 0 || !(*pte & PTE_P))
      continue;
    if (prot & FNU_MEM_WRITE)
      *pte |= PTE_W;
    else
      *pte &= ~(pte_t)PTE_W;
    if (prot & FNU_MEM_EXEC)
      *pte &= ~PTE_NX;
    else
      *pte |= vm_nx();
    if (prot & (FNU_MEM_READ | FNU_MEM_WRITE | FNU_MEM_EXEC))
      *pte |= PTE_U;
    else
      *pte &= ~(pte_t)PTE_U;
  }
  return 0;
}

int hat_range_empty(pte_t *pgdir, uintptr_t va, size_t len) {
  uintptr_t a;
  pte_t *pte;

  for (a = PGROUNDDOWN(va); a < va + len; a += PGSIZE) {
    pte = hat_pte(pgdir, (void *)a, 0);
    if (pte != 0 && (*pte & (PTE_P | PTE_SWAPPED)))
      return 0;
  }
  return 1;
}

static int may_fault(void) {
  int ok;

  spinlock_enter();
  ok = curcpu()->spinlock_count == 1;
  spinlock_exit();
  return ok;
}

char *uva_kva(pte_t *pgdir, char *uva) {
  pte_t *pte;

  if ((uintptr_t)uva >= USERTOP)
    return 0;
  pte = hat_pte(pgdir, uva, 0);
  if (pte == 0 || (*pte & PTE_P) == 0) {
    if (!may_fault() || vm_fault_pgdir(pgdir, (uintptr_t)uva, 0) != 0)
      return 0;
    pte = hat_pte(pgdir, uva, 0);
    if (pte == 0 || (*pte & PTE_P) == 0)
      return 0;
  }
  if ((*pte & PTE_U) == 0)
    return 0;
  return (char *)P2V(PTE_ADDR(*pte));
}

// the same for a write
static char *uva_kva_write(pte_t *pgdir, char *uva) {
  pte_t *pte;

  pte = hat_pte(pgdir, uva, 0);
  if (pte == 0 || (*pte & PTE_P) == 0 || (*pte & PTE_CACHE) ||
      !(*pte & PTE_W)) {
    if (!may_fault() || vm_fault_pgdir(pgdir, (uintptr_t)uva, 1) != 0)
      return 0;
    pte = hat_pte(pgdir, uva, 0);
    if (pte == 0 || (*pte & PTE_P) == 0 || !(*pte & PTE_W))
      return 0;
  }
  return uva_kva(pgdir, uva);
}

int uva_prefault(pte_t *pgdir, uintptr_t va, int write) {
  char *ka = write ? uva_kva_write(pgdir, (char *)va) : uva_kva(pgdir, (char *)va);

  return ka != 0 ? 0 : -1;
}

int uva_range_ok(pte_t *pgdir, uintptr_t va, size_t len) {
  uintptr_t a;

  if (len == 0)
    return 0;
  if (va + len < va)
    return -1;
  for (a = PGROUNDDOWN(va); a < va + len; a += PGSIZE)
    if (uva_kva(pgdir, (char *)a) == 0)
      return -1;
  return 0;
}

int copy_user(void *dst, const void *src, size_t len, uintptr_t *onfault);
int copy_user_str(char *dst, const char *src, size_t len, size_t *done,
                  uintptr_t *onfault);

static int own_space(pte_t *pgdir, uintptr_t va, size_t len) {
  return curproc != 0 && pgdir == curproc->p_p->pgdir && va < USERTOP &&
         len <= USERTOP - va;
}

int copyout(pte_t *pgdir, uintptr_t va, void *p, size_t len) {
  char *buf, *pa0;
  size_t n;
  uintptr_t va0;

  if (own_space(pgdir, va, len))
    return copy_user((void *)va, p, len, &curproc->pcb_onfault) == 0 ? 0 : -1;
  buf = (char *)p;
  while (len > 0) {
    va0 = (uintptr_t)PGROUNDDOWN(va);
    pa0 = uva_kva_write(pgdir, (char *)va0);
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

  if (own_space(pgdir, va, len))
    return copy_user(p, (void *)va, len, &curproc->pcb_onfault) == 0 ? 0 : -1;
  buf = (char *)p;
  while (len > 0) {
    va0 = (uintptr_t)PGROUNDDOWN(va);
    pa0 = uva_kva(pgdir, (char *)va0);
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

int copyinstr(pte_t *pgdir, char *buf, uintptr_t va, size_t len, size_t *done) {
  size_t got = 0, n;
  uintptr_t va0;
  char *pa0;

  if (own_space(pgdir, va, 1))
    return copy_user_str(buf, (const char *)va,
                         len < USERTOP - va ? len : USERTOP - va, done,
                         &curproc->pcb_onfault);

  while (got < len) {
    va0 = (uintptr_t)PGROUNDDOWN(va);
    if ((pa0 = uva_kva(pgdir, (char *)va0)) == 0)
      return FNU_ERR_FAULT;
    for (n = va - va0; n < PGSIZE && got < len; n++, va++)
      if ((buf[got++] = pa0[n]) == 0) {
        if (done != 0)
          *done = got;
        return 0;
      }
  }
  return FNU_ERR_NAMETOOLONG;
}

static void *iomap(uint64_t pa, uint64_t size, int perm) {
  uintptr_t off = pa & (PGSIZE - 1);
  uint64_t len = PGROUNDUP(size + off);
  uintptr_t va;

  if (size == 0 || len > IOMAP_SIZE / 2)
    return 0;
  va = atomic_fetchadd_long(&iomap_next, len);
  if (va - IOMAP_BASE > IOMAP_SIZE / 2 - len)
    panic("ioremap: window exhausted");
  if (map_range(kernel_pgdir, va, pa - off, len, perm) < 0)
    return 0;
  return (void *)(va + off);
}

#define KMAP_BASE (IOMAP_BASE + IOMAP_SIZE / 2)
#define KMAP_SIZE (IOMAP_SIZE / 2)

static struct {
  struct mutex lock;
  uintptr_t next;
  int unmapped;
} kmap = {MUTEX_INITIALIZER("kmap"), KMAP_BASE, 0};

uintptr_t kmap_alloc(uint64_t len) {
  uintptr_t va = 0;

  len = PGROUNDUP(len);
  if (len == 0)
    return 0;
  mtx_enter(&kmap.lock);
  if (kmap.next - KMAP_BASE <= KMAP_SIZE - len) {
    va = kmap.next;
    kmap.next += len;
  }
  mtx_leave(&kmap.lock);
  return va;
}

void kmap_page(uintptr_t va, uint64_t pa, int write, int exec, int cache) {
  pte_t perm = 0, *pte;

  if (write)
    perm |= PTE_W;
  if (!exec)
    perm |= vm_nx();
  if (cache == KMAP_UNCACHED)
    perm |= PTE_PCD;
  else if (cache == KMAP_COMBINING)
    perm |= PTE_PWT;
  if ((pte = do_walk(kernel_pgdir, (void *)va, 1, 4, 1)) == 0)
    panic("kmap_page: no table for 0x%lx", va);
  *pte = pa | perm | PTE_P;
}

// lock held
int hat_kmap(uintptr_t va, char *page) {
  pte_t *pte = do_walk(kernel_pgdir, (void *)va, WALK_NOSLEEP, 4, 1);

  if (pte == 0)
    return -1;
  *pte = V2P(page) | PTE_W | vm_nx() | PTE_P;
  return 0;
}

char *hat_kunmap(uintptr_t va) {
  pte_t *pte = do_walk(kernel_pgdir, (void *)va, 0, 4, 1);
  uint64_t pa;

  if (pte == 0 || !(*pte & PTE_P))
    return 0;
  pa = PTE_ADDR(*pte);
  *pte = 0;
  __asm __volatile("invlpg (%0)" : : "r"(va) : "memory");
  return P2V(pa);
}

void v2p_bad(uintptr_t va) { panic("V2P of 0x%lx, which is not direct-mapped", va); }

void kunmap_page(uintptr_t va) {
  pte_t *pte = do_walk(kernel_pgdir, (void *)va, 0, 4, 1);

  if (pte != 0)
    *pte = 0;
  __asm __volatile("invlpg (%0)" : : "r"(va) : "memory");
  kmap.unmapped = 1;
}

int kva2pa(uintptr_t va, uint64_t *pa) {
  pte_t *table = kernel_pgdir, e;
  int level;

  for (level = 4; level >= 1; level--) {
    e = table[ptx((void *)va, level)];
    if (!(e & PTE_P))
      return 0;
    if (level == 1) {
      *pa = PTE_ADDR(e) + (va & (PGSIZE - 1));
      return 1;
    }
    if (e & PTE_PS) {
      uint64_t span = level == 3 ? SIZE_1G : SIZE_2M;

      *pa = (PTE_ADDR(e) & ~(span - 1)) + (va & (span - 1));
      return 1;
    }
    table = (pte_t *)P2V(PTE_ADDR(e));
  }
  return 0;
}

void tlb_flush_local(void) { lcr3(rcr3()); }

static volatile uint tlb_gen;
static volatile uint tlb_seen[MAXCPU];
static volatile uint tlb_busy;

static void tlb_poll(void) {
  int me = (int)(curcpu() - cpus);
  uint g = tlb_gen;

  if (tlb_seen[me] != g) {
    tlb_flush_local();
    tlb_seen[me] = g;
  }
}

void kmap_update(void) {
  spinlock_enter();
  tlb_flush_local();
  if (kmap.unmapped && ncpus >= 2) {
    kmap.unmapped = 0;
    tlb_shootdown();
  }
  spinlock_exit();
}

void tlb_shootdown(void) {
  uint gen;
  int i, me;

  if (ncpus < 2)
    return;
  spinlock_enter();
  me = (int)(curcpu() - cpus);
  while (xchg(&tlb_busy, 1) != 0) {
    tlb_poll();
    __asm __volatile("pause");
  }
  gen = ++tlb_gen;
  tlb_flush_local();
  tlb_seen[me] = gen;
  lapic_tlb_others();
  for (i = 0; i < ncpus; i++) {
    if (i == me)
      continue;
    while (cpus[i].started && tlb_seen[i] != gen) {
      tlb_poll();
      __asm __volatile("pause");
    }
  }
  __asm __volatile("movl $0, %0" : "+m"(tlb_busy) :);
  spinlock_exit();
}

void tlb_flush_ack(void) { tlb_poll(); }

void *ioremap(uint64_t pa, uint64_t size) {
  return iomap(pa, size, PTE_W | PTE_PCD);
}

void *ioremap_wc(uint64_t pa, uint64_t size) {
  return iomap(pa, size, PTE_W | PTE_PWT);
}
