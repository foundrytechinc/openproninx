// LOCAL APIC

#include "defs.h"
#include "param.h"
#include "proc.h"
#include "trap.h"
#include "x86.h"

#define MSR_APIC_BASE 0x1b
#define APIC_BASE_X2APIC (1ull << 10)
#define APIC_BASE_ENABLE (1ull << 11)
#define MSR_X2APIC 0x800
#define CPUID2_X2APIC (1u << 21)

#define ID (0x0020 / 4)
#define VER (0x0030 / 4)
#define TPR (0x0080 / 4)
#define EOI (0x00B0 / 4)
#define SVR (0x00F0 / 4)
#define SVR_ENABLE 0x00000100
#define ESR (0x0280 / 4)
#define ICRLO (0x0300 / 4)
#define ICR_DELIVM_INIT 0x00000500
#define ICR_DELIVM_STARTUP 0x00000600
#define ICR_DELIVS 0x00001000
#define ICR_LEVEL_ASSERT 0x00004000
#define ICR_TRIGGER_LEVEL 0x00008000
#define ICR_DEST_OTHERS 0x000c0000
#define ICRHI (0x0310 / 4)
#define TIMER (0x0320 / 4)
#define TIMER_PERIODIC 0x00020000
#define PCINT (0x0340 / 4)
#define LINT0 (0x0350 / 4)
#define LINT1 (0x0360 / 4)
#define ERROR (0x0370 / 4)
#define LVT_MASKED 0x00010000
#define LVT_DM_NMI 0x00000400
#define LVT_DM_EXTINT 0x00000700
#define LVT_ACTIVE_LOW 0x00002000
#define TICR (0x0380 / 4)
#define TCCR (0x0390 / 4)
#define TDCR (0x03E0 / 4)
#define TDCR_X1 0x0000000B

volatile uint32_t *lapic;
int x2apic_mode;

static uint32_t lapicr(int index) {
  return x2apic_mode ? (uint32_t)rdmsr(MSR_X2APIC + (uint32_t)index / 4)
                     : lapic[index];
}

static void lapicw(int index, uint32_t value) {
  if (x2apic_mode) {
    wrmsr(MSR_X2APIC + (uint32_t)index / 4, value);
    return;
  }
  lapic[index] = value;
  lapic[ID];
}

static void ipi_wait(int spins) {
  int i;

  if (x2apic_mode)
    return;
  for (i = 0; i < spins && (lapic[ICRLO] & ICR_DELIVS); i++)
    __asm __volatile("pause");
}

static void ipi(uint32_t dest, uint32_t lo) {
  if (x2apic_mode) {
    wrmsr(MSR_X2APIC + ICRLO / 4, ((uint64_t)dest << 32) | lo);
    return;
  }
  lapicw(ICRHI, dest << 24);
  lapicw(ICRLO, lo);
}

void lapic_x2apic_enable(void) {
  uint64_t base = rdmsr(MSR_APIC_BASE);

  if (x2apic_mode && !(base & APIC_BASE_X2APIC))
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE | APIC_BASE_X2APIC);
}

void lapic_choose_mode(uint32_t maxid) {
  uint32_t a, b, c, d;
  const char *want = kgetenv("hw.x2apic_enable");
  int can;

  cpuid_count(1, 0, &a, &b, &c, &d);
  can = (c & CPUID2_X2APIC) != 0;
  if (rdmsr(MSR_APIC_BASE) & APIC_BASE_X2APIC)
    x2apic_mode = 1;
  else if (can && (maxid > 254 || (want != 0 && want[0] == '1')))
    x2apic_mode = 1;
  else if (maxid > 254)
    printf("LAPIC: APIC ids past 254 and no x2APIC to reach them\n");
  if (x2apic_mode) {
    lapic_x2apic_enable();
    printf("LAPIC: x2APIC mode\n");
  }
}


static void reset_esr(void) {
  lapicw(ESR, 0);
  lapicw(ESR, 0);
}

void lapiceoi(void) {
  if (lapic) {
    lapicw(EOI, 0);
  }
}

static uint32_t lapic_tval = 10000000;
static uint64_t lapic_per_second;
static int calibrated;

static uint32_t tccr(void) { return lapicr(TCCR); }

// 50 ms against the TSC, ends in brackets
static void lapic_calibrate(void) {
  uint64_t t0 = 0, t1 = 1, tsc_hz = delay_tsc_hz(), hz;
  uint32_t first, last;
  int bad = 0;

  lapicw(TIMER, LVT_MASKED);
  lapicw(TDCR, TDCR_X1);
  lapicw(TICR, 0xffffffff);
  if (tsc_hz != 0) {
    bad = tsc_bracket(tccr, &t0, &first) < 0;
    delay(50000);
    bad |= tsc_bracket(tccr, &t1, &last) < 0;
  } else {
    first = lapicr(TCCR);
    delay(50000);
    last = lapicr(TCCR);
    tsc_hz = 20;
  }
  lapicw(TICR, 0);
  if (bad) {
    printf("lapic: SMIs left no clean read of the timer\n");
    return;
  }
  if (first <= last || t1 <= t0)
    return;
  hz = (uint64_t)(first - last) * tsc_hz / (t1 - t0);
  if (hz < 1000000 || hz > 4000000000ULL)
    return;
  lapic_per_second = hz;
  lapic_tval = (uint32_t)(hz / HZ);
}

