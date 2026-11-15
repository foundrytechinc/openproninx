#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "x86.h"

// UEFI RUNTIME SERVICES
// the firmware wants its physical addresses: calls run on a 1:1 map of their own

#define EFI_RESET_COLD 0
#define EFI_RESET_WARM 1
#define EFI_RESET_SHUTDOWN 2

struct efi_runtime {
  uint8_t hdr[24];
  void *get_time;
  void *set_time;
  void *get_wakeup_time;
  void *set_wakeup_time;
  void *set_virtual_address_map;
  void *convert_pointer;
  void *get_variable;
  void *get_next_variable_name;
  void *set_variable;
  void *get_next_high_monotonic_count;
  void *reset_system;
  void *update_capsule;
  void *query_capsule_capabilities;
  void *query_variable_info;
};

_Static_assert(__offsetof(struct efi_runtime, reset_system) == 104,
               "ResetSystem is the eleventh entry after the header");

typedef void(EFIAPI *efi_reset_fn)(uint32_t type, uint64_t status,
                                   uint64_t size, void *data);

extern pte_t *kernel_pgdir;

static pte_t *efi_pgdir;
static uint64_t efi_runtime_phys;
static uint32_t efi_range_count;

void efirt_init(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  uint i;

  if (bi == 0 || bi->firmware != FNU_FIRMWARE_UEFI)
    return;
  if (bi->efi_runtime_services == 0 || bi->efi_ranges == 0)
    return;
  if ((efi_pgdir = (pte_t *)page_alloc()) == 0)
    return;

  memmove(efi_pgdir, kernel_pgdir, PGSIZE);

  for (i = 0; i < bi->efi_ranges; i++) {
    uint64_t base = bi->efi_rt[i].base;
    uint64_t len = bi->efi_rt[i].pages * PGSIZE;
    int perm = 0;

    if ((bi->efi_rt[i].attribute & FNU_EFI_MEMORY_RO) == 0)
      perm |= PTE_W;
    if ((bi->efi_rt[i].attribute & FNU_EFI_MEMORY_WB) == 0)
      perm |= PTE_PCD;
    if (len == 0 || (base & (PGSIZE - 1)) != 0)
      continue;
    if (map_range(efi_pgdir, base, base, len, perm) < 0) {
      efi_pgdir = 0;
      return;
    }
  }

  if (map_range(efi_pgdir, 0, 0, PGSIZE, PTE_W) < 0) {
    efi_pgdir = 0;
    return;
  }
  efi_runtime_phys = bi->efi_runtime_services;
  efi_range_count = bi->efi_ranges;
}

void efirt_report(void) {
  const struct fnu_bootinfo *bi = bootinfo();

  if (bi != 0 && bi->firmware == FNU_FIRMWARE_UEFI && efi_pgdir == 0)
    printf("EFI: runtime services unavailable, reset falls back to acpi\n");
}

int efirt_available(void) { return efi_pgdir != 0; }

static int efirt_reset(uint32_t type) {
  volatile struct efi_runtime *rt;
  efi_reset_fn reset;
  uintptr_t cr3;

  if (efi_pgdir == 0)
    return -1;

  cli();
  cr3 = rcr3();
  lcr3(V2P(efi_pgdir));
  rt = (volatile struct efi_runtime *)(uintptr_t)efi_runtime_phys;
  reset = (efi_reset_fn)rt->reset_system;
  if (reset != 0)
    reset(type, 0, 0, 0);
  lcr3(cr3);
  return -1;
}

int efirt_poweroff(void) { return efirt_reset(EFI_RESET_SHUTDOWN); }

int efirt_reboot(void) { return efirt_reset(EFI_RESET_COLD); }
