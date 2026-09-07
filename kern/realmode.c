#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "realmode.h"
#include "spinlock.h"
#include "x86.h"

extern uchar _binary_rmtramp_start[];
extern uchar _binary_rmtramp_size[];
extern pte_t *kpgdir;

static struct spinlock rmlock;
static int ready;

// the trampoline, its stack and its page tables all sit under 1M
static int low_memory_is_ram(uint64_t base, uint64_t end) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t at = base;
  uint i, progress;

  if (bi == 0)
    return 0;
  while (at < end) {
    progress = 0;
    for (i = 0; i < bi->memranges; i++) {
      if (bi->mem[i].type != FNU_MEM_RAM || bi->mem[i].base > at ||
          bi->mem[i].base + bi->mem[i].length <= at)
        continue;
      at = bi->mem[i].base + bi->mem[i].length;
      progress = 1;
      break;
    }
    if (!progress)
      return 0;
  }
  return 1;
}

uint32_t realmode_ivt(int vector) {
  const volatile uint32_t *ivt = (const volatile uint32_t *)P2V(0);
  return ivt[vector & 0xff];
}

int realmode_available(void) { return ready; }

void *realmode_buffer(void) { return P2V(RM_BUF); }

void realmode_init(void) {
  uint64_t *pdpt = (uint64_t *)P2V(RM_PDPT);
  uint64_t *pd = (uint64_t *)P2V(RM_PD);
  int i;

  const struct fnu_bootinfo *bi = bootinfo();

  initlock(&rmlock, "realmode");

  // uefi leaves no bios behind the ivt
  if (bi != 0 && bi->firmware == FNU_FIRMWARE_UEFI)
    return;
  // an empty int 10h vector means no video bios was ever posted here
  if (realmode_ivt(0x10) == 0)
    return;
  // and the firmware has to agree that the low pages are ours to scribble on
  if (!low_memory_is_ram(RM_CODE, RM_PD + PGSIZE))
    return;

  memmove(P2V(RM_CODE), _binary_rmtramp_start,
          (uintptr_t)_binary_rmtramp_size);
  memset(pdpt, 0, PGSIZE);
  memset(pd, 0, PGSIZE);
  for (i = 0; i < 512; i++)
    pd[i] = ((uint64_t)i << 21) | PTE_P | PTE_W | PTE_PS;
  pdpt[0] = RM_PD | PTE_P | PTE_W;
  ready = 1;
}

// leaving long mode wants a table that maps the kernel and the low meg 1:1
int realmode_int(int vector, struct realregs *r) {
  volatile uint16_t *br = (volatile uint16_t *)P2V(RM_DATA);
  uint64_t *pml4 = (uint64_t *)P2V(RM_PML4);
  void (*enter)(void);
  uint64_t cr3;

  if (!ready)
    return -1;

  acquire(&rmlock);
  memmove(pml4, kpgdir, PGSIZE);
  pml4[0] = RM_PDPT | PTE_P | PTE_W;

  br[BR_AX / 2] = r->ax;
  br[BR_BX / 2] = r->bx;
  br[BR_CX / 2] = r->cx;
  br[BR_DX / 2] = r->dx;
  br[BR_SI / 2] = r->si;
  br[BR_DI / 2] = r->di;
  br[BR_ES / 2] = r->es;
  br[BR_VEC / 2] = (uint16_t)vector;

  // the blob is linked at RM_CODE, callable once the identity map is live
  enter = (void (*)(void))(uintptr_t)RM_CODE;
  cr3 = rcr3();
  lcr3(RM_PML4);
  enter();
  lcr3(cr3);

  r->ax = br[BR_AX / 2];
  r->bx = br[BR_BX / 2];
  r->cx = br[BR_CX / 2];
  r->dx = br[BR_DX / 2];
  release(&rmlock);
  return 0;
}
