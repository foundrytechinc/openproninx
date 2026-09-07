#include "defs.h"
#include "mmu.h"
#include "x86.h"

#define MSR_MCG_CAP 0x179
#define MSR_MCG_STATUS 0x17a
#define MSR_MCG_CTL 0x17b
#define MSR_MC_CTL(i) (0x400 + (i) * 4)
#define MSR_MC_STATUS(i) (0x401 + (i) * 4)
#define MSR_MC_ADDR(i) (0x402 + (i) * 4)
#define MSR_MC_MISC(i) (0x403 + (i) * 4)

#define MCG_CAP_COUNT 0xff
#define MCG_CAP_CTL_P 0x100

#define MC_STATUS_PCC 0x0200000000000000ULL
#define MC_STATUS_ADDRV 0x0400000000000000ULL
#define MC_STATUS_MISCV 0x0800000000000000ULL
#define MC_STATUS_UC 0x2000000000000000ULL
#define MC_STATUS_OVER 0x4000000000000000ULL
#define MC_STATUS_VAL 0x8000000000000000ULL

#define MCHK_BANKS_MAX 32

static uint banks;

// what the banks held when we arrived. the console does not exist yet when
// mcheck_init runs, so it is kept and said from kmain.
static struct mc_record {
  uint64_t status, addr, misc;
  uint bank;
} leftover[MCHK_BANKS_MAX];
static uint leftovers;
static int collected;

static void mc_say(const char *when, uint bank, uint64_t status, uint64_t addr,
                   uint64_t misc) {
  cprintf("MCE: %s cpu%d bank %d status 0x%x %s%s%s", when, cpuid(), bank,
          status, (status & MC_STATUS_UC) ? "uncorrected" : "corrected",
          (status & MC_STATUS_PCC) ? " context-lost" : "",
          (status & MC_STATUS_OVER) ? " overflow" : "");
  if (status & MC_STATUS_ADDRV)
    cprintf(" addr 0x%x", addr);
  if (status & MC_STATUS_MISCV)
    cprintf(" misc 0x%x", misc);
  cprintf("\n");
}

int mcheck_report(const char *when) {
  uint64_t status;
  uint i;
  int found = 0;

  for (i = 0; i < banks; i++) {
    status = rdmsr(MSR_MC_STATUS(i));
    if (!(status & MC_STATUS_VAL))
      continue;
    found++;
    mc_say(when, i, status,
           (status & MC_STATUS_ADDRV) ? rdmsr(MSR_MC_ADDR(i)) : 0,
           (status & MC_STATUS_MISCV) ? rdmsr(MSR_MC_MISC(i)) : 0);
    wrmsr(MSR_MC_STATUS(i), 0);
  }
  return found;
}

// banks outlive a warm reset
static void mcheck_collect(void) {
  uint64_t status;
  uint i;

  for (i = 0; i < banks && leftovers < MCHK_BANKS_MAX; i++) {
    status = rdmsr(MSR_MC_STATUS(i));
    if (!(status & MC_STATUS_VAL))
      continue;
    leftover[leftovers].bank = i;
    leftover[leftovers].status = status;
    leftover[leftovers].addr =
        (status & MC_STATUS_ADDRV) ? rdmsr(MSR_MC_ADDR(i)) : 0;
    leftover[leftovers].misc =
        (status & MC_STATUS_MISCV) ? rdmsr(MSR_MC_MISC(i)) : 0;
    leftovers++;
  }
}

void mcheck_report_boot(void) {
  uint i;

  for (i = 0; i < leftovers; i++)
    mc_say("left over", leftover[i].bank, leftover[i].status,
           leftover[i].addr, leftover[i].misc);
}

// without CR4.MCE a machine check just resets the board
void mcheck_init(void) {
  uint64_t cap;
  uint sig, feat, ebx, ecx, family, model, i;

  cpuid_count(1, 0, &sig, &ebx, &ecx, &feat);
  if (!(feat & (1u << 7)))
    return;

  if (feat & (1u << 14)) {
    cap = rdmsr(MSR_MCG_CAP);
    banks = (uint)(cap & MCG_CAP_COUNT);
    if (banks > MCHK_BANKS_MAX)
      banks = MCHK_BANKS_MAX;

    if (!collected) {
      collected = 1;
      mcheck_collect();
    }

    if (cap & MCG_CAP_CTL_P)
      wrmsr(MSR_MCG_CTL, ~0ULL);

    family = (sig >> 8) & 0xf;
    model = (sig >> 4) & 0xf;
    if (family == 0xf)
      family += (sig >> 20) & 0xff;
    if (family == 0x6 || family == 0xf)
      model += ((sig >> 16) & 0xf) << 4;

    for (i = 0; i < banks; i++) {
      // p6 before nehalem keeps bank 0 enabled and reserved
      if (!(family == 0x6 && model < 0x1a && i == 0))
        wrmsr(MSR_MC_CTL(i), ~0ULL);
      wrmsr(MSR_MC_STATUS(i), 0);
    }
    wrmsr(MSR_MCG_STATUS, 0);
  }

  lcr4(rcr4() | CR4_MCE);
}
