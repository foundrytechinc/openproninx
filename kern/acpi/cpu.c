// PROCESSOR PERFORMANCE STATES

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "inc/abi.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "x86.h"

#define NOTIFY_PPC 0x80

#define MSR_PERF_STATUS 0x198
#define MSR_PERF_CTL 0x199
#define MSR_AMD_PSTATE_CTL 0xc0010062
#define MSR_AMD_PSTATE_STATUS 0xc0010063

#define GAS_FFIXED 0x7f
#define GRD_TAG 0x82

#define CPUID_EST (1u << 7)
#define CPUID_HWPSTATE (1u << 7)

struct pstate {
  uint32_t mhz;
  uint32_t mw;
  uint32_t control;
  uint32_t status;
};

struct preg {
  int space;
  uint64_t addr;
  int width;
};

static struct {
  struct aml_node *node;
  struct pstate *s;
  int n;
  struct preg ctl, sts;
  uint32_t ctlmsr, stsmsr;
  int ppc;    // the fastest the firmware allows
  int turbo;  // state 0 is the turbo one, nominal + 1 MHz
  int cur;    // the state set
  int want;   // hw.cpu.freq, MHz; 0 leaves the firmware's state
  int cap;    // percent heat allows
  int nominal;
  char levels[128];
  int on;
} pf = {.cap = 100, .cur = -1};

static int preg(struct aml_value *v, struct preg *r) {
  if (v == 0 || v->type != AML_BUFFER || v->length < 15 || v->buf[0] != GRD_TAG)
    return -1;
  r->space = v->buf[3];
  r->width = v->buf[4];
  memcpy(&r->addr, v->buf + 7, 8);
  return 0;
}

static int pss(struct aml_node *node) {
  struct aml_value res, *e;
  int i, n = 0;

  if (aml_eval_name(node, "_PSS", 0, 0, &res) != 0)
    return 0;
  if (res.type == AML_PACKAGE && res.length > 0) {
    if (pf.s != 0)
      free(pf.s, M_ACPI);
    pf.s = malloc((size_t)res.length * sizeof(*pf.s), M_ACPI, M_WAITOK | M_ZERO);
    for (i = 0; i < res.length; i++) {
      e = res.pkg[i];
      if (e->type != AML_PACKAGE || e->length < 6)
        continue;
      pf.s[n].mhz = (uint32_t)aml_val2int(e->pkg[0]);
      pf.s[n].mw = (uint32_t)aml_val2int(e->pkg[1]);
      pf.s[n].control = (uint32_t)aml_val2int(e->pkg[4]);
      pf.s[n].status = (uint32_t)aml_val2int(e->pkg[5]);
      n++;
    }
  }
  aml_freevalue(&res);
  return n;
}

static int pct(struct aml_node *node) {
  struct aml_value res;
  int err = -1;

  if (aml_eval_name(node, "_PCT", 0, 0, &res) != 0)
    return -1;
  if (res.type == AML_PACKAGE && res.length >= 2 &&
      preg(res.pkg[0], &pf.ctl) == 0 && preg(res.pkg[1], &pf.sts) == 0)
    err = 0;
  aml_freevalue(&res);
  if (err != 0)
    return -1;
  if (pf.ctl.space == GAS_FFIXED) {
    struct fnu_hwinfo hw;
    uint32_t a, b, c, d;

    hwinfo_fill(&hw);
    if (strcmp(hw.cpu_vendor, "GenuineIntel") == 0) {
      cpuid_count(1, 0, &a, &b, &c, &d);
      if (!(c & CPUID_EST))
        return -1;
      pf.ctlmsr = MSR_PERF_CTL;
      pf.stsmsr = MSR_PERF_STATUS;
    } else if ((strcmp(hw.cpu_vendor, "AuthenticAMD") == 0 ||
                strcmp(hw.cpu_vendor, "HygonGenuine") == 0) &&
               hw.cpu_family >= 0x10) {
      cpuid_count(0x80000007, 0, &a, &b, &c, &d);
      if (!(d & CPUID_HWPSTATE))
        return -1;
      pf.ctlmsr = MSR_AMD_PSTATE_CTL;
      pf.stsmsr = MSR_AMD_PSTATE_STATUS;
    } else {
      return -1;
    }
  } else if (pf.ctl.space != GAS_SYSTEM_IOSPACE &&
             pf.ctl.space != GAS_SYSTEM_MEMORY) {
    return -1;
  }
  return 0;
}

