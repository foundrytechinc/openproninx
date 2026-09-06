#include "acpi.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "x86.h"

#define ACPI_MAX_TABLES 64

extern struct cpu cpus[NCPU];
extern int ncpu;
extern volatile uint32_t *lapic;
extern volatile struct ioapic *ioapic;
extern uint8_t ioapicid;

static struct {
  struct rsdp_descriptor *rsdp;
  int is_xsdt;
  struct acpi_sdt_header *tables[ACPI_MAX_TABLES];
  int table_count;
  struct acpi_fadt *fadt;
  struct acpi_madt *madt;
  uint16_t slp_typa;
  uint16_t slp_typb;
  int s5_found;
  int initialized;
} acpi_state;

static int acpi_checksum(const void *addr, int len) {
  const uint8_t *p = (const uint8_t *)addr;
  uint8_t sum = 0;
  for (int i = 0; i < len; i++)
    sum += p[i];
  return (sum == 0);
}

// Search EBDA and BIOS read-only memory space for RSDP signature
static struct rsdp_descriptor *acpi_find_rsdp(void) {
  // 1. Scan EBDA (Extended BIOS Data Area)
  uint16_t *bda = (uint16_t *)P2V(0x40E);
  uintptr_t ebda_phys = ((uintptr_t)(*bda)) << 4;
  if (ebda_phys != 0 && ebda_phys < 0x100000) {
    uint8_t *ebda = (uint8_t *)P2V(ebda_phys);
    for (int i = 0; i < 1024; i += 16) {
      if (memcmp(ebda + i, "RSD PTR ", 8) == 0) {
        if (acpi_checksum(ebda + i, 20))
          return (struct rsdp_descriptor *)(ebda + i);
      }
    }
  }

  // 2. Scan BIOS Read-Only memory space: 0xE0000 to 0xFFFFF
  uint8_t *bios = (uint8_t *)P2V(0xE0000);
  for (int i = 0; i < 0x20000; i += 16) {
    if (memcmp(bios + i, "RSD PTR ", 8) == 0) {
      if (acpi_checksum(bios + i, 20))
        return (struct rsdp_descriptor *)(bios + i);
    }
  }

  return 0;
}

// Search DSDT AML byte stream for \_S5_ package to extract SLP_TYPa and SLP_TYPb
static void acpi_parse_s5(void) {
  if (!acpi_state.fadt)
    return;

  uintptr_t dsdt_phys = 0;
  if (acpi_state.is_xsdt && acpi_state.fadt->x_dsdt) {
    dsdt_phys = (uintptr_t)acpi_state.fadt->x_dsdt;
  } else if (acpi_state.fadt->dsdt) {
    dsdt_phys = (uintptr_t)acpi_state.fadt->dsdt;
  }

  if (dsdt_phys == 0)
    return;

  struct acpi_sdt_header *dsdt_hdr =
      (struct acpi_sdt_header *)ioremap(dsdt_phys, sizeof(struct acpi_sdt_header));
  if (!dsdt_hdr || memcmp(dsdt_hdr->signature, "DSDT", 4) != 0)
    return;

  uint32_t len = dsdt_hdr->length;
  uint8_t *dsdt = (uint8_t *)ioremap(dsdt_phys, len);
  if (!dsdt || !acpi_checksum(dsdt, len))
    return;

  // Search AML stream for "_S5_"
  for (uint32_t i = 36; i < len - 8; i++) {
    if (memcmp(dsdt + i, "_S5_", 4) == 0) {
      uint32_t p = i + 4;
      if (dsdt[p] == 0x12) { // PackageOp
        p++;
        uint8_t lead = dsdt[p];
        uint8_t num_bytes = (lead >> 6) & 3;
        p += (num_bytes + 1); // Skip Package Length

        p++; // Skip NumElements

        // Parse first element (SLP_TYPa)
        if (dsdt[p] == 0x0A) { // BytePrefix
          acpi_state.slp_typa = dsdt[p + 1];
          p += 2;
        } else if (dsdt[p] == 0x00) { // ZeroOp
          acpi_state.slp_typa = 0;
          p += 1;
        } else if (dsdt[p] == 0x01) { // OneOp
          acpi_state.slp_typa = 1;
          p += 1;
        } else {
          acpi_state.slp_typa = dsdt[p];
          p += 1;
        }

        // Parse second element (SLP_TYPb)
        if (dsdt[p] == 0x0A) {
          acpi_state.slp_typb = dsdt[p + 1];
        } else if (dsdt[p] == 0x00) {
          acpi_state.slp_typb = 0;
        } else if (dsdt[p] == 0x01) {
          acpi_state.slp_typb = 1;
        } else {
          acpi_state.slp_typb = dsdt[p];
        }

        acpi_state.s5_found = 1;
        cprintf("ACPI: S5 sleep state found (SLP_TYPa=0x%x, SLP_TYPb=0x%x)\n",
                acpi_state.slp_typa, acpi_state.slp_typb);
        return;
      }
    }
  }
}

