#include "boot/uefi/uefi.h"

#define MAP_MAX 512

#define DMAP_PML4 256
#define PTE_P 0x001
#define PTE_W 0x002
#define PTE_PS 0x080

#define IDENTITY_GB 4
#define PD_POOL 8
#define TABLE_PAGES (4 + PD_POOL)

static struct fnu_memrange raw[MAP_MAX];
static struct fnu_efi_range rt[MAP_MAX];

// FreeBSD EFI_ALLOWED_TYPES_MASK
static int runtime_needed(const struct efi_memory_descriptor *d) {
  if ((d->attribute & EFI_MEMORY_RUNTIME) != 0)
    return 1;
  switch (d->type) {
  case EFI_BOOT_SERVICES_CODE:
  case EFI_BOOT_SERVICES_DATA:
  case EFI_RUNTIME_SERVICES_CODE:
  case EFI_RUNTIME_SERVICES_DATA:
  case EFI_ACPI_MEMORY_NVS:
    return 1;
  default:
    return 0;
  }
}

static void rt_sort(uint32 n) {
  struct fnu_efi_range t;
  uint32 i, j;

  for (i = 1; i < n; i++) {
    t = rt[i];
    j = i;
    while (j > 0 && rt[j - 1].base > t.base) {
      rt[j] = rt[j - 1];
      j--;
    }
    rt[j] = t;
  }
}

static uint32 rt_merge(uint32 n) {
  uint32 i, out = 0;

  for (i = 0; i < n; i++) {
    // join by attribute; it is all the kernel maps by
    if (out > 0 && rt[out - 1].attribute == rt[i].attribute &&
        rt[out - 1].base + rt[out - 1].pages * EFI_PAGE_SIZE == rt[i].base) {
      rt[out - 1].pages += rt[i].pages;
      continue;
    }
    rt[out++] = rt[i];
  }
  return out;
}

// after NetBSD efiboot getmemtype
static uint32 fnu_type(const struct efi_memory_descriptor *d) {
  switch (d->type) {
  case EFI_LOADER_CODE:
  case EFI_LOADER_DATA:
  case EFI_BOOT_SERVICES_CODE:
  case EFI_BOOT_SERVICES_DATA:
  case EFI_CONVENTIONAL_MEMORY:
    return (d->attribute & EFI_MEMORY_WB) ? FNU_MEM_RAM : FNU_MEM_RESERVED;
  case EFI_ACPI_RECLAIM_MEMORY:
    return FNU_MEM_ACPI_RECLAIM;
  case EFI_ACPI_MEMORY_NVS:
    return FNU_MEM_ACPI_NVS;
  case EFI_UNUSABLE_MEMORY:
    return FNU_MEM_UNUSABLE;
  default:
    return FNU_MEM_RESERVED;
  }
}

static void sort(uint32 n) {
  struct fnu_memrange t;
  uint32 i, j;

  for (i = 1; i < n; i++) {
    t = raw[i];
    j = i;
    while (j > 0 && raw[j - 1].base > t.base) {
      raw[j] = raw[j - 1];
      j--;
    }
    raw[j] = t;
  }
}

static uint32 merge(uint32 n) {
  uint32 i, out = 0;

  for (i = 0; i < n; i++) {
    if (out > 0 && raw[out - 1].type == raw[i].type &&
        raw[out - 1].base + raw[out - 1].length == raw[i].base) {
      raw[out - 1].length += raw[i].length;
      continue;
    }
    raw[out++] = raw[i];
  }
  return out;
}

// a dropped reserved range is a hole the kernel was never going to use
static uint32 trim(uint32 n) {
  uint32 i, victim;

  while (n > FNU_MEM_MAX) {
    victim = n;
    for (i = 0; i < n; i++) {
      if (raw[i].type == FNU_MEM_RAM)
        continue;
      if (victim == n || raw[i].length < raw[victim].length)
        victim = i;
    }
    if (victim == n)
      victim = 0;
    for (i = victim; i + 1 < n; i++)
      raw[i] = raw[i + 1];
    n--;
  }
  return n;
}