static void ppc(void) {
  int64_t v;

  pf.ppc = 0;
  if (aml_eval_int(pf.node, "_PPC", 0, 0, &v) == 0 && v >= 0 && v < pf.n)
    pf.ppc = (int)v;
}

static void request(int i) {
  uint64_t v;
  int w;

  if (pf.ctlmsr != 0) {
    if (pf.ctlmsr == MSR_PERF_CTL) {
      if (rdmsr_safe(MSR_PERF_CTL, &v) != 0)
        return;
      v = (v & ~0xffffull) | (pf.s[i].control & 0xffff);
    } else {
      v = pf.s[i].control;
    }
    wrmsr_safe(pf.ctlmsr, v);
    return;
  }
  v = pf.s[i].control;
  w = pf.ctl.width / 8;
  if (w < 1 || w > 4)
    w = 4;
  acpi_gasio(ACPI_IOWRITE, pf.ctl.space, pf.ctl.addr, w, w, &v);
}

static void apply(int i) {
  struct proc *p = curproc;
  struct cpu_info *held;
  int c;

  if (i == pf.cur || p == 0)
    return;
  held = p->pinned;
  for (c = 0; c < ncpus; c++) {
    if (!cpus[c].started)
      continue;
    sched_peg_curproc(&cpus[c]);
    request(i);
  }
  if (held != 0)
    sched_peg_curproc(held);
  else
    sched_unpeg_curproc();
  pf.cur = i;
}

// the fastest state firmware and heat allow
static int limit(void) {
  int lo = pf.n - 1, hi = pf.ppc;

  return hi + (lo - hi) * (100 - pf.cap) / 100;
}

// pick the state; heat and _PPC can say no
static void select_freq(void) {
  int i, lim = limit();

  if (pf.want == 0) {
    if (pf.cur >= 0 && pf.cur < lim)
      apply(lim);
    return;
  }
  for (i = lim; i < pf.n - 1 && pf.s[i].mhz > (uint32_t)pf.want; i++)
    ;
  apply(i);
}

void acpi_cpu_cap(int percent) {
  if (!pf.on)
    return;
  pf.cap = percent < 0 ? 0 : percent > 100 ? 100 : percent;
  select_freq();
}

int acpi_cpu_has_perf(void) { return pf.on; }

static int freq_written(void *arg __unused, int old __unused) {
  select_freq();
  return 0;
}

static int levels(void *arg __unused, char *buf, size_t size) {
  strlcpy(buf, pf.levels, size);
  return (int)strlen(buf);
}

// speed now: PERF_STATUS times the bus clock, or the table lies
static int64_t cpuspeed(void *arg __unused) {
  uint64_t v, bclk = cpu_bus_clock();
  int i;

  if (pf.stsmsr == MSR_PERF_STATUS && bclk != 0 &&
      rdmsr_safe(MSR_PERF_STATUS, &v) == 0 && ((v >> 8) & 0xff) != 0)
    return (int64_t)(((v >> 8) & 0xff) * bclk / 1000000);
  if (pf.stsmsr != 0 && rdmsr_safe(pf.stsmsr, &v) == 0)
    for (i = 0; i < pf.n; i++)
      if ((v & 0xffff) == (pf.s[i].status & 0xffff))
        return pf.s[i].mhz;
  return pf.s[pf.cur >= 0 ? pf.cur : pf.ppc].mhz;
}

static void ppc_notify(struct aml_node *node __unused, int value, void *arg __unused) {
  if (value != NOTIFY_PPC)
    return;
  ppc();
  select_freq();
}

static int is_cpu(struct aml_node *node);

static int foundcpu(struct aml_node *node, void *arg __unused) {
  if (pf.node != 0 || !is_cpu(node))
    return 0;
  if (!(acpi_getsta(node) & STA_PRESENT))
    return 0;
  if ((pf.n = pss(node)) == 0 || pct(node) != 0) {
    pf.n = 0;
    return 0;
  }
  pf.node = node;
  return 0;
}

