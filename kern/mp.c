// MULTIPROCESSOR

#include "mp.h"
#include "apboot.h"
#include "inc/bootinfo.h"
#include "kclock.h"
#include "acpi.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "trap.h"
#include "x86.h"

#define LAPIC_DEFAULT_BASE 0xfee00000ull
#define IOAPIC_DEFAULT_BASE 0xfec00000ull
#define MSR_APIC_BASE 0x1b
#define ID (0x0020 / 4)

struct cpu_info cpus[MAXCPU];
int ncpus;

static uint8_t sum(uint8_t *addr, int len) {
  int i, sum;

  sum = 0;
  for (i = 0; i < len; i++)
    sum += addr[i];
  return sum;
}

static struct mp *mpsearch1(uintptr_t a, int len) {
  uint8_t *e, *p, *addr;

  addr = (uint8_t *)P2V(a);
  e = addr + len;
  for (p = addr; p < e; p += sizeof(struct mp))
    if (memcmp(p, "_MP_", 4) == 0 && sum(p, sizeof(struct mp)) == 0)
      return (struct mp *)p;
  return 0;
}

static int lowmem_ok(uint64_t base, uint64_t len) {
  return base != 0 && len != 0 && base + len <= 0x100000;
}

static struct mp *mpsearch(void) {
  uchar *bda;
  uint p;
  struct mp *mp;

  bda = (uchar *)P2V(0x400);
  if ((p = ((bda[0x0F] << 8) | bda[0x0E]) << 4) != 0) {
    if (lowmem_ok(p, 1024) && (mp = mpsearch1(p, 1024)))
      return mp;
  } else {
    p = ((bda[0x14] << 8) | bda[0x13]) * 1024;
    if (p >= 1024 && lowmem_ok(p - 1024, 1024) &&
        (mp = mpsearch1(p - 1024, 1024)))
      return mp;
  }
  return mpsearch1(0xF0000, 0x10000);
}

// the MP configuration table
static struct mpconf *mpconfig(struct mp **pmp) {
  struct mpconf *conf;
  struct mp *mp;

  if ((mp = mpsearch()) == 0 || mp->physaddr == 0)
    return 0;
  conf = (struct mpconf *)P2V((uintptr_t)mp->physaddr);
  if (memcmp(conf, "PCMP", 4) != 0)
    return 0;
  if (conf->version != 1 && conf->version != 4)
    return 0;
  if (sum((uchar *)conf, conf->length) != 0)
    return 0;
  *pmp = mp;
  return conf;
}

static struct mpconf *saved_mp_conf;

#define NPCIROUTE 96
static struct {
  uint8_t bus, srcirq, intin;
  uint16_t flags;
} pciroute[NPCIROUTE];
static int npciroute;

static void mp_routes(struct mpconf *conf) {
  uchar *p, *e;
  uint8_t pcibus[256 / 8] = {0};

  for (p = (uchar *)(conf + 1), e = (uchar *)conf + conf->length; p < e;) {
    switch (*p) {
    case MPPROC:
      p += sizeof(struct mpproc);
      continue;
    case MPBUS: {
      struct mpbus *b = (struct mpbus *)p;
      if (memcmp(b->busname, "PCI   ", 6) == 0)
        pcibus[b->busid / 8] |= (uint8_t)(1 << (b->busid % 8));
      p += 8;
      continue;
    }
    case MPIOINTR: {
      struct mpiointr *in = (struct mpiointr *)p;
      if (in->inttype == 0 && (pcibus[in->srcbus / 8] & (1 << (in->srcbus % 8))) &&
          npciroute < NPCIROUTE) {
        pciroute[npciroute].bus = in->srcbus;
        pciroute[npciroute].srcirq = in->srcirq;
        pciroute[npciroute].intin = in->dstintin;
        pciroute[npciroute].flags = in->flags;
        npciroute++;
      }
      p += 8;
      continue;
    }
    case MPIOAPIC:
    case MPLINTR:
      p += *p == MPIOAPIC ? (int)sizeof(struct mpioapic) : 8;
      continue;
    default:
      return;
    }
  }
}

int mp_pci_route(int bus, int device, int pin, int *intin, int *level, int *low) {
  int i;

  for (i = 0; i < npciroute; i++)
    if (pciroute[i].bus == bus && pciroute[i].srcirq == ((device << 2) | pin)) {
      int po = pciroute[i].flags & 3, el = (pciroute[i].flags >> 2) & 3;

      *intin = pciroute[i].intin;
      *low = po == 0 || po == 3;
      *level = el == 0 || el == 3;
      return 0;
    }
  return -1;
}

static int nodesc;