uint64_t lapic_timer_hz(void) { return lapic_per_second; }

uint32_t lapic_timer_reload(void) { return lapic_tval; }

// LINT0 ExtINT masked, LINT1 NMI, unless the tables say otherwise. the tables lie
#define NLINTNMI 16
static struct {
  int by, id, lint, low;
} lintnmi[NLINTNMI];
static int nlintnmi;

void lapic_lint_nmi(int by, int id, int lint, int flags) {
  if (lint < 0 || lint > 1 || nlintnmi >= NLINTNMI)
    return;
  if (((flags >> 2) & 3) == 3)
    printf("lapic: Forcing LINT%d to edge trigger\n", lint);
  lintnmi[nlintnmi].by = by;
  lintnmi[nlintnmi].id = id;
  lintnmi[nlintnmi].lint = lint;
  lintnmi[nlintnmi].low = (flags & 3) == 3;
  nlintnmi++;
}

static uint32_t lint_value(int pin) {
  uint32_t v = pin == 0 ? LVT_MASKED | LVT_DM_EXTINT : LVT_DM_NMI;
  int me = lapicid(), uid = -1, i;

  for (i = 0; i < ncpus; i++)
    if (cpus[i].apicid == (uint32_t)me)
      uid = cpus[i].ci_acpi_proc_id;
  for (i = 0; i < nlintnmi; i++) {
    if (lintnmi[i].lint != pin)
      continue;
    if (lintnmi[i].id != -1 && lintnmi[i].id != (lintnmi[i].by == LINT_BY_UID ? uid : me))
      continue;
    v = LVT_DM_NMI | (lintnmi[i].low ? LVT_ACTIVE_LOW : 0);
  }
  return v;
}

void lapicinit(void) {
  if (!lapic) {
    return;
  }

  lapic_x2apic_enable();
  lapicw(SVR, SVR_ENABLE | T_APIC_SPURIOUS);
  // the vector masked while old errors are cleared
  lapicw(ERROR, LVT_MASKED | T_APIC_ERROR);
  reset_esr();

  if (!calibrated) {
    calibrated = 1;
    lapic_calibrate();
  }
  lapicw(TDCR, TDCR_X1);
  lapicw(TIMER, TIMER_PERIODIC | T_APIC_TIMER);
  lapicw(TICR, lapic_tval);

  lapicw(LINT0, lint_value(0));
  lapicw(LINT1, lint_value(1));

  if (((lapicr(VER) >> 16) & 0xFF) >= 4) {
    lapicw(PCINT, LVT_MASKED);
  }

  lapicw(ERROR, T_APIC_ERROR);

  reset_esr();

  lapiceoi();

  lapicw(TPR, 0);
}

// keep the sleep quiet; errors wait in ESR
void lapic_suspend(void) {
  if (!lapic)
    return;
  lapicw(ERROR, LVT_MASKED | T_APIC_ERROR);
  lapicw(TIMER, LVT_MASKED | T_APIC_TIMER);
  lapicw(LINT0, LVT_MASKED);
  lapicw(LINT1, LVT_MASKED);
  if (((lapicr(VER) >> 16) & 0xFF) >= 4)
    lapicw(PCINT, LVT_MASKED);
  reset_esr();
}

// a boot step that left an error in the local APIC
void lapic_esr_check(const char *step) {
  uint32_t esr;

  if ((!lapic && !x2apic_mode) || (esr = lapic_esr()) == 0)
    return;
  printf("cpu%d: local APIC error 0x%x by %s\n", lapicid(), esr, step);
  lapicw(ESR, 0);
}

// what ESR gathered since the last clear
uint32_t lapic_esr(void) {
  if (!lapic && !x2apic_mode)
    return 0;
  lapicw(ESR, 0);
  return lapicr(ESR);
}

// perf counter overflow as an NMI, or masked
void lapic_pmi_nmi(int on) {
  if (lapic || x2apic_mode)
    lapicw(PCINT, LVT_DM_NMI | (on ? 0 : LVT_MASKED));
}

// ???
void lapic_print_state(void) {
  printf("lapic: base msr 0x%lx, version 0x%x, svr 0x%x, tpr 0x%x, icr 0x%x:0x%x, lint0 "
         "0x%x, lint1 0x%x\n",
         (unsigned long)rdmsr(MSR_APIC_BASE), lapicr(VER), lapicr(SVR), lapicr(TPR),
         x2apic_mode ? 0 : lapicr(ICRHI), lapicr(ICRLO), lapicr(LINT0), lapicr(LINT1));
}

