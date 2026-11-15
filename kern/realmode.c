#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "realmode.h"
#include "mutex.h"
#include "x86.h"

extern uchar _binary_rmtramp_start[];
extern uchar _binary_rmtramp_size[];
extern pte_t *kernel_pgdir;

static struct mutex rmlock;
static int ready;

uint32_t realmode_ivt(int vector) {
  const volatile uint32_t *ivt = (const volatile uint32_t *)P2V(0);
  return ivt[vector & 0xff];
}

int realmode_available(void) { return ready; }

static uint64_t rm_pa, rm_pml4; // from lowmem

void *realmode_buffer(void) { return P2V(rm_pa + RM_BUF); }

uint32_t realmode_buffer_lin(void) { return (uint32_t)(rm_pa + RM_BUF); }

void realmode_init(void) {
  const struct fnu_bootinfo *bi = bootinfo();

  mtx_init(&rmlock, "realmode");

  if (bi != 0 && bi->firmware == FNU_FIRMWARE_UEFI) {
    x86bios_init();
    return;
  }
  if (realmode_ivt(0x10) == 0)
    return;
  if ((uintptr_t)_binary_rmtramp_size > RM_DATA)
    panic("realmode: the code runs into its data");
  if ((rm_pa = lowmem_alloc(RM_PAGES)) == 0 || (rm_pml4 = lowmem_alloc(1)) == 0 ||
      lowmem_identity() == 0) {
    printf("realmode: no low memory, no bios calls\n");
    return;
  }
  memmove(P2V(rm_pa), _binary_rmtramp_start, (uintptr_t)_binary_rmtramp_size);
  ready = 1;
  x86bios_init();
}

int realmode_int(int vector, struct realregs *r) {
  volatile uint16_t *br = (volatile uint16_t *)P2V(rm_pa + RM_DATA);
  uint64_t *pml4 = (uint64_t *)P2V(rm_pml4);
  void (*enter)(void);
  struct cpu_info *c;
  uint64_t cr3;
  uint32_t tpr;

  if (!ready)
    return -1;

  mtx_enter(&rmlock);
  c = curcpu();
  memmove(pml4, kernel_pgdir, PGSIZE);
  pml4[0] = lowmem_identity() | PTE_P | PTE_W;
  *(volatile uint64_t *)P2V(rm_pa + RM_DATA + BS_CR3) = rm_pml4;

  br[BR_AX / 2] = r->ax;
  br[BR_BX / 2] = r->bx;
  br[BR_CX / 2] = r->cx;
  br[BR_DX / 2] = r->dx;
  br[BR_SI / 2] = r->si;
  br[BR_DI / 2] = r->di;
  br[BR_ES / 2] = r->es;
  br[BR_VEC / 2] = (uint16_t)vector;

  enter = (void (*)(void))(uintptr_t)rm_pa;
  tpr = lapic_introff();
  cr3 = rcr3();
  lcr3(rm_pml4);
  enter();
  lcr3(cr3);
  wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
  lapic_intron(tpr);

  r->ax = br[BR_AX / 2];
  r->bx = br[BR_BX / 2];
  r->cx = br[BR_CX / 2];
  r->dx = br[BR_DX / 2];
  mtx_leave(&rmlock);
  return 0;
}