int acpi_init(void) {
  if (acpi_state.initialized)
    return 0;

  memset(&acpi_state, 0, sizeof(acpi_state));

  struct rsdp_descriptor *rsdp = acpi_find_rsdp();
  if (!rsdp) {
    cprintf("ACPI: RSDP not found, falling back to legacy BIOS\n");
    return -1;
  }

  acpi_state.rsdp = rsdp;
  char oem[7];
  memmove(oem, rsdp->oem_id, 6);
  oem[6] = '\0';

  // Check for ACPI 2.0+ XSDP
  if (rsdp->revision >= 2) {
    struct xsdp_descriptor *xsdp = (struct xsdp_descriptor *)rsdp;
    if (xsdp->length >= sizeof(struct xsdp_descriptor) &&
        acpi_checksum(xsdp, xsdp->length) &&
        xsdp->xsdt_address != 0) {
      acpi_state.is_xsdt = 1;
    }
  }

  cprintf("ACPI: RSDP v%d found at 0x%p (OEM '%s', mode: %s)\n",
          rsdp->revision ? (rsdp->revision + 1) : 1, rsdp, oem,
          acpi_state.is_xsdt ? "XSDT 64-bit" : "RSDT 32-bit");

  if (acpi_state.is_xsdt) {
    struct xsdp_descriptor *xsdp = (struct xsdp_descriptor *)rsdp;
    struct acpi_sdt_header *xsdt_hdr =
        (struct acpi_sdt_header *)ioremap(xsdp->xsdt_address, sizeof(struct acpi_sdt_header));
    if (!xsdt_hdr || memcmp(xsdt_hdr->signature, "XSDT", 4) != 0) {
      cprintf("ACPI: invalid XSDT table\n");
      return -1;
    }

    uint32_t xsdt_len = xsdt_hdr->length;
    struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)ioremap(xsdp->xsdt_address, xsdt_len);
    if (!xsdt || !acpi_checksum(xsdt, xsdt_len)) {
      cprintf("ACPI: XSDT checksum failed\n");
      return -1;
    }

    int entries = (xsdt_len - sizeof(struct acpi_sdt_header)) / 8;
    uint64_t *table_ptrs = (uint64_t *)(xsdt + 1);

    for (int i = 0; i < entries && acpi_state.table_count < ACPI_MAX_TABLES; i++) {
      uint64_t table_phys = table_ptrs[i];
      if (table_phys == 0)
        continue;

      struct acpi_sdt_header *hdr =
          (struct acpi_sdt_header *)ioremap(table_phys, sizeof(struct acpi_sdt_header));
      if (!hdr)
        continue;

      uint32_t tbl_len = hdr->length;
      struct acpi_sdt_header *full_tbl =
          (struct acpi_sdt_header *)ioremap(table_phys, tbl_len);
      if (!full_tbl || !acpi_checksum(full_tbl, tbl_len))
        continue;

      acpi_state.tables[acpi_state.table_count++] = full_tbl;

      if (memcmp(full_tbl->signature, "FACP", 4) == 0)
        acpi_state.fadt = (struct acpi_fadt *)full_tbl;
      else if (memcmp(full_tbl->signature, "APIC", 4) == 0)
        acpi_state.madt = (struct acpi_madt *)full_tbl;
    }
  } else {
    uint32_t rsdt_phys = rsdp->rsdt_address;
    struct acpi_sdt_header *rsdt_hdr =
        (struct acpi_sdt_header *)ioremap(rsdt_phys, sizeof(struct acpi_sdt_header));
    if (!rsdt_hdr || memcmp(rsdt_hdr->signature, "RSDT", 4) != 0) {
      cprintf("ACPI: invalid RSDT table\n");
      return -1;
    }

    uint32_t rsdt_len = rsdt_hdr->length;
    struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)ioremap(rsdt_phys, rsdt_len);
    if (!rsdt || !acpi_checksum(rsdt, rsdt_len)) {
      cprintf("ACPI: RSDT checksum failed\n");
      return -1;
    }

    int entries = (rsdt_len - sizeof(struct acpi_sdt_header)) / 4;
    uint32_t *table_ptrs = (uint32_t *)(rsdt + 1);

    for (int i = 0; i < entries && acpi_state.table_count < ACPI_MAX_TABLES; i++) {
      uint32_t table_phys = table_ptrs[i];
      if (table_phys == 0)
        continue;

      struct acpi_sdt_header *hdr =
          (struct acpi_sdt_header *)ioremap(table_phys, sizeof(struct acpi_sdt_header));
      if (!hdr)
        continue;

      uint32_t tbl_len = hdr->length;
      struct acpi_sdt_header *full_tbl =
          (struct acpi_sdt_header *)ioremap(table_phys, tbl_len);
      if (!full_tbl || !acpi_checksum(full_tbl, tbl_len))
        continue;

      acpi_state.tables[acpi_state.table_count++] = full_tbl;

      if (memcmp(full_tbl->signature, "FACP", 4) == 0)
        acpi_state.fadt = (struct acpi_fadt *)full_tbl;
      else if (memcmp(full_tbl->signature, "APIC", 4) == 0)
        acpi_state.madt = (struct acpi_madt *)full_tbl;
    }
  }

  cprintf("ACPI: parsed %d description tables (MADT: %s, FADT: %s)\n",
          acpi_state.table_count,
          acpi_state.madt ? "present" : "missing",
          acpi_state.fadt ? "present" : "missing");

  acpi_state.initialized = 1;

  if (acpi_state.fadt)
    acpi_parse_s5();

  return 0;
}