static void mp_find_cpus(void) {
  uchar *p, *e;
  int ismp;
  struct mp *mp;
  struct mpconf *mp_conf;
  struct mpproc *mp_proc;

  // the MP table only where there is no MADT
  if (acpi_mp_init() == 0)
    return;
  if ((mp_conf = mpconfig(&mp)) == 0) {
    uint64_t base = rdmsr(MSR_APIC_BASE) & 0xfffff000ull;

    if (base == 0)
      base = LAPIC_DEFAULT_BASE;
    lapic = (uint32_t *)ioremap(base, 4096);
    nodesc = 1;
    cpus[0].ci_acpi_proc_id = -1;
    ncpus = 1;
    printf("SMP: no MADT and no MP table; one processor, local apic at "
            "0x%lx\n", base);
    return;
  }
  saved_mp_conf = mp_conf;
  mp_routes(mp_conf);
  ismp = 1;
  lapic = (uint32_t *)ioremap(mp_conf->lapicaddr, 4096);

  for (p = (uchar *)(mp_conf + 1), e = (uchar *)mp_conf + mp_conf->length;
       p < e;) {
    switch (*p) {
    case MPPROC:
      mp_proc = (struct mpproc *)p;
      if (ncpus < MAXCPU) {
        cpus[ncpus].apicid = mp_proc->apicid;
        cpus[ncpus].ci_acpi_proc_id = -1;
        ncpus++;
      }
      p += sizeof(struct mpproc);
      continue;
    case MPIOAPIC:
      p += sizeof(struct mpioapic);
      continue;
    case MPLINTR: {
      struct mpiointr *in = (struct mpiointr *)p;

      if (in->inttype == 1)
        lapic_lint_nmi(LINT_BY_APIC, in->dstapic == 0xff ? -1 : in->dstapic,
                       in->dstintin, in->flags);
      p += 8;
      continue;
    }
    case MPBUS:
    case MPIOINTR:
      p += 8;
      continue;
    default:
      ismp = 0;
      break;
    }
  }

  if (!ismp) {
    panic("Didn't find a suitable machine");
  }

  if (mp->imcrp) {
    outb(0x22, 0x70);
    outb(0x23, inb(0x23) | 1);
  }
}

void mpinit(void) {
  uint32_t maxid = 0;
  int i;

  mp_find_cpus();
  for (i = 0; i < ncpus; i++)
    if (cpus[i].apicid > maxid)
      maxid = cpus[i].apicid;
  lapic_choose_mode(maxid);
  if (nodesc)
    cpus[0].apicid = (uint32_t)lapicid();
}

int mp_ioapic_each(void (*fn)(int id, uint64_t addr, int gsibase)) {
  struct mpioapic *io;
  uchar *p, *e;
  int n = 0;

  if (saved_mp_conf == 0) {
    fn(-1, IOAPIC_DEFAULT_BASE, 0);
    return 1;
  }
  for (p = (uchar *)(saved_mp_conf + 1),
      e = (uchar *)saved_mp_conf + saved_mp_conf->length;
       p < e;) {
    switch (*p) {
    case MPPROC:
      p += sizeof(struct mpproc);
      continue;
    case MPIOAPIC:
      io = (struct mpioapic *)p;
      if (io->flags & 1) {
        fn(io->apicno, io->addr, -1);
        n++;
      }
      p += sizeof(struct mpioapic);
      continue;
    default:
      p += 8;
      continue;
    }
  }
  return n;
}

extern char _binary_entryother_start[];
extern char _binary_entryother_size[];
void mpentry(void);
void mp_startup(void);

static uint64_t ap_pa; // the start page, from lowmem

static volatile uint16_t *ap_hdr16(int off) { return (volatile uint16_t *)P2V(ap_pa + (uint)off); }
static volatile uint64_t *ap_hdr64(int off) { return (volatile uint64_t *)P2V(ap_pa + (uint)off); }

static void ap_step(int step) { *ap_hdr16(AP_HDR_STEP) = (uint16_t)step; }

void mp_startup(void) {
  ap_step(5);
  lapic_x2apic_enable();
  ap_step(6);
  mtrr_load();
  percpu_attach();
  ap_step(7);
  gdt_init();
  tsc_adjust_check();
  lapicinit();
  ap_step(8);
  mp_startup_common();
}

// START THE APS

// warm start vector, 40:67 for a BIOS INIT; put back after
#define CMOS_SHUTDOWN 0x0f
#define SHUTDOWN_WARM 0x0a
#define WARM_VECTOR 0x467

static uint32_t saved_vector;
static uint saved_reason;

static int bios_booted(void) {
  const struct fnu_bootinfo *bi = bootinfo();

  return bi == 0 || bi->firmware != FNU_FIRMWARE_UEFI;
}

static void warm_start_set(void) {
  saved_reason = mc146818_read(CMOS_SHUTDOWN);
  if (bios_booted()) {
    saved_vector = *(volatile uint32_t *)P2V(WARM_VECTOR);
    *(volatile uint16_t *)P2V(WARM_VECTOR) = 0;
    *(volatile uint16_t *)P2V(WARM_VECTOR + 2) = (uint16_t)(ap_pa >> 4);
  }
  mc146818_write(CMOS_SHUTDOWN, SHUTDOWN_WARM);
}

static void warm_start_clear(void) {
  if (bios_booted())
    *(volatile uint32_t *)P2V(WARM_VECTOR) = saved_vector;
  mc146818_write(CMOS_SHUTDOWN, saved_reason);
}