// the intel thing. tell them all first
#define CAP_PERF_MSRS 0x01
#define CAP_C1_IO_HALT 0x02
#define CAP_SMP_SAME 0x08
#define CAP_SMP_SAME_C3 0x10
#define CPU_CAPS (CAP_PERF_MSRS | CAP_C1_IO_HALT | CAP_SMP_SAME | CAP_SMP_SAME_C3)

static int is_cpu(struct aml_node *node) {
  struct aml_value res;
  int hid = 0;

  if (node->value == 0)
    return 0;
  if (node->value->type == AML_DEVICE && aml_eval_hid(node, &res) == 0) {
    hid = res.type == AML_STRING && strcmp(res.str, "ACPI0007") == 0;
    aml_freevalue(&res);
  }
  return node->value->type == AML_PROCESSOR || hid;
}

// _OSC or _PDC, else the firmware hides _PSS. thanks a lot
static int tellcpu(struct aml_node *node, void *arg) {
  static const uint8_t uuid[16] = {0x16, 0xa6, 0x77, 0x40, 0x0c, 0x29, 0xbe, 0x47,
                                   0x9e, 0xbd, 0xd8, 0x70, 0x58, 0x71, 0x39, 0x53};
  uint32_t osc[2] = {0, CPU_CAPS}, pdc[3] = {1, 1, CPU_CAPS};
  struct aml_value a[4], res;

  (void)arg;
  if (!is_cpu(node) || !(acpi_getsta(node) & STA_PRESENT))
    return 0;
  memset(a, 0, sizeof(a));
  a[0].type = AML_BUFFER;
  a[0].length = sizeof(uuid);
  a[0].buf = (uint8_t *)uuid;
  a[1].type = AML_INTEGER;
  a[1].integer = 1;
  a[2].type = AML_INTEGER;
  a[2].integer = 2;
  a[3].type = AML_BUFFER;
  a[3].length = sizeof(osc);
  a[3].buf = (uint8_t *)osc;
  if (aml_search(node, "_OSC") != 0 && aml_eval_name(node, "_OSC", 4, a, &res) == 0) {
    if (res.type == AML_BUFFER && res.length >= 4 && (res.buf[0] & 0x0e) != 0)
      printf("acpi: %s _OSC returned status 0x%x\n", aml_nodename(node), res.buf[0]);
    aml_freevalue(&res);
    return 0;
  }
  if (aml_search(node, "_PDC") != 0) {
    a[0].length = sizeof(pdc);
    a[0].buf = (uint8_t *)pdc;
    aml_eval_name(node, "_PDC", 1, a, 0);
  }
  return 0;
}

void acpi_cpu_attach(void) {
  int i;

  aml_walk(aml_root(), 0, tellcpu, 0);
  aml_walk(aml_root(), 0, foundcpu, 0);
  if (pf.node == 0)
    return;
  ppc();
  pf.turbo = pf.n >= 2 && pf.s[0].mhz == pf.s[1].mhz + 1;
  pf.nominal = (int)pf.s[pf.turbo ? 1 : 0].mhz;
  pf.on = 1;
  pf.cur = -1;
  for (i = 0; i < pf.n; i++)
    snprintf(pf.levels + strlen(pf.levels), sizeof(pf.levels) - strlen(pf.levels),
             "%s%u", i ? " " : "", pf.s[i].mhz);
  printf("acpi: %s performance states %s MHz, by %s\n", aml_nodename(pf.node),
         pf.levels, pf.ctlmsr ? "MSR" : "port");
  aml_notify_register(pf.node, ppc_notify, 0);
  sysctl_add_int("hw.cpu.freq", &pf.want, FNU_CTLF_WRITE, 0, 1000000);
  sysctl_on_write("hw.cpu.freq", freq_written, 0);
  sysctl_add_int("hw.cpu.nominal", &pf.nominal, 0, 0, 1000000);
  sysctl_add_string_fn("hw.cpu.levels", levels, 0, 0);
  sysctl_add_quad_fn("hw.cpuspeed", cpuspeed, 0);
}