void *acpi_find_table(const char *signature) {
  if (!acpi_state.initialized || !signature)
    return 0;

  for (int i = 0; i < acpi_state.table_count; i++) {
    if (memcmp(acpi_state.tables[i]->signature, signature, 4) == 0)
      return acpi_state.tables[i];
  }
  return 0;
}

// Discover CPUs, Local APIC and IOAPIC from ACPI MADT (APIC table)
int acpi_mp_init(void) {
  if (!acpi_state.initialized || !acpi_state.madt)
    return -1;

  struct acpi_madt *madt = acpi_state.madt;
  uintptr_t lapic_phys = (uintptr_t)madt->lapic_address;
  uint8_t *p = (uint8_t *)(madt + 1);
  uint8_t *end = (uint8_t *)madt + madt->header.length;

  ncpu = 0;
  ioapic = 0;

  while (p < end) {
    struct acpi_subtable_header *sub = (struct acpi_subtable_header *)p;
    if (sub->length < 2 || p + sub->length > end)
      break;

    switch (sub->type) {
    case ACPI_MADT_TYPE_LAPIC: {
      struct madt_lapic *l = (struct madt_lapic *)p;
      // Flags bit 0 = Enabled, bit 1 = Online Capable
      if ((l->flags & 1) || (l->flags & 2)) {
        if (ncpu < NCPU) {
          cpus[ncpu].apicid = l->apic_id;
          ncpu++;
        }
      }
      break;
    }
    case ACPI_MADT_TYPE_IOAPIC: {
      struct madt_ioapic *io = (struct madt_ioapic *)p;
      if (ioapic == 0) {
        ioapicid = io->ioapic_id;
        ioapic = (struct ioapic *)DEVSPACE_P2V((uintptr_t)io->ioapic_address);
      }
      break;
    }
    case ACPI_MADT_TYPE_LAPIC_OVERRIDE: {
      struct madt_lapic_override *lo = (struct madt_lapic_override *)p;
      lapic_phys = (uintptr_t)lo->lapic_address;
      break;
    }
    }
    p += sub->length;
  }

  if (ncpu == 0 || ioapic == 0 || lapic_phys == 0)
    return -1;

  lapic = (uint32_t *)DEVSPACE_P2V(lapic_phys);

  cprintf("ACPI: MADT configured %d CPUs, Local APIC at 0x%p, IOAPIC at 0x%p\n",
          ncpu, lapic, ioapic);
  return 0;
}

