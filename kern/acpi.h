#ifndef PRONINX_ACPI_H
#define PRONINX_ACPI_H

#include "inc/types.h"

// ACPI 1.0 Root System Description Pointer
struct rsdp_descriptor {
  char signature[8];       // "RSD PTR "
  uint8_t checksum;        // Sum of all 20 bytes must equal 0
  char oem_id[6];
  uint8_t revision;        // 0 for ACPI 1.0, 2 for ACPI 2.0+
  uint32_t rsdt_address;   // Physical address of 32-bit RSDT
} __attribute__((packed));

// ACPI 2.0+ Extended System Description Pointer
struct xsdp_descriptor {
  struct rsdp_descriptor rsdp;
  uint32_t length;
  uint64_t xsdt_address;   // Physical address of 64-bit XSDT
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __attribute__((packed));

// Generic System Description Table Header
struct acpi_sdt_header {
  char signature[4];       // 4-character ASCII table signature
  uint32_t length;         // Total table length including header
  uint8_t revision;
  uint8_t checksum;        // Sum of all bytes in table must equal 0
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

// Generic Address Structure (GAS)
struct acpi_gas {
  uint8_t address_space_id; // 0 = System Memory, 1 = System I/O, 2 = PCI Config
  uint8_t register_bit_width;
  uint8_t register_bit_offset;
  uint8_t access_size;
  uint64_t address;
} __attribute__((packed));

// MADT: Multiple APIC Description Table (Signature "APIC")
struct acpi_madt {
  struct acpi_sdt_header header;
  uint32_t lapic_address;  // Physical address of Local APIC
  uint32_t flags;          // 1 = Dual 8259 legacy PICs installed
} __attribute__((packed));

#define ACPI_MADT_TYPE_LAPIC           0
#define ACPI_MADT_TYPE_IOAPIC          1
#define ACPI_MADT_TYPE_INT_OVERRIDE    2
#define ACPI_MADT_TYPE_NMI_SOURCE      3
#define ACPI_MADT_TYPE_LAPIC_NMI       4
#define ACPI_MADT_TYPE_LAPIC_OVERRIDE  5

struct acpi_subtable_header {
  uint8_t type;
  uint8_t length;
} __attribute__((packed));

struct madt_lapic {
  struct acpi_subtable_header header;
  uint8_t processor_id;
  uint8_t apic_id;
  uint32_t flags;          // Bit 0 = Enabled, Bit 1 = Online Capable
} __attribute__((packed));

struct madt_ioapic {
  struct acpi_subtable_header header;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_address; // Physical address of IOAPIC
  uint32_t gsi_base;
} __attribute__((packed));

struct madt_int_override {
  struct acpi_subtable_header header;
  uint8_t bus;             // 0 = ISA
  uint8_t source_irq;      // Bus-relative interrupt source
  uint32_t gsi;            // Global System Interrupt
  uint16_t flags;          // Polarity and trigger mode
} __attribute__((packed));

struct madt_lapic_override {
  struct acpi_subtable_header header;
  uint16_t reserved;
  uint64_t lapic_address;  // 64-bit Local APIC address
} __attribute__((packed));

// FADT: Fixed ACPI Description Table (Signature "FACP")
struct acpi_fadt {
  struct acpi_sdt_header header;
  uint32_t firmware_ctrl;
  uint32_t dsdt;
  uint8_t  reserved;
  uint8_t  preferred_pm_profile;
  uint16_t sci_int;
  uint32_t smi_cmd;
  uint8_t  acpi_enable;
  uint8_t  acpi_disable;
  uint8_t  s4bios_req;
  uint8_t  pstate_cnt;
  uint32_t pm1a_evt_blk;
  uint32_t pm1b_evt_blk;
  uint32_t pm1a_cnt_blk;
  uint32_t pm1b_cnt_blk;
  uint32_t pm2_cnt_blk;
  uint32_t pm_tmr_blk;
  uint32_t gpe0_blk;
  uint32_t gpe1_blk;
  uint8_t  pm1_evt_len;
  uint8_t  pm1_cnt_len;
  uint8_t  pm2_cnt_len;
  uint8_t  pm_tmr_len;
  uint8_t  gpe0_blk_len;
  uint8_t  gpe1_blk_len;
  uint8_t  gpe1_base;
  uint8_t  cst_cnt;
  uint16_t p_lvl2_lat;
  uint16_t p_lvl3_lat;
  uint16_t flush_size;
  uint16_t flush_stride;
  uint8_t  duty_offset;
  uint8_t  duty_width;
  uint8_t  day_alrm;
  uint8_t  mon_alrm;
  uint8_t  century;
  uint16_t iapc_boot_arch;
  uint8_t  reserved1;
  uint32_t flags;
  struct acpi_gas reset_reg;
  uint8_t  reset_value;
  uint8_t  reserved2[3];
  uint64_t x_firmware_ctrl;
  uint64_t x_dsdt;
} __attribute__((packed));

#define ACPI_FADT_RESET_REG_SUP (1U << 10)

int acpi_init(void);
void acpi_print_summary(void);
void *acpi_find_table(const char *signature);
int acpi_mp_init(void);
void acpi_poweroff(void) __attribute__((noreturn));
void acpi_reboot(void) __attribute__((noreturn));

#endif /* PRONINX_ACPI_H */
