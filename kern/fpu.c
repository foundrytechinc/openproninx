#include "defs.h"
#include "fpu.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

#define CR0_MP 0x00000002
#define CR0_EM 0x00000004
#define CR0_TS 0x00000008
#define CR0_NE 0x00000020
#define CR4_OSFXSR 0x00000200
#define CR4_OSXMMEXCPT 0x00000400

#define MXCSR_DEFAULT 0x1f80

static void fpuclear(void) {
  uint32_t mxcsr = MXCSR_DEFAULT;

  __asm __volatile("fninit");
  __asm __volatile("ldmxcsr %0" : : "m"(mxcsr));
}

void fpusave(struct fxsave *area) { __asm __volatile("fxsave64 %0" : "=m"(*area)); }

static struct fxsave fpu_clean;

void fpuinit(void) {
  lcr0((rcr0() | CR0_MP | CR0_NE) & ~(uintptr_t)(CR0_EM | CR0_TS));
  lcr4(rcr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
  fpuclear();
  fpusave(&fpu_clean);
}

void fpuclean(struct fxsave *area) { *area = fpu_clean; }

void fpurestore(const struct fxsave *area) {
  __asm __volatile("fxrstor64 %0" : : "m"(*area));
}

void fpureset(struct fxsave *area) {
  fpuclear();
  fpusave(area);
}
