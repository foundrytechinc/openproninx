// ACPI TABLES

#include "acpi.h"
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "inc/stat.h"
#include "uio.h"
#include "inc/bootinfo.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "x86.h"

struct tabent {
  struct acpi_sdt_header *hdr;
  uint8_t *aml;
};

#define PER_CHUNK ((PGSIZE - sizeof(void *)) / sizeof(struct tabent))

struct tabchunk {
  struct tabchunk *next;
  struct tabent e[PER_CHUNK];
};

extern struct cpu_info cpus[MAXCPU];
extern int ncpus;
extern volatile uint32_t *lapic;
extern volatile struct ioapic *ioapic;
extern uint8_t ioapicid;

static struct {
  struct rsdp_descriptor *rsdp;
  int is_xsdt;
  struct tabchunk *chunks;
  int count;
  struct acpi_fadt *fadt;
  struct acpi_madt *madt;
  struct acpi_facs *facs;
  struct acpi_sdt_header *dsdt;
  int initialized;
} state;

static int sum_ok(const void *addr, uint len) {
  const uint8_t *p = addr;
  uint8_t sum = 0;
  uint i;

  for (i = 0; i < len; i++)
    sum += p[i];
  return sum == 0;
}

static struct rsdp_descriptor *find_rsdp(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint16_t *bda = (uint16_t *)P2V(0x40E);
  uintptr_t ebda_phys = ((uintptr_t)(*bda)) << 4;
  uint8_t *p;
  int i;

  if (bi && bi->acpi_rsdp != 0 && bi->acpi_rsdp < 0x100000000ULL) {
    struct rsdp_descriptor *r =
        (struct rsdp_descriptor *)P2V((uintptr_t)bi->acpi_rsdp);

    if (memcmp(r->signature, "RSD PTR ", 8) == 0 && sum_ok(r, 20))
      return r;
  }
  if (ebda_phys != 0 && ebda_phys < 0x100000) {
    p = (uint8_t *)P2V(ebda_phys);
    for (i = 0; i < 1024; i += 16)
      if (memcmp(p + i, "RSD PTR ", 8) == 0 && sum_ok(p + i, 20))
        return (struct rsdp_descriptor *)(p + i);
  }
  p = (uint8_t *)P2V(0xE0000);
  for (i = 0; i < 0x20000; i += 16)
    if (memcmp(p + i, "RSD PTR ", 8) == 0 && sum_ok(p + i, 20))
      return (struct rsdp_descriptor *)(p + i);
  return 0;
}

static struct acpi_sdt_header *map_table_sum(uint64_t pa, int strict) {
  struct acpi_sdt_header *hdr;
  uint len;

  if (pa == 0)
    return 0;
  if ((hdr = ioremap(pa, sizeof(*hdr))) == 0)
    return 0;
  len = hdr->length;
  if (len < sizeof(*hdr))
    return 0;
  if ((hdr = ioremap(pa, len)) == 0)
    return 0;
  if (!sum_ok(hdr, len)) {
    printf("ACPI: %c%c%c%c at 0x%lx has a wrong checksum\n", hdr->signature[0],
            hdr->signature[1], hdr->signature[2], hdr->signature[3], pa);
    if (strict)
      return 0;
  }
  return hdr;
}

static struct acpi_sdt_header *map_table(uint64_t pa) {
  return map_table_sum(pa, 0);
}

static struct tabent *tabent(int i) {
  struct tabchunk *c = state.chunks;

  for (; c != 0 && i >= (int)PER_CHUNK; i -= (int)PER_CHUNK)
    c = c->next;
  return c != 0 ? &c->e[i] : 0;
}

static void add_table(struct acpi_sdt_header *tbl) {
  struct tabchunk **pp;

  if (state.count % (int)PER_CHUNK == 0) {
    for (pp = &state.chunks; *pp != 0; pp = &(*pp)->next)
      ;
    if ((*pp = (struct tabchunk *)page_alloc()) == 0)
      panic("acpi: no page for the table list");
    memset(*pp, 0, PGSIZE);
  }
  tabent(state.count++)->hdr = tbl;
  if (memcmp(tbl->signature, "FACP", 4) == 0)
    state.fadt = (struct acpi_fadt *)tbl;
  else if (memcmp(tbl->signature, "APIC", 4) == 0)
    state.madt = (struct acpi_madt *)tbl;
}