void memory_map(struct fnu_bootinfo *bi, const void *map, uint64 bytes,
                uint64 descsize) {
  const uint8 *p = map;
  const uint8 *end = p + bytes;
  uint32 n = 0, rtn = 0, i;

  if (descsize < sizeof(struct efi_memory_descriptor))
    return;
  for (; p + descsize <= end && n < MAP_MAX; p += descsize) {
    const struct efi_memory_descriptor *d =
        (const struct efi_memory_descriptor *)p;

    if (d->pages == 0)
      continue;
    // mapped back at their own addresses for the length of a runtime call
    if (runtime_needed(d) && rtn < MAP_MAX &&
        (d->physical_start & (EFI_PAGE_SIZE - 1)) == 0) {
      rt[rtn].base = d->physical_start;
      rt[rtn].pages = d->pages;
      rt[rtn].attribute = d->attribute;
      rt[rtn].type = d->type;
      rt[rtn].pad = 0;
      rtn++;
    }
    raw[n].base = d->physical_start;
    raw[n].length = d->pages * EFI_PAGE_SIZE;
    raw[n].type = fnu_type(d);
    raw[n].pad = 0;
    n++;
  }

  sort(n);
  n = trim(merge(n));

  for (i = 0; i < n; i++)
    bi->mem[i] = raw[i];
  bi->memranges = n;

  rt_sort(rtn);
  rtn = rt_merge(rtn);
  // a hole in the map is a page the firmware would fault on
  if (rtn > FNU_EFI_RANGE_MAX)
    rtn = 0;
  for (i = 0; i < rtn; i++)
    bi->efi_rt[i] = rt[i];
  bi->efi_ranges = rtn;
}

static int has_1g_pages(void) {
  uint32 a, b, c, d;

  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x80000000u));
  if (a < 0x80000001u)
    return 0;
  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x80000001u));
  return (d & (1u << 26)) != 0;
}

static uint64 rsp_now(void) {
  uint64 sp;

  __asm__ volatile("movq %%rsp,%0" : "=r"(sp));
  return sp;
}

static uint64 *pd_pool;
static uint32 pd_used;

// one gigabyte of 2M entries, shared by the identity and direct maps
static void map_gb(uint64 *pdpt, uint64 *dmap, uint32 idx) {
  uint64 *pd;
  uint32 i;

  if (idx >= 512 || (pdpt[idx] & PTE_P) != 0)
    return;
  if (pd_used >= PD_POOL)
    panic("page table pool exhausted");
  pd = pd_pool + pd_used++ * 512;
  for (i = 0; i < 512; i++)
    pd[i] = (((uint64)idx << 30) | ((uint64)i << 21)) | PTE_P | PTE_W | PTE_PS;
  pdpt[idx] = (uint64)(uintptr_t)pd | PTE_P | PTE_W;
  dmap[idx] = pdpt[idx];
}

// identity map for the switch itself, the direct map and the kernel image.
// the kernel rebuilds all of this over the full memory map once it runs.
uint64 paging_setup(void) {
  uint64 *pml4, *pdpt_lo, *pdpt_dmap, *pdpt_hi;
  efi_phys block = 0xffffffffULL;
  uint32 i;

  if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                                   TABLE_PAGES, &block)))
    panic("no low memory for the page tables");

  pml4 = (uint64 *)(uintptr_t)block;
  pdpt_lo = pml4 + 512;
  pdpt_dmap = pdpt_lo + 512;
  pdpt_hi = pdpt_dmap + 512;
  pd_pool = pdpt_hi + 512;
  pd_used = 0;
  memset(pml4, 0, TABLE_PAGES * EFI_PAGE_SIZE);

  pml4[0] = (uint64)(uintptr_t)pdpt_lo | PTE_P | PTE_W;
  pml4[DMAP_PML4] = (uint64)(uintptr_t)pdpt_dmap | PTE_P | PTE_W;
  pml4[511] = (uint64)(uintptr_t)pdpt_hi | PTE_P | PTE_W;

  if (has_1g_pages()) {
    for (i = 0; i < 512; i++) {
      pdpt_lo[i] = ((uint64)i << 30) | PTE_P | PTE_W | PTE_PS;
      pdpt_dmap[i] = pdpt_lo[i];
    }
    pdpt_hi[510] = PTE_P | PTE_W | PTE_PS;
    return (uint64)(uintptr_t)pml4;
  }

  // no 1G entries: map the low four gigabytes plus the ones we sit in
  for (i = 0; i < IDENTITY_GB; i++)
    map_gb(pdpt_lo, pdpt_dmap, i);
  map_gb(pdpt_lo, pdpt_dmap, (uint32)(rsp_now() >> 30));
  map_gb(pdpt_lo, pdpt_dmap, (uint32)((uint64)(uintptr_t)&paging_setup >> 30));
  map_gb(pdpt_lo, pdpt_dmap, (uint32)((uint64)(uintptr_t)pd_pool >> 30));
  pdpt_hi[510] = pdpt_lo[0];
  return (uint64)(uintptr_t)pml4;
}

void enter_kernel(uint64 entry, uint64 cr3, uint64 info) {
  register uint64 target __asm__("rax") = entry;
  register uint64 arg __asm__("rdi") = info;
  register uint64 table __asm__("rcx") = cr3;

  __asm__ volatile("cli\n\t"
                   "movq %2, %%cr3\n\t"
                   "jmp *%0"
                   :
                   : "r"(target), "r"(arg), "r"(table)
                   : "memory");
  __builtin_unreachable();
}
