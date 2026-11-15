// LOCKUPS
// NMI twice a second; 20 s of silence prints rip and frames
// plus hwlat: who stole my cpu
#include "defs.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

#define MSR_PMC0 0x0c1
#define MSR_EVTSEL0 0x186
#define MSR_K7_EVNTSEL0 0xc0010000
#define MSR_K7_PERFCTR0 0xc0010004
#define MSR_GLOBAL_STATUS 0x38e
#define MSR_GLOBAL_CTRL 0x38f
#define MSR_GLOBAL_OVF_CTRL 0x390
#define MSR_MPERF 0x0e7
#define MSR_APERF 0x0e8
#define EVT_CORE_CYCLES 0x3c
#define EVT_K7_CYCLES 0x76
#define EVTSEL_USR (1u << 16)
#define EVTSEL_OS (1u << 17)
#define EVTSEL_INT (1u << 20)
#define EVTSEL_EN (1u << 22)

#define QUIET_SECONDS 20

volatile uint64_t console_said;
const char *volatile boot_step = "";

static struct {
  uint32_t evtsel, ctr, event;
  int on, version, width;
  uint64_t period, hz;
  uint64_t said, since, reported;
  int64_t smi;
} w;

static void arm(void) {
  wrmsr(w.ctr, (0 - w.period) & ((1ull << w.width) - 1));
  if (w.version >= 2)
    wrmsr(MSR_GLOBAL_OVF_CTRL, 1);
  lapic_pmi_nmi(1);
}

// Intel's architectural counter 0, or AMD's legacy one
static const char *pick(void) {
  uint32_t a, b, c, d, max;

  cpuid_count(0, 0, &max, &b, &c, &d);
  if (b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e) {
    if (max < 0xa)
      return "no architectural performance monitoring";
    cpuid_count(0xa, 0, &a, &b, &c, &d);
    w.version = (int)(a & 0xff);
    w.width = (int)((a >> 16) & 0xff);
    if (w.version < 1 || ((a >> 8) & 0xff) < 1)
      return "no general counter";
    if ((b & 1) != 0)
      return "no core cycle event";
    w.evtsel = MSR_EVTSEL0;
    w.ctr = MSR_PMC0;
    w.event = EVT_CORE_CYCLES;
  } else if (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163) {
    w.width = 48;
    w.evtsel = MSR_K7_EVNTSEL0;
    w.ctr = MSR_K7_PERFCTR0;
    w.event = EVT_K7_CYCLES;
  } else
    return "no known counters";
  if (w.width < 32)
    return "counters too narrow";
  return 0;
}

void lockup_start(void) {
  const char *why;
  uint64_t v = 0;

  w.hz = delay_tsc_hz();
  if (w.hz == 0)
    why = "no tsc rate";
  else
    why = pick();
  if (why == 0 && (rdmsr_safe(w.evtsel, &v) != 0 || (v & EVTSEL_EN) != 0))
    why = v & EVTSEL_EN ? "counter 0 is the firmware's" : "counter 0 faults";
  if (why != 0) {
    printf("lockup: no watch on cpu0, %s\n", why);
    return;
  }
  // no full-width writes: 31 bits sign-extended. intel, thanks
  w.period = w.hz / 2 < 0x7fffffff ? w.hz / 2 : 0x7fffffff;
  wrmsr(w.evtsel, 0);
  w.said = console_said;
  w.since = w.reported = rdtsc();
  w.smi = smi_count();
  arm();
  if (w.version >= 2)
    wrmsr(MSR_GLOBAL_CTRL, rdmsr(MSR_GLOBAL_CTRL) | 1);
  wrmsr(w.evtsel, w.event | EVTSEL_USR | EVTSEL_OS | EVTSEL_INT | EVTSEL_EN);
  w.on = 1;
}

void lockup_stop(void) {
  if (!w.on)
    return;
  w.on = 0;
  wrmsr(w.evtsel, 0);
  lapic_pmi_nmi(0);
}

// 1 when the NMI was the counter's
int lockup_nmi(struct trapframe *tf) {
  uint64_t now, *rbp;
  int64_t smi;
  int i;

  if (!w.on || cpuid() != 0 || (rdmsr(w.ctr) >> (w.width - 1)) & 1)
    return 0;
  now = rdtsc();
  if (console_said != w.said) {
    w.said = console_said;
    w.since = now;
    w.smi = smi_count();
  } else if (now - w.since >= QUIET_SECONDS * w.hz &&
             now - w.reported >= QUIET_SECONDS * w.hz) {
    w.reported = now;
    smi = smi_count();
    printf("lockup: cpu0 silent for %lu s in %s, rip 0x%lx, rflags 0x%lx",
           (unsigned long)((now - w.since) / w.hz), boot_step, (unsigned long)tf->rip,
           (unsigned long)tf->rflags);
    if (w.smi >= 0)
      printf(", %ld SMIs meanwhile", (long)(smi - w.smi));
    printf("\n");
    rbp = (uint64_t *)tf->rbp;
    for (i = 0; i < 12 && ((uintptr_t)rbp >> 63) != 0; i++) {
      printf("  [%d] 0x%lx\n", i, (unsigned long)rbp[1]);
      rbp = (uint64_t *)rbp[0];
    }
    w.said = console_said;
  }
  arm();
  return 1;
}

// 100 ms, interrupts off. a TSC gap over 10 us is SMM, mostly
// SMM, сука
void hwlat_report(const char *where) {
  uint64_t hz = delay_tsc_hz(), t0, prev, now, gap, min_gap, total = 0, longest = 0;
  uint64_t a0 = 0, m0 = 0, a1 = 0, m1 = 0;
  uint64_t flags = readeflags();
  int64_t s0, s1;
  uint32_t a, b, c, d;
  int gaps = 0, perf;

  if (hz == 0)
    return;
  cpuid_count(6, 0, &a, &b, &c, &d);
  perf = (c & 1) != 0;
  min_gap = hz / 100000;
  cli();
  s0 = smi_count();
  if (perf && (rdmsr_safe(MSR_APERF, &a0) != 0 || rdmsr_safe(MSR_MPERF, &m0) != 0))
    perf = 0;
  t0 = prev = rdtsc();
  do {
    now = rdtsc();
    if ((gap = now - prev) > min_gap) {
      gaps++;
      total += gap;
      if (gap > longest)
        longest = gap;
    }
    prev = now;
  } while (now - t0 < hz / 10);
  if (perf && (rdmsr_safe(MSR_APERF, &a1) != 0 || rdmsr_safe(MSR_MPERF, &m1) != 0))
    perf = 0;
  s1 = smi_count();
  if (flags & FL_IF)
    sti();
  if (longest * 1000000 / hz < 1000)
    return;
  printf("hwlat: %s: %d gaps, %lu us of 100 ms away, longest %lu us", where, gaps,
         (unsigned long)(total * 1000000 / hz), (unsigned long)(longest * 1000000 / hz));
  if (s0 >= 0)
    printf(", %ld SMIs", (long)(s1 - s0));
  if (perf && m1 > m0)
    printf(", cpu at %lu MHz", (unsigned long)(hz / 1000000 * (a1 - a0) / (m1 - m0)));
  printf("\n");
}