int acpi_init(void) {
  struct rsdp_descriptor *rsdp;
  struct acpi_sdt_header *root;
  uint64_t rootpa;
  uint esize, n, i;
  uint8_t *entries;

  if (state.initialized)
    return 0;
  memset(&state, 0, sizeof(state));
  if ((rsdp = find_rsdp()) == 0) {
    printf("ACPI: RSDP not found, falling back to legacy BIOS\n");
    return -1;
  }
  state.rsdp = rsdp;

  rootpa = rsdp->rsdt_address;
  esize = 4;
  if (rsdp->revision >= 2) {
    struct xsdp_descriptor *x = (struct xsdp_descriptor *)rsdp;

    if (x->length >= sizeof(*x) && sum_ok(x, x->length) &&
        x->xsdt_address != 0) {
      rootpa = x->xsdt_address;
      esize = 8;
      state.is_xsdt = 1;
    }
  }

  root = map_table_sum(rootpa, 1);
  if (root == 0 || memcmp(root->signature, state.is_xsdt ? "XSDT" : "RSDT", 4)) {
    printf("ACPI: invalid %s table\n", state.is_xsdt ? "XSDT" : "RSDT");
    return -1;
  }
  n = (root->length - (uint)sizeof(*root)) / esize;
  entries = (uint8_t *)(root + 1);
  for (i = 0; i < n; i++) {
    uint64_t pa = 0;
    struct acpi_sdt_header *tbl;

    memcpy(&pa, entries + i * esize, esize);
    if ((tbl = map_table(pa)) != 0)
      add_table(tbl);
  }

  if (state.fadt != 0) {
    uint64_t dsdt = state.fadt->dsdt, facs = state.fadt->firmware_ctrl;

    if (state.fadt->header.revision >= 3 &&
        state.fadt->header.length >= __offsetof(struct acpi_fadt, x_dsdt) + 8) {
      if (state.fadt->x_dsdt != 0)
        dsdt = state.fadt->x_dsdt;
      if (state.fadt->x_firmware_ctrl != 0)
        facs = state.fadt->x_firmware_ctrl;
    }
    if ((state.dsdt = map_table(dsdt)) != 0 &&
        memcmp(state.dsdt->signature, "DSDT", 4) != 0)
      state.dsdt = 0;
    if (facs != 0)
      state.facs = ioremap(facs, sizeof(struct acpi_facs));
  }

  state.initialized = 1;
  return 0;
}

void *acpi_find_table(const char *signature) {
  int i;

  if (!state.initialized || !signature)
    return 0;
  for (i = 0; i < state.count; i++)
    if (memcmp(tabent(i)->hdr->signature, signature, 4) == 0)
      return tabent(i)->hdr;
  return 0;
}

int acpi_table_count(void) { return state.count; }

struct acpi_sdt_header *acpi_table(int i) {
  return i >= 0 && i < state.count ? tabent(i)->hdr : 0;
}

struct acpi_fadt *acpi_fadt(void) { return state.fadt; }

struct acpi_facs *acpi_facs(void) { return state.facs; }

struct acpi_sdt_header *acpi_dsdt(void) { return state.dsdt; }

uint8_t *acpi_table_aml(int i) {
  struct acpi_sdt_header *hdr = acpi_table(i);
  uint len;

  if (hdr == 0)
    return 0;
  if (tabent(i)->aml == 0) {
    len = hdr->length - (uint)sizeof(*hdr);
    tabent(i)->aml = malloc(len + ACPI_AML_PAD, M_ACPI, M_WAITOK | M_ZERO);
    memcpy(tabent(i)->aml, (uint8_t *)(hdr + 1), len);
  }
  return tabent(i)->aml;
}

uint8_t *acpi_table_copy(uint64_t pa, uint *len) {
  struct acpi_sdt_header *hdr = map_table(pa);
  uint8_t *copy;

  if (hdr == 0 || memcmp(hdr->signature, "SSDT", 4) != 0)
    return 0;
  *len = hdr->length - (uint)sizeof(*hdr);
  copy = malloc(*len + ACPI_AML_PAD, M_ACPI, M_WAITOK | M_ZERO);
  memcpy(copy, (uint8_t *)(hdr + 1), *len);
  return copy;
}

