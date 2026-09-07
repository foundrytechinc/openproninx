#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "x86.h"

/* uefi runtime services. the firmware still wants its own physical
 * addresses, so the call runs on a 1:1 map of its own, the way FreeBSD
 * efi_create_1t1_map does. */

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

_Static_assert(__builtin_offsetof(struct efi_runtime, reset_system) == 104,
               "ResetSystem is the eleventh entry after the header");

typedef void(__attribute__((ms_abi)) * efi_reset_fn)(uint32_t type,
                                                     uint64_t status,
                                                     uint64_t size, void *data);

extern pte_t *kpgdir;

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
  if ((efi_pgdir = (pte_t *)kalloc()) == 0)
    return;

  // the kernel half stays, so the call runs on the stack it arrived on
  memmove(efi_pgdir, kpgdir, PGSIZE);

  // only what the firmware described, where it described it, cached the way
  // it asked. OpenBSD efi_map_runtime. nothing here uses NX.
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

  // FreeBSD efi_create_1t1_map maps physical page zero too
  if (map_range(efi_pgdir, 0, 0, PGSIZE, PTE_W) < 0) {
    efi_pgdir = 0;
    return;
  }
  efi_runtime_phys = bi->efi_runtime_services;
  efi_range_count = bi->efi_ranges;
}

// the console does not exist when efirt_init runs, so this is said later
void efirt_report(void) {
  const struct fnu_bootinfo *bi = bootinfo();

  if (bi == 0 || bi->firmware != FNU_FIRMWARE_UEFI)
    return;
  if (efi_pgdir == 0) {
    cprintf("EFI: runtime services unavailable, reset falls back to acpi\n");
    return;
  }
  cprintf("EFI: runtime services at 0x%x, %u mapped ranges\n",
          efi_runtime_phys, (uint)efi_range_count);
}

int efirt_available(void) { return efi_pgdir != 0; }

// returns only when the firmware declined
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
