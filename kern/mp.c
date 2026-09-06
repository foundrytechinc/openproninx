// Multiprocessor support
// Search memory for MP description structures.
// http://developer.intel.com/design/pentium/datashts/24201606.pdf

#include "mp.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"

struct cpu cpus[NCPU];
int ncpu;

static uint8_t sum(uint8_t *addr, int len) {
  int i, sum;

  sum = 0;
  for (i = 0; i < len; i++)
    sum += addr[i];
  return sum;
}

// Look for an MP structure in the len bytes at addr.
static struct mp *mpsearch1(uintptr_t a, int len) {
  uint8_t *e, *p, *addr;

  addr = (uint8_t *)P2V(a);
  e = addr + len;
  for (p = addr; p < e; p += sizeof(struct mp))
    if (memcmp(p, "_MP_", 4) == 0 && sum(p, sizeof(struct mp)) == 0)
      return (struct mp *)p;
  return 0;
}

// Search for the MP Floating Pointer Structure, which according to the
// spec is in one of the following three locations:
// 1) in the first KB of the EBDA;
// 2) in the last KB of system base memory;
// 3) in the BIOS ROM between 0xE0000 and 0xFFFFF.
static struct mp *mpsearch(void) {
  uchar *bda;
  uint p;
  struct mp *mp;

  bda = (uchar *)P2V(0x400);
  if ((p = ((bda[0x0F] << 8) | bda[0x0E]) << 4)) {
    if ((mp = mpsearch1(p, 1024)))
      return mp;
  } else {
    p = ((bda[0x14] << 8) | bda[0x13]) * 1024;
    if ((mp = mpsearch1(p - 1024, 1024)))
      return mp;
  }
  return mpsearch1(0xF0000, 0x10000);
}

// Search for an MP configuration table.
// Do not accept default configurations (physaddr == 0).
// Check for correct signature, calculate the checksum and,
// if correct, check the version.
// ref. MPspec Chapter 4
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

void mpinit(void) {
  // First try modern ACPI MADT multiprocessor initialization
  if (acpi_mp_init() == 0) {
    return;
  }

  // Fallback to Intel MultiProcessor Specification (MP Spec 1.4)
  uchar *p, *e;
  int ismp;
  struct mp *mp;
  struct mpconf *mp_conf;
  struct mpproc *mp_proc;
  struct mpioapic *mp_ioapic;

  if ((mp_conf = mpconfig(&mp)) == 0) {
    panic("Expect to run on an SMP");
  }
  saved_mp_conf = mp_conf;
  ismp = 1;
  lapic = (uint32_t *)DEVSPACE_P2V((uintptr_t)mp_conf->lapicaddr);

  for (p = (uchar *)(mp_conf + 1), e = (uchar *)mp_conf + mp_conf->length;
       p < e;) {
    switch (*p) {
    case MPPROC:
      mp_proc = (struct mpproc *)p;
      if (ncpu < NCPU) {
        cpus[ncpu].apicid = mp_proc->apicid; // apicid may differ from ncpu
        ncpu++;
      }
      p += sizeof(struct mpproc);
      continue;
    case MPIOAPIC:
      mp_ioapic = (struct mpioapic *)p;
      ioapicid = mp_ioapic->apicno;
      ioapic = (struct ioapic *)DEVSPACE_P2V((uintptr_t)mp_ioapic->addr);
      p += sizeof(struct mpioapic);
      continue;
    case MPBUS:
    case MPIOINTR:
    case MPLINTR:
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
    outb(0x22, 0x70);          // Select IMCR
    outb(0x23, inb(0x23) | 1); // Mask external interrupts
  }
}

extern char _binary_obj_kern_entryother_start[];
extern char _binary_obj_kern_entryother_size[];

static void mpenter(void) {
  switchkvm();
  seginit();
  lapicinit();
  mpmain();
}

/*
 * Statistical fact:
 * >100% of multithreaded processors are multithreaded.
 */
// Start the non-boot (AP) processors.
void startothers(void) {
  uint8_t *code;
  struct cpu *c;
  char *stack;
  uint64_t *p4, *p3;

  // Write entry code to physical 0x7000.
  code = P2V(0x7000);
  memmove(code, _binary_obj_kern_entryother_start,
          (uintptr_t)_binary_obj_kern_entryother_size);

  // Set up AP boot page table at 0x8000 (PML4) and 0x9000 (PDPT)
  p4 = (uint64_t *)P2V(0x8000);
  p3 = (uint64_t *)P2V(0x9000);

  memset(p4, 0, PGSIZE);
  memset(p3, 0, PGSIZE);

  // Entry 0 in PML4 -> maps 0..512GB to PDPT at physical 0x9000
  p4[0] = 0x9000 | PTE_P | PTE_W;

  // Entry 511 in PML4 -> maps higher-half to PDPT at physical 0x9000
  p4[511] = 0x9000 | PTE_P | PTE_W;

  // Entry 0 in PDPT -> maps 0..1GB with 1GB huge page to physical 0x0
  p3[0] = 0x0 | PTE_P | PTE_W | PTE_PS;

  // Entry 510 in PDPT -> maps 0xffffffff80000000 (KERNBASE) with 1GB huge page to physical 0x0
  p3[510] = 0x0 | PTE_P | PTE_W | PTE_PS;

  cprintf("SMP: starting %d other CPUs...\n", ncpu - 1);
  for (c = cpus; c < cpus + ncpu; c++) {
    if (c == cpus + cpuid())
      continue;

    // Allocate per-core kernel stack
    stack = kalloc();
    if (!stack)
      panic("startothers: kalloc failed");

    *(uint64_t *)(code - 8) = (uint64_t)(stack + KSTACKSIZE);
    *(uint64_t *)(code - 16) = (uint64_t)mpenter;

    cprintf("SMP: booting CPU%d (apicid %d)...\n", (int)(c - cpus), c->apicid);
    lapicstartap(c->apicid, 0x7000);

    // Wait for CPU to finish mpmain() and set started flag
    while (c->started == 0)
      ;
    cprintf("SMP: CPU%d online\n", (int)(c - cpus));
  }
}