// /dev/acpitbls: raw tables, root table first, then DSDT and FACS
static const uint8_t *piece(int i, uint *len) {
  const struct acpi_sdt_header *h = 0;
  uintptr_t room;

  if (i < state.count)
    h = tabent(i)->hdr;
  else if (i == state.count)
    h = state.dsdt;
  else if (i == state.count + 1 && state.facs != 0) {
    room = PGSIZE - ((uintptr_t)state.facs & (PGSIZE - 1));
    *len = state.facs->length < room ? state.facs->length : (uint)room;
    return (const uint8_t *)state.facs;
  } else
    return 0;
  if (h == 0) {
    *len = 0;
    return (const uint8_t *)"";
  }
  *len = h->length;
  return (const uint8_t *)h;
}

static int tblread(struct inode *ip, struct uio *uio) {
  uint64_t at = 0, off = uio->uio_offset, skip;
  size_t want = uio->uio_resid, n;
  const uint8_t *p;
  uint len;
  int i;

  (void)ip;
  if (!state.initialized)
    return 0;
  for (i = 0; uio->uio_resid > 0 && (p = piece(i, &len)) != 0; i++, at += len) {
    if (off >= at + len)
      continue;
    skip = off - at;
    n = len - skip < uio->uio_resid ? len - skip : uio->uio_resid;
    if (uiomove((void *)(p + skip), n, uio) != 0)
      return -FNU_ERR_FAULT;
    off += n;
  }
  return (int)(want - uio->uio_resid);
}

static int tblwrite(struct inode *ip, struct uio *uio) {
  (void)ip;
  (void)uio;
  return -FNU_ERR_NOTPERMITTED;
}

void acpi_tables_attach(void) {
  if (!state.initialized)
    return;
  devsw[ACPITBLS].class = FNU_DEVCLASS_MEMORY;
  devsw[ACPITBLS].read = tblread;
  devsw[ACPITBLS].write = tblwrite;
  devfs_add("acpitbls", FNU_TYPE_DEVICE, ACPITBLS, 0, 0400);
}

// MCFG
struct mcfg_entry {
  uint64_t base;
  uint16_t seg;
  uint8_t start, end;
  uint32_t reserved;
} __packed;

uint64_t acpi_mcfg_addr(int seg, int bus, int dev, int fn, int reg) {
  struct acpi_sdt_header *t = acpi_find_table("MCFG");
  struct mcfg_entry *e;
  uint n, i;

  if (t == 0 || t->length < sizeof(*t) + 8)
    return 0;
  n = (t->length - (uint)sizeof(*t) - 8) / (uint)sizeof(*e);
  e = (struct mcfg_entry *)((uint8_t *)(t + 1) + 8);
  for (i = 0; i < n; i++, e++)
    if (e->seg == seg && bus >= e->start && bus <= e->end)
      return e->base + ((uint64_t)(bus - e->start) << 20) +
             ((uint64_t)dev << 15) + ((uint64_t)fn << 12) + (uint64_t)reg;
  return 0;
}

int acpi_century_index(void) {
  if (!state.fadt || state.fadt->header.length < 109)
    return 0;
  return state.fadt->century;
}

uint64_t acpi_rsdp_pa(void) {
  return state.initialized ? (uint64_t)V2P(state.rsdp) : 0;
}

// MADT

static void madt_cpu(uint32_t id, int uid, int *past) {
  int i;

  for (i = 0; i < ncpus; i++)
    if (cpus[i].apicid == id)
      return;
  if (ncpus >= MAXCPU) {
    (*past)++;
    return;
  }
  cpus[ncpus].apicid = id;
  cpus[ncpus].ci_acpi_proc_id = uid;
  ncpus++;
}