// Clean ACPI S5 Soft-off / Shutdown
void acpi_poweroff(void) {
  cprintf("ACPI: initiating system shutdown...\n");

  cli();

  if (acpi_state.fadt) {
    struct acpi_fadt *fadt = acpi_state.fadt;

    // Enable ACPI mode via SMI command if disabled
    if (fadt->smi_cmd && fadt->acpi_enable) {
      outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
      for (int i = 0; i < 300; i++) {
        if (fadt->pm1a_cnt_blk && (inw((uint16_t)fadt->pm1a_cnt_blk) & 1))
          break;
        microdelay(10000);
      }
    }

    uint16_t slp_typa = acpi_state.s5_found ? acpi_state.slp_typa : 5;
    uint16_t slp_typb = acpi_state.s5_found ? acpi_state.slp_typb : 5;

    // Write SLP_EN (bit 13) | (SLP_TYP << 10)
    if (fadt->pm1a_cnt_blk)
      outw((uint16_t)fadt->pm1a_cnt_blk, (slp_typa << 10) | (1 << 13));
    if (fadt->pm1b_cnt_blk)
      outw((uint16_t)fadt->pm1b_cnt_blk, (slp_typb << 10) | (1 << 13));
  }

  // Hypervisor poweroff fallbacks
  outw(0x604, 0x2000);  // QEMU modern / Bochs ACPI
  outw(0xB004, 0x2000); // Older Bochs / QEMU
  outw(0x4004, 0x3400); // VirtualBox

  microdelay(100000);

  cprintf("ACPI: poweroff complete. System halted.\n");
  for (;;) {
    hlt();
  }
}

// Clean ACPI reset with hardware fallback to 8042 and triple fault
void acpi_reboot(void) {
  cli();

  // 1. Try ACPI Reset Register from FADT
  if (acpi_state.fadt && (acpi_state.fadt->flags & ACPI_FADT_RESET_REG_SUP)) {
    struct acpi_gas *rr = &acpi_state.fadt->reset_reg;
    if (rr->address != 0) {
      if (rr->address_space_id == 1) {
        // System I/O
        outb((uint16_t)rr->address, acpi_state.fadt->reset_value);
      } else if (rr->address_space_id == 0) {
        // System Memory
        *(volatile uint8_t *)DEVSPACE_P2V((uintptr_t)rr->address) = acpi_state.fadt->reset_value;
      }
      microdelay(50000);
    }
  }

  // 2. Fallback to PS/2 Keyboard Controller 8042 pulse
  uint8_t good = 0x02;
  for (int i = 0; i < 10000; i++) {
    good = inb(0x64);
    if ((good & 0x02) == 0)
      break;
    microdelay(10);
  }
  outb(0x64, 0xFE);
  microdelay(50000);

  // 3. Fallback: Triple Fault (empty IDT + software interrupt)
  struct {
    uint16_t limit;
    uint64_t base;
  } __attribute__((packed)) null_idt = {0, 0};
  asm volatile("lidt (%0); int $3" : : "r"(&null_idt));

  for (;;) {
    hlt();
  }
}

void acpi_print_summary(void) {
  if (!acpi_state.initialized) {
    cprintf("ACPI: disabled (no RSDP found)\n");
    return;
  }
  char oem[7];
  memmove(oem, acpi_state.rsdp->oem_id, 6);
  oem[6] = '\0';
  cprintf("ACPI: RSDP v%d (OEM '%s', %s), %d tables (MADT: %s, FADT: %s, S5: %s)\n",
          acpi_state.rsdp->revision ? (acpi_state.rsdp->revision + 1) : 1,
          oem,
          acpi_state.is_xsdt ? "XSDT" : "RSDT",
          acpi_state.table_count,
          acpi_state.madt ? "yes" : "no",
          acpi_state.fadt ? "yes" : "no",
          acpi_state.s5_found ? "yes" : "no");
}
