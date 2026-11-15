#ifndef PRONINX_ACPI_H
#define PRONINX_ACPI_H

#include "inc/types.h"

// ACPI

// RSDP
struct rsdp_descriptor {
  char signature[8]; // "RSD PTR "
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
} __packed;

struct xsdp_descriptor {
  struct rsdp_descriptor rsdp;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __packed;

struct acpi_sdt_header {
  char signature[4];
  uint32_t length; // with the header
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __packed;

// GAS
struct acpi_gas {
  uint8_t address_space_id;
#define GAS_SYSTEM_MEMORY 0
#define GAS_SYSTEM_IOSPACE 1
#define GAS_PCI_CFG_SPACE 2
#define GAS_EMBEDDED 3
#define GAS_SMBUS 4
#define GAS_FUNCTIONAL_FIXED 127
  uint8_t register_bit_width;
  uint8_t register_bit_offset;
  uint8_t access_size;
  uint64_t address;
} __packed;

// MADT
struct acpi_madt {
  struct acpi_sdt_header header;
  uint32_t lapic_address;
  uint32_t flags; // 1: dual 8259
} __packed;

#define ACPI_MADT_TYPE_LAPIC 0
#define ACPI_MADT_TYPE_IOAPIC 1
#define ACPI_MADT_TYPE_INT_OVERRIDE 2
#define ACPI_MADT_TYPE_NMI_SOURCE 3
#define ACPI_MADT_TYPE_LAPIC_NMI 4
#define ACPI_MADT_TYPE_LAPIC_OVERRIDE 5
#define ACPI_MADT_TYPE_X2APIC 9
#define ACPI_MADT_TYPE_X2APIC_NMI 10

struct acpi_subtable_header {
  uint8_t type;
  uint8_t length;
} __packed;

struct madt_lapic {
  struct acpi_subtable_header header;
  uint8_t processor_id;
  uint8_t apic_id;
  uint32_t flags;
} __packed;

struct madt_x2apic {
  struct acpi_subtable_header header;
  uint16_t reserved;
  uint32_t x2apic_id;
  uint32_t flags;
  uint32_t uid;
} __packed;

struct madt_lapic_nmi {
  struct acpi_subtable_header header;
  uint8_t processor_id;
  uint16_t flags;
  uint8_t lint;
} __packed;

struct madt_x2apic_nmi {
  struct acpi_subtable_header header;
  uint16_t flags;
  uint32_t uid;
  uint8_t lint;
  uint8_t reserved[3];
} __packed;

struct madt_ioapic {
  struct acpi_subtable_header header;
  uint8_t ioapic_id;
  uint8_t reserved;
  uint32_t ioapic_address;
  uint32_t gsi_base;
} __packed;

struct madt_int_override {
  struct acpi_subtable_header header;
  uint8_t bus; // 0 is ISA
  uint8_t source_irq;
  uint32_t gsi;
  uint16_t flags;
} __packed;

struct madt_lapic_override {
  struct acpi_subtable_header header;
  uint16_t reserved;
  uint64_t lapic_address;
} __packed;

// FADT
struct acpi_fadt {
  struct acpi_sdt_header header;
  uint32_t firmware_ctrl; // the FACS
  uint32_t dsdt;
  uint8_t int_model; // revision 1 only
  uint8_t pm_profile;
  uint16_t sci_int;
  uint32_t smi_cmd;
  uint8_t acpi_enable;
  uint8_t acpi_disable;
  uint8_t s4bios_req;
  uint8_t pstate_cnt;
  uint32_t pm1a_evt_blk;
  uint32_t pm1b_evt_blk;
  uint32_t pm1a_cnt_blk;
  uint32_t pm1b_cnt_blk;
  uint32_t pm2_cnt_blk;
  uint32_t pm_tmr_blk;
  uint32_t gpe0_blk;
  uint32_t gpe1_blk;
  uint8_t pm1_evt_len;
  uint8_t pm1_cnt_len;
  uint8_t pm2_cnt_len;
  uint8_t pm_tmr_len;
  uint8_t gpe0_blk_len;
  uint8_t gpe1_blk_len;
  uint8_t gpe1_base;
  uint8_t cst_cnt;
  uint16_t p_lvl2_lat;
  uint16_t p_lvl3_lat;
  uint16_t flush_size;
  uint16_t flush_stride;
  uint8_t duty_offset;
  uint8_t duty_width;
  uint8_t day_alrm;
  uint8_t mon_alrm;
  uint8_t century;
  uint16_t iapc_boot_arch;
  uint8_t reserved1;
  uint32_t flags;
  // from revision 2
  struct acpi_gas reset_reg;
  uint8_t reset_value;
  uint8_t reserved2a;
  uint8_t reserved2b;
  uint8_t fadt_minor;
  uint64_t x_firmware_ctrl;
  uint64_t x_dsdt;
  struct acpi_gas x_pm1a_evt_blk;
  struct acpi_gas x_pm1b_evt_blk;
  struct acpi_gas x_pm1a_cnt_blk;
  struct acpi_gas x_pm1b_cnt_blk;
  struct acpi_gas x_pm2_cnt_blk;
  struct acpi_gas x_pm_tmr_blk;
  struct acpi_gas x_gpe0_blk;
  struct acpi_gas x_gpe1_blk;
  struct acpi_gas sleep_control_reg;
  struct acpi_gas sleep_status_reg;
} __packed;

#define FADT_PWR_BUTTON 0x00000010 // no fixed power button
#define FADT_SLP_BUTTON 0x00000020
#define FADT_RESET_REG_SUP 0x00000400
#define FADT_HW_REDUCED_ACPI 0x00100000
#define ACPI_FADT_RESET_REG_SUP FADT_RESET_REG_SUP

struct acpi_facs {
  char signature[4];
  uint32_t length;
  uint32_t hardware_signature;
  uint32_t wakeup_vector;
  uint32_t global_lock;
  uint32_t flags;
  uint64_t x_wakeup_vector;
  uint8_t version;
} __packed;

#define GL_BIT_PENDING 0x01
#define GL_BIT_OWNED 0x02

struct acpi_ecdt {
  struct acpi_sdt_header header;
  struct acpi_gas ec_control;
  struct acpi_gas ec_data;
  uint32_t uid;
  uint8_t gpe_bit;
  char ec_id[1];
} __packed;

// PM1
#define ACPI_PM1_TMR_STS 0x0001
#define ACPI_PM1_GBL_STS 0x0020
#define ACPI_PM1_PWRBTN_STS 0x0100
#define ACPI_PM1_SLPBTN_STS 0x0200
#define ACPI_PM1_RTC_STS 0x0400
#define ACPI_PM1_WAK_STS 0x8000
// PM1
#define ACPI_PM1_PWRBTN_EN 0x0100
#define ACPI_PM1_SLPBTN_EN 0x0200
#define ACPI_PM1_RTC_EN 0x0400
// PM1
#define ACPI_PM1_SCI_EN 0x0001
#define ACPI_PM1_GBL_RLS 0x0004
#define ACPI_PM1_SLP_TYP_SHIFT 10
#define ACPI_PM1_SLP_TYP_MASK 0x1c00
#define ACPI_PM1_SLP_EN 0x2000
#define ACPI_PM2_ARB_DIS 0x0001

// operation region spaces
#define ACPI_OPREG_SYSMEM 0
#define ACPI_OPREG_SYSIO 1
#define ACPI_OPREG_PCICFG 2
#define ACPI_OPREG_EC 3
#define ACPI_OPREG_SMBUS 4
#define ACPI_OPREG_CMOS 5
#define ACPI_OPREG_PCIBAR 6
#define ACPI_OPREG_IPMI 7
#define ACPI_OPREG_GPIO 8
#define ACPI_OPREG_GSB 9

#define ACPI_PCI_SEG(a) ((uint16_t)((a) >> 48))
#define ACPI_PCI_BUS(a) ((uint8_t)((a) >> 40))
#define ACPI_PCI_DEV(a) ((uint8_t)((a) >> 32))
#define ACPI_PCI_FN(a) ((uint16_t)((a) >> 16))
#define ACPI_PCI_REG(a) ((uint16_t)(a))

#define ACPI_STATE_S0 0
#define ACPI_STATE_S5 5

// _STA
#define STA_PRESENT 0x01
#define STA_ENABLED 0x02
#define STA_SHOW_UI 0x04
#define STA_DEV_OK 0x08
#define STA_BATTERY 0x10

#define ACPI_DEV_PCIB "PNP0A03"
#define ACPI_DEV_PCIEB "PNP0A08"
#define ACPI_DEV_ECD "PNP0C09"
#define ACPI_DEV_CMB "PNP0C0A"
#define ACPI_DEV_PBD "PNP0C0C"
#define ACPI_DEV_LD "PNP0C0D"
#define ACPI_DEV_SBD "PNP0C0E"
#define ACPI_DEV_AC "ACPI0003"

#define ACPI_AML_PAD (2 + 255 * 4 + 4)

// TABLES

int acpi_init(void);
void acpi_print_summary(void);
void *acpi_find_table(const char *signature);
void acpi_tables_attach(void);
void dmar_init(void);
int acpi_table_count(void);
struct acpi_sdt_header *acpi_table(int i);
struct acpi_fadt *acpi_fadt(void);
int acpi_century_index(void);
int acpi_mp_init(void);
int acpi_pm_trace_on(void);
uint64_t acpi_rsdp_pa(void);
uint64_t acpi_mcfg_addr(int seg, int bus, int dev, int fn, int reg);
int acpi_isa_irq(int irq, int sci, int *gsi, int *level, int *low);
int acpi_ioapic_each(void (*fn)(int id, uint64_t addr, int gsibase));

int acpi_region_sysio(void *cookie, int dir, uint64_t addr, int size,
                      uint64_t *val);
int acpi_region_sysmem(void *cookie, int dir, uint64_t addr, int size,
                       uint64_t *val);
int acpi_region_pcicfg(void *cookie, int dir, uint64_t addr, int size,
                       uint64_t *val);
int acpi_acquire_glk(uint32_t *lock);
int acpi_release_glk(uint32_t *lock);
struct acpi_facs *acpi_facs(void);
struct acpi_sdt_header *acpi_dsdt(void);
uint8_t *acpi_table_aml(int i);
uint8_t *acpi_table_copy(uint64_t pa, uint *len);

int acpi_gasio(int dir, int space, uint64_t address, int access, int len,
               void *buf);
#define ACPI_IOREAD 0
#define ACPI_IOWRITE 1

// OSPM

#define GPE_LEVEL 1
#define GPE_EDGE 2

struct aml_node;

void acpi_start(void);
void acpi_thread_start(void);
int64_t acpi_getsta(struct aml_node *node);
void acpi_task_add(void (*fn)(void *, int), void *arg0, int arg1);
int acpi_task_run(void);
void acpi_poll_register(void (*fn)(void *), void *arg);
void acpi_poll_every(void (*fn)(void *), void *arg, int tenths);
int acpi_gpe_handler(int gpe, int (*fn)(int, void *), void *arg, int flags);
void acpi_gpe_enable(int gpe);
void acpi_gpe_done(int gpe);
void acpi_powerbutton(void);
void acpi_power_pressed(void);
void acpi_sleep_pressed(void);
void acpi_lid_closed(void);
void acpi_lid_opened(void);
void acpi_backlight_off(void);
void acpi_backlight_on(void);
void acpi_poll_now(void);
void acpi_sci_poll(void);
int acpi_tasks_pending(void);
void acpi_glk_release_signal(void);

// SLEEP
int acpi_sleep_supported(int state);
int acpi_sleep_registers(int state);
void acpi_sleep_events(void);
void acpi_wake_capture(void);
const char *acpi_wake_reason(char *buf, size_t n);
void acpi_wake_registers(void);
int acpi_sleep(int state);
void acpi_sleep_request(int state);
void acpi_wake_device(struct aml_node *dev);
void acpi_sleep_sysctl(void);

void acpi_poweroff_prepare(int off);
void acpi_poweroff(void) __dead;
int acpi_reset_via_fadt(void);
int acpi_reset_describe(uint32_t *space, uint64_t *address, uint32_t *value);
int acpi_enabled(void);

// DEVICES

struct acpi_driver {
  const char *name;
  const char *const *hids;
  int early;
  void (*attach)(struct aml_node *dev, const char *hid);
};

void acpi_attach_early(void);
void acpi_attach_devices(void);
extern const struct acpi_driver acpi_ec_driver;
extern const struct acpi_driver psm_acpi_driver;
extern const struct acpi_driver acpi_button_driver;
extern const struct acpi_driver acpi_ac_driver;
extern const struct acpi_driver acpi_battery_driver;
extern const struct acpi_driver acpi_ideapad_driver;
extern const struct acpi_driver acpi_tpm_driver;
void acpi_tpm_suspend(void);
void acpi_tpm_shutdown(void);
void acpi_tpm_resume(void);
void acpi_ec_resume(void);
void acpi_thermal_attach(struct aml_node *zone);
void acpi_cpu_attach(void);
void acpi_cpu_cap(int percent);
int acpi_cpu_has_perf(void);
void acpi_ec_ecdt(void);

// THE PCI INTERRUPT ROUTING TABLE

void acpi_prt_init(void);
void acpi_prt_resume(void);
int acpi_pci_route(int bus, int dev, int pin, int *gsi, int *level, int *low);

#endif /* PRONINX_ACPI_H */