void lapic_error(void) {
  static uint said;
  uint32_t esr;

  lapicw(ESR, 0);
  esr = lapicr(ESR);
  if (esr != 0 && ++said <= 8)
    printf("cpu%d: local APIC error 0x%x\n", lapicid(), esr);
}

// TPR shut while the bios runs with its own IF. ебал я этот bios
uint32_t lapic_introff(void) {
  uint32_t was;

  if (lapic == 0 && !x2apic_mode)
    return 0;
  was = lapicr(TPR);
  lapicw(TPR, 0xff);
  return was;
}

void lapic_intron(uint32_t tpr) {
  if (lapic == 0 && !x2apic_mode)
    return;
  lapicw(TPR, tpr);
}

int lapicid(void) {
  if (!lapic)
    return 0;
  return x2apic_mode ? (int)lapicr(ID) : (int)(lapic[ID] >> 24);
}

// park every other core
void lapic_halt_others(void) {
  struct cpu_info *c, *me;
  uint64_t flags;
  int us;

  if (!lapic)
    return;
  flags = readeflags();
  cli();
  me = curcpu();
  ipi(0, ICR_DEST_OTHERS | ICR_LEVEL_ASSERT | T_IPI_HALT);
  ipi_wait(100000);
  for (us = 0; us < 1000000; us++) {
    for (c = cpus; c < cpus + ncpus; c++)
      if (c != me && c->started && !c->halted)
        break;
    if (c == cpus + ncpus)
      break;
    delay(1);
  }
  if (c != cpus + ncpus)
    printf("lapic: cpu%d did not stop\n", (int)(c - cpus));
  if (flags & FL_IF)
    sti();
}

void lapic_notify(uint32_t apicid) {
  if (!lapic)
    return;
  ipi_wait(100000);
  ipi(apicid, ICR_LEVEL_ASSERT | T_IPI_SIGNOTIFY);
}

void lapic_tlb_others(void) {
  if (!lapic)
    return;
  ipi_wait(100000);
  ipi(0, ICR_DEST_OTHERS | ICR_LEVEL_ASSERT | T_IPI_TLB);
}

void lapic_halt_to(uint32_t apicid) {
  if (!lapic)
    return;
  ipi(apicid, ICR_LEVEL_ASSERT | T_IPI_HALT);
  ipi_wait(100000);
}

// x2APIC has no delivery status: the write is the send
static int ipi_sent(int us) {
  int i;

  if (x2apic_mode)
    return 1;
  for (i = 0; i < us && (lapic[ICRLO] & ICR_DELIVS); i++)
    delay(1);
  return !(lapic[ICRLO] & ICR_DELIVS);
}

// 10 ms after INIT is for the 82489DX. nobody has one
static uint init_udelay(void) {
  static uint us = ~0u;
  uint32_t a, b, c, d, fam;

  if (us != ~0u)
    return us;
  cpuid_count(0, 0, &a, &b, &c, &d);
  if (b == 0x756e6547 || b == 0x68747541 || b == 0x6f677948) { // Intel, AMD, Hygon
    int intel = b == 0x756e6547, hygon = b == 0x6f677948;

    cpuid_count(1, 0, &a, &b, &c, &d);
    fam = (a >> 8) & 0xf;
    if (fam == 0xf)
      fam += (a >> 20) & 0xff;
    if ((intel && fam == 6) || (hygon && fam >= 0x18) || (!intel && !hygon && fam >= 0xf))
      return us = 0;
  }
  return us = 10000;
}

// INIT, let go, STARTUP twice; 1 or 2 if something stayed unsent
static int startap(uint32_t dest, uint32_t shorthand, uint32_t addr) {
  uint wait = init_udelay();
  int i;

  reset_esr();
  ipi(dest, shorthand | ICR_DELIVM_INIT | ICR_LEVEL_ASSERT | (shorthand ? 0 : ICR_TRIGGER_LEVEL));
  if (!ipi_sent(100))
    return 1;
  delay(wait);
  if (shorthand == 0) {
    ipi(dest, ICR_DELIVM_INIT | ICR_TRIGGER_LEVEL);
    if (!ipi_sent(100))
      return 1;
  }
  for (i = 0; i < 2; i++) {
    reset_esr();
    ipi(dest, shorthand | ICR_DELIVM_STARTUP | ICR_LEVEL_ASSERT | ((addr >> 12) & 0xff));
    delay(wait == 0 ? 10 : 300);
    if (!ipi_sent(100))
      return 2;
    delay(wait == 0 ? 10 : 200);
  }
  return 0;
}

int lapicstartap(uint32_t apicid, uint32_t addr) { return startap(apicid, 0, addr); }

// every other cpu, whatever its id
int lapicstartap_all(uint32_t addr) { return startap(0, ICR_DEST_OTHERS, addr); }
