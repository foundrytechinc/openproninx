// LOW MEMORY
// under 1 MB, for code that starts in real mode, top down, not the first 64K
#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"

#define LOW_FLOOR 0x10000      // BIOSes write under it, asleep and awake
#define BIOS_START_MIN 0x20000 // an EBDA or base memory under it is a lie
#define BIOS_START_MAX 0x9f000

static uint64_t ceiling; // the BIOS's own from here to 1 MB
static uint64_t next;    // the next page given is under this

// BDA 0x413 base memory, 0x40E EBDA
static uint64_t bios_start(void) {
  const volatile uint8_t *bda = (const volatile uint8_t *)P2V(0x400);
  uint64_t start = (uint64_t)(bda[0x13] | bda[0x14] << 8) << 10;
  uint64_t ebda = (uint64_t)(bda[0x0e] | bda[0x0f] << 8) << 4;

  if (start < BIOS_START_MIN || start > BIOS_START_MAX)
    start = BIOS_START_MAX;
  if (ebda >= BIOS_START_MIN && ebda < start)
    start = ebda;
  return start & ~(uint64_t)(PGSIZE - 1);
}

void lowmem_init(void) {
  ceiling = next = bios_start();
}

// npages contiguous pages of the firmware's ram, or 0
uint64_t lowmem_alloc(int npages) {
  uint64_t len = (uint64_t)npages * PGSIZE, top;

  if (npages <= 0 || next == 0)
    return 0;
  for (top = next; top >= LOW_FLOOR + len; top -= PGSIZE)
    if (bootinfo_is_ram(top - len, top)) {
      next = top - len;
      return next;
    }
  printf("LOWMEM: no %d pages of ram between 0x%x and 0x%lx\n", npages, LOW_FLOOR,
         (unsigned long)next);
  return 0;
}

uint64_t lowmem_ceiling(void) { return ceiling; }

// real mode paging tables: first GB, direct map, kernel
// redone at every use, firmware scribbles on them
uint64_t lowmem_tables(void) {
  static uint64_t pml4;
  uint64_t *p4, *p3, *pd, pdpt_pa, pd_pa;
  int i;

  if (pml4 == 0 && (pml4 = lowmem_alloc(3)) == 0)
    return 0;
  pdpt_pa = pml4 + PGSIZE;
  pd_pa = pml4 + 2 * PGSIZE;
  p4 = P2V(pml4);
  p3 = P2V(pdpt_pa);
  pd = P2V(pd_pa);
  memset(p4, 0, 3 * PGSIZE);
  for (i = 0; i < 512; i++)
    pd[i] = ((uint64_t)i << 21) | PTE_P | PTE_W | PTE_PS;
  p3[0] = pd_pa | PTE_P | PTE_W;
  p3[510] = pd_pa | PTE_P | PTE_W;
  p4[0] = pdpt_pa | PTE_P | PTE_W;
  p4[PTX4(DMAP_BASE)] = pdpt_pa | PTE_P | PTE_W;
  p4[511] = pdpt_pa | PTE_P | PTE_W;
  return pml4;
}

// the first GB as itself, for a pml4 of one's own
uint64_t lowmem_identity(void) {
  uint64_t pml4 = lowmem_tables();

  return pml4 != 0 ? pml4 + PGSIZE : 0;
}