int acpi_mp_init(void) {
  struct acpi_madt *madt = state.madt;
  uintptr_t lapic_phys;
  uint8_t *p, *end;
  int past = 0, nio = 0;

  if (!state.initialized || !madt)
    return -1;
  lapic_phys = (uintptr_t)madt->lapic_address;
  p = (uint8_t *)(madt + 1);
  end = (uint8_t *)madt + madt->header.length;
  ncpus = 0;

  while (p < end) {
    struct acpi_subtable_header *sub = (struct acpi_subtable_header *)p;

    if (sub->length < 2 || p + sub->length > end)
      break;
    switch (sub->type) {
    case ACPI_MADT_TYPE_LAPIC: {
      struct madt_lapic *l = (struct madt_lapic *)p;

      if (l->flags & 1)
        madt_cpu(l->apic_id, l->processor_id, &past);
      break;
    }
    case ACPI_MADT_TYPE_X2APIC: {
      struct madt_x2apic *x = (struct madt_x2apic *)p;

      if (x->flags & 1)
        madt_cpu(x->x2apic_id, (int)x->uid, &past);
      break;
    }
    case ACPI_MADT_TYPE_LAPIC_NMI: {
      struct madt_lapic_nmi *n = (struct madt_lapic_nmi *)p;

      lapic_lint_nmi(LINT_BY_UID, n->processor_id == 0xff ? -1 : n->processor_id,
                     n->lint, n->flags);
      break;
    }
    case ACPI_MADT_TYPE_X2APIC_NMI: {
      struct madt_x2apic_nmi *n = (struct madt_x2apic_nmi *)p;

      lapic_lint_nmi(LINT_BY_UID, n->uid == 0xffffffffu ? -1 : (int)n->uid, n->lint,
                     n->flags);
      break;
    }
    case ACPI_MADT_TYPE_IOAPIC:
      nio++;
      break;
    case ACPI_MADT_TYPE_LAPIC_OVERRIDE: {
      struct madt_lapic_override *lo = (struct madt_lapic_override *)p;

      lapic_phys = (uintptr_t)lo->lapic_address;
      break;
    }
    }
    p += sub->length;
  }
  if (ncpus == 0 || nio == 0 || lapic_phys == 0)
    return -1;
  lapic = (uint32_t *)ioremap(lapic_phys, 4096);
  if (past > 0)
    printf("ACPI: %d more CPUs than MAXCPU (%d) are left idle\n", past, MAXCPU);
  return 0;
}

int acpi_ioapic_each(void (*fn)(int id, uint64_t addr, int gsibase)) {
  struct acpi_madt *madt = state.madt;
  uint8_t *p, *end;
  int n = 0;

  if (!state.initialized || madt == 0)
    return -1;
  p = (uint8_t *)(madt + 1);
  end = (uint8_t *)madt + madt->header.length;
  while (p + 2 <= end) {
    struct acpi_subtable_header *sub = (struct acpi_subtable_header *)p;

    if (sub->length < 2 || p + sub->length > end)
      break;
    if (sub->type == ACPI_MADT_TYPE_IOAPIC &&
        sub->length >= sizeof(struct madt_ioapic)) {
      struct madt_ioapic *io = (struct madt_ioapic *)p;

      fn(io->ioapic_id, io->ioapic_address, (int)io->gsi_base);
      n++;
    }
    p += sub->length;
  }
  return n;
}

int acpi_isa_irq(int irq, int sci, int *gsi, int *level, int *low) {
  struct acpi_madt *madt = state.madt;
  uint8_t *p, *end;

  *gsi = irq;
  *level = sci;
  *low = sci;
  if (madt == 0)
    return 0;
  p = (uint8_t *)(madt + 1);
  end = (uint8_t *)madt + madt->header.length;
  while (p + 2 <= end) {
    struct acpi_subtable_header *sub = (struct acpi_subtable_header *)p;

    if (sub->length < 2 || p + sub->length > end)
      break;
    if (sub->type == ACPI_MADT_TYPE_INT_OVERRIDE &&
        sub->length >= sizeof(struct madt_int_override)) {
      struct madt_int_override *o = (struct madt_int_override *)p;

      if (o->source_irq == irq) {
        // 0 says the line's own
        *gsi = (int)o->gsi;
        if (((o->flags >> 2) & 3) != 0)
          *level = ((o->flags >> 2) & 3) == 3;
        if ((o->flags & 3) != 0)
          *low = (o->flags & 3) == 3;
        return 1;
      }
    }
    p += sub->length;
  }
  return 0;
}

void acpi_print_summary(void) {
  if (!state.initialized)
    printf("ACPI: disabled (no RSDP found)\n");
}