// start page and low tables once; the code every time
static int ap_trampoline(void) {
  if (ap_pa == 0 && (ap_pa = lowmem_alloc(1)) == 0)
    return -1;
  if (lowmem_tables() == 0)
    return -1;
  memmove(P2V(ap_pa), _binary_entryother_start, (uintptr_t)_binary_entryother_size);
  *(volatile uint32_t *)P2V(ap_pa + AP_HDR_TABLES) = (uint32_t)lowmem_tables();
  return 0;
}

static const char *start_code_state(void) {
  uint n = (uint)(uintptr_t)_binary_entryother_size;

  return memcmp((const char *)P2V(ap_pa) + AP_HDR_END,
                _binary_entryother_start + AP_HDR_END, n - AP_HDR_END) == 0
             ? "intact"
             : "changed";
}

static int ap_start(struct cpu_info *c) {
  int64_t smi0, smi1;
  int ms, r;

  if (c->apstack == 0 && (c->apstack = contig_alloc(KSTACKSIZE)) == 0)
    return -1;
  if (c->ist == 0 && (c->ist = contig_alloc(NIST * ISTSTACKSIZE)) == 0)
    return -1;
  *ap_hdr64(AP_HDR_STACK) = (uint64_t)(c->apstack + KSTACKSIZE);
  *ap_hdr64(AP_HDR_ENTRY) = (uint64_t)mpentry;
  *ap_hdr64(AP_HDR_CR3) = V2P(kernel_pgdir);
  ap_step(0);
  smi0 = smi_count();
  if ((r = lapicstartap(c->apicid, (uint32_t)ap_pa)) != 0) {
    printf("SMP: CPU%d (apicid %d): the %s IPI was not delivered (ESR 0x%x)\n",
           (int)(c - cpus), c->apicid, r == 1 ? "INIT" : "STARTUP", lapic_esr());
    return -2;
  }
  for (ms = 0; ms < 5000; ms++) {
    if (c->started)
      return 0;
    delay(1000);
  }
  smi1 = smi_count();
  printf("SMP: CPU%d (apicid %d) did not start: step %d of 8, start code at 0x%lx "
         "%s, ESR 0x%x, %s, this cpu apic id %d\n",
         (int)(c - cpus), c->apicid, *ap_hdr16(AP_HDR_STEP), (unsigned long)ap_pa,
         start_code_state(), lapic_esr(), x2apic_mode ? "x2APIC" : "xAPIC", lapicid());
  if (smi0 >= 0)
    printf("SMP: %ld SMIs in those 5 s\n", (long)(smi1 - smi0));
  lapic_print_state();
  return -3;
}

// broadcast: the ones a lost STARTUP left waiting give their ids and halt
static int ap_probe(void) {
  int ms, n, i;

  if (ap_trampoline() < 0)
    return 0;
  *ap_hdr16(AP_HDR_PROBE) = 1;
  if (lapicstartap_all((uint32_t)ap_pa) != 0) {
    printf("SMP: the broadcast INIT and STARTUP went unsent (ESR 0x%x)\n", lapic_esr());
    return 0;
  }
  for (ms = 0; ms < 1000 && *ap_hdr16(AP_HDR_COUNT) < ncpus - 1; ms++)
    delay(1000);
  n = *ap_hdr16(AP_HDR_COUNT);
  printf("SMP: a broadcast INIT and STARTUP woke %d cpus", n);
  for (i = 0; i < n && i < AP_PROBE_MAX; i++)
    printf("%s%d", i == 0 ? ", apic ids " : " ", ((volatile uint8_t *)P2V(ap_pa + AP_HDR_IDS))[i]);
  printf("\n");
  *ap_hdr16(AP_HDR_PROBE) = 0;
  return n;
}

static int start_pass(void) {
  struct cpu_info *c;
  int r, up = 0;

  for (c = cpus; c < cpus + ncpus; c++) {
    if (c == cpus + cpuid() || c->started)
      continue;
    // one that does not come up is left out
    if ((r = ap_start(c)) == -1)
      panic("SMP: no memory for CPU%d's stacks", (int)(c - cpus));
    if (r == 0) {
      printf("SMP: CPU%d online\n", (int)(c - cpus));
      up++;
    } else
      printf("SMP: going on without CPU%d\n", (int)(c - cpus));
  }
  return up;
}

void startothers(void) {
  if (ncpus > 1 && ap_trampoline() < 0) {
    printf("SMP: no low memory for the start code, one cpu\n");
    ncpus = 1;
    return;
  }
  mtrr_save();
  hwlat_report("before the other cpus");
  warm_start_set();
  if (ncpus > 1 && start_pass() == 0 && ap_probe() > 0)
    start_pass();
  warm_start_clear();
}

// SLEEP
void cpu_restart_others(void) {
  struct cpu_info *c;

  if (ap_trampoline() < 0)
    return;
  mtrr_save();
  warm_start_set();
  for (c = cpus; c < cpus + ncpus; c++) {
    if (!c->parked)
      continue;
    c->park = 0;
    c->parked = 0;
    ap_start(c);
  }
  warm_start_clear();
}
