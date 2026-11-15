// MTRRS

#include "defs.h"
#include "inc/bootinfo.h"
#include "mmu.h"
#include "x86.h"

#define MSR_MTRRCAP 0x0fe
#define MSR_MTRRDEF 0x2ff
#define MSR_MTRRVAR 0x200
#define MTRRCAP_VCNT(c) ((c) & 0xff)
#define MTRRCAP_FIX (1u << 8)
#define MTRRDEF_E (1u << 11)
#define CPUID_MTRR (1u << 12)
#define NVAR 32

static const uint32_t fixed[] = {0x250, 0x258, 0x259, 0x268, 0x269,
                                 0x26a, 0x26b, 0x26c, 0x26d, 0x26e, 0x26f};

static struct {
  int saved;
  int nvar, fix;
  uint64_t def;
  uint64_t var[2 * NVAR];
  uint64_t fixv[sizeof(fixed) / sizeof(fixed[0])];
} m;

static int has_mtrr(void) {
  uint32_t a, b, c, d;

  cpuid_count(1, 0, &a, &b, &c, &d);
  return (d & CPUID_MTRR) != 0;
}

// the boot processor's, kept
void mtrr_save(void) {
  uint64_t cap;
  uint i;

  if (!has_mtrr())
    return;
  cap = rdmsr(MSR_MTRRCAP);
  m.nvar = (int)MTRRCAP_VCNT(cap);
  if (m.nvar > NVAR)
    m.nvar = NVAR;
  m.fix = (cap & MTRRCAP_FIX) != 0;
  m.def = rdmsr(MSR_MTRRDEF);
  for (i = 0; i < (uint)(2 * m.nvar); i++)
    m.var[i] = rdmsr(MSR_MTRRVAR + i);
  for (i = 0; m.fix && i < sizeof(fixed) / sizeof(fixed[0]); i++)
    m.fixv[i] = rdmsr(fixed[i]);
  m.saved = 1;
}

void mtrr_load(void) {
  uintptr_t cr0, cr4;
  uint i;

  if (!m.saved || !has_mtrr())
    return;
  cr0 = rcr0();
  cr4 = rcr4();
  lcr0((cr0 | CR0_CD) & ~(uintptr_t)CR0_NW);
  wbinvd();
  if (cr4 & CR4_PGE)
    lcr4(cr4 & ~(uintptr_t)CR4_PGE);
  lcr3(rcr3());
  wrmsr(MSR_MTRRDEF, m.def & ~(uint64_t)MTRRDEF_E);
  for (i = 0; i < (uint)(2 * m.nvar); i++)
    wrmsr(MSR_MTRRVAR + i, m.var[i]);
  for (i = 0; m.fix && i < sizeof(fixed) / sizeof(fixed[0]); i++)
    wrmsr(fixed[i], m.fixv[i]);
  wrmsr(MSR_MTRRDEF, m.def);
  wbinvd();
  lcr3(rcr3());
  lcr0(cr0);
  lcr4(cr4);
}

// ram not write-back runs at bus speed, ??? whatever the page tables say
#define MTRR_UC 0
#define MTRR_WC 1
#define MTRR_WT 4
#define MTRR_WP 5
#define MTRR_WB 6
#define MTRRVAR_V (1u << 11)
#define STEP (64 * 1024)

static uint64_t phys_mask;

// SDM 11.11.4.1: UC wins, then WT over WB
static int var_type(uint64_t pa) {
  int i, t = -1, ty;

  for (i = 0; i < m.nvar; i++) {
    uint64_t base = m.var[2 * i], mask = m.var[2 * i + 1];

    if (!(mask & MTRRVAR_V) || ((pa ^ base) & mask & phys_mask & ~0xfffull) != 0)
      continue;
    ty = (int)(base & 0xff);
    if (ty == MTRR_UC)
      return MTRR_UC;
    if (t == -1 || (t == MTRR_WB && ty == MTRR_WT))
      t = ty;
  }
  return t != -1 ? t : (int)(m.def & 0xff);
}

void mtrr_check_ram(void) {
  static const char *const names[8] = {"uncached", "write-combining", "?", "?",
                                       "write-through", "write-protect", "write-back", "?"};
  const struct fnu_bootinfo *bi = bootinfo();
  uint64_t other[8] = {0}, pa, end, any = 0;
  uint32_t a, b, c, d;
  uint i;

  if (bi == 0 || !has_mtrr())
    return;
  mtrr_save();
  cpuid_count(0x80000000, 0, &a, &b, &c, &d);
  phys_mask = 0xfffffffffull;
  if (a >= 0x80000008) {
    cpuid_count(0x80000008, 0, &a, &b, &c, &d);
    phys_mask = (1ull << (a & 0xff)) - 1;
  }
  if (!(m.def & MTRRDEF_E)) {
    printf("MTRR: off, all memory uncached\n");
    return;
  }
  for (i = 0; i < bi->memranges; i++) {
    if (bi->mem[i].type != FNU_MEM_RAM)
      continue;
    end = bi->mem[i].base + bi->mem[i].length;
    for (pa = bi->mem[i].base < 0x100000 ? 0x100000 : bi->mem[i].base; pa < end; pa += STEP) {
      int t = var_type(pa) & 7;

      if (t != MTRR_WB) {
        other[t] += STEP;
        any = 1;
      }
    }
  }
  if (!any)
    return;
  printf("MTRR: the firmware leaves ram not write-back:");
  for (i = 0; i < 8; i++)
    if (other[i] != 0)
      printf(" %lu MB %s", (unsigned long)(other[i] >> 20), names[i]);
  printf("\nMTRR: default %s, ", names[m.def & 7]);
  for (i = 0; i < (uint)m.nvar; i++)
    if (m.var[2 * i + 1] & MTRRVAR_V)
      printf(" 0x%lx/0x%lx %s", (unsigned long)(m.var[2 * i] & ~0xfffull),
             (unsigned long)(m.var[2 * i + 1] & phys_mask & ~0xfffull),
             names[m.var[2 * i] & 7]);
  printf("\n");
}
