#include "boot/uefi/uefi.h"

#define KERNEL_PATH "/fnu/kernel"
#define RAMDISK_PATH "/fnu/ramdisk"

// kinit1 frees four megabytes past the last module without consulting the
// map, so the loader owns that much slack behind the ramdisk
#define KERNEL_SLACK (8 * 1024 * 1024)

#define MEMMAP_SLACK 8192
#define LOW_LIMIT 0xffffffffULL

struct elfhdr {
  uint32 magic;
  uint8 ident[12];
  uint16 type;
  uint16 machine;
  uint32 version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint32 flags;
  uint16 ehsize;
  uint16 phentsize;
  uint16 phnum;
  uint16 shentsize;
  uint16 shnum;
  uint16 shstrndx;
};

struct proghdr {
  uint32 type;
  uint32 flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
};

_Static_assert(sizeof(struct elfhdr) == 64, "elfhdr");
_Static_assert(sizeof(struct proghdr) == 56, "proghdr");

#define PHDR_MAX 16

struct efi_system_table *ST;
struct efi_boot_services *BS;
efi_handle IMAGE;

void *memcpy(void *d, const void *s, uint64 n) {
  uint8 *a = d;
  const uint8 *b = s;

  while (n--)
    *a++ = *b++;
  return d;
}

void *memset(void *d, int c, uint64 n) {
  uint8 *a = d;

  while (n--)
    *a++ = (uint8)c;
  return d;
}

int memcmp(const void *a, const void *b, uint64 n) {
  const uint8 *x = a, *y = b;

  while (n--) {
    if (*x != *y)
      return *x - *y;
    x++;
    y++;
  }
  return 0;
}

int guid_eq(const struct efi_guid *a, const struct efi_guid *b) {
  return a->a == b->a && a->b == b->b && a->c == b->c &&
         memcmp(a->d, b->d, sizeof(a->d)) == 0;
}

void putstr(const char *s) {
  efi_char line[128];
  uint32 n = 0;

  if (ST == 0 || ST->con_out == 0)
    return;
  for (; *s; s++) {
    if (*s == '\n')
      line[n++] = '\r';
    line[n++] = (efi_char)(uint8)*s;
    if (n + 2 >= sizeof(line) / sizeof(line[0])) {
      line[n] = 0;
      ST->con_out->output_string(ST->con_out, line);
      n = 0;
    }
  }
  line[n] = 0;
  ST->con_out->output_string(ST->con_out, line);
}

void putdec(uint64 v) {
  char buf[24];
  int i = (int)sizeof(buf) - 1;

  buf[i] = 0;
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  putstr(buf + i);
}

void puthex(uint64 v) {
  static const char digits[] = "0123456789abcdef";
  char buf[19];
  int i = (int)sizeof(buf) - 1;

  buf[i] = 0;
  do {
    buf[--i] = digits[v & 0xf];
    v >>= 4;
  } while (v != 0);
  buf[--i] = 'x';
  buf[--i] = '0';
  putstr(buf + i);
}

void panic(const char *s) {
  putstr("\nboot: ");
  putstr(s);
  putstr("\n");
  for (;;)
    __asm__ volatile("hlt");
}

static struct fnu_bootinfo binfo;

static const struct efi_guid acpi20 = EFI_ACPI_20_TABLE_GUID;
static const struct efi_guid acpi10 = EFI_ACPI_10_TABLE_GUID;

static uint64 acpi_rsdp(void) {
  struct efi_configuration_table *t = ST->configuration_table;
  uint64 fallback = 0;
  efi_uintn i;

  for (i = 0; i < ST->table_entries; i++) {
    if (guid_eq(&t[i].vendor_guid, &acpi20))
      return (uint64)(uintptr_t)t[i].vendor_table;
    if (guid_eq(&t[i].vendor_guid, &acpi10))
      fallback = (uint64)(uintptr_t)t[i].vendor_table;
  }
  return fallback;
}

// the kernel is linked at a fixed physical address. when the firmware has
// already claimed it, say which ranges are in the way instead of guessing
static void report_conflict(uint64 lo, uint64 hi) {
  static uint8 buf[65536];
  efi_uintn n = sizeof(buf), key, ds;
  const uint8 *p;
  uint32 dv;

  if (EFI_ERROR(BS->get_memory_map(&n, (struct efi_memory_descriptor *)buf,
                                   &key, &ds, &dv)))
    return;
  putstr("boot: wanted ");
  puthex(lo);
  putstr(" .. ");
  puthex(hi);
  putstr(", firmware holds\n");
  for (p = buf; p + ds <= buf + n; p += ds) {
    const struct efi_memory_descriptor *d =
        (const struct efi_memory_descriptor *)p;
    uint64 end = d->physical_start + d->pages * EFI_PAGE_SIZE;

    if (d->type == EFI_CONVENTIONAL_MEMORY || end <= lo ||
        d->physical_start >= hi)
      continue;
    putstr("  ");
    puthex(d->physical_start);
    putstr(" .. ");
    puthex(end);
    putstr(" type ");
    putdec(d->type);
    putstr("\n");
  }
}

static uint64 pages_for(uint64 bytes) {
  return (bytes + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;
}

static efi_phys alloc_low(uint64 bytes) {
  efi_phys at = LOW_LIMIT;

  if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                                   pages_for(bytes), &at)))
    return 0;
  return at;
}

// one contiguous block from the kernel's own load address, so the layout the
// kernel already assumes under bios holds here too
static uint64 load_kernel(void) {
  struct efi_file *f;
  struct elfhdr eh;
  struct proghdr ph[PHDR_MAX];
  efi_phys block;
  uint64 lo = ~0ULL, hi = 0, got, rsize = 0;
  uint32 i;

  f = file_open(KERNEL_PATH);
  if (f == 0)
    panic("no " KERNEL_PATH " on the esp");
  if (file_pread(f, 0, &eh, sizeof(eh), &got) < 0 || got != sizeof(eh))
    panic("cannot read the kernel header");
  if (eh.magic != 0x464c457fu)
    panic("kernel not elf");
  if (eh.phentsize != sizeof(ph[0]))
    panic("bad phentsize");
  if (eh.phnum == 0 || eh.phnum > PHDR_MAX)
    panic("unexpected program header count");
  if (file_pread(f, eh.phoff, ph, (uint64)eh.phnum * sizeof(ph[0]), &got) < 0)
    panic("cannot read the program headers");

  for (i = 0; i < eh.phnum; i++) {
    if (ph[i].type != 1)
      continue;
    if (ph[i].memsz < ph[i].filesz)
      panic("bad segment");
    if (ph[i].paddr < lo)
      lo = ph[i].paddr;
    if (ph[i].paddr + ph[i].memsz > hi)
      hi = ph[i].paddr + ph[i].memsz;
  }
  if (hi == 0)
    panic("kernel has no load segments");
  lo &= ~(uint64)(EFI_PAGE_SIZE - 1);
  hi = (hi + EFI_PAGE_SIZE - 1) & ~(uint64)(EFI_PAGE_SIZE - 1);

  {
    struct efi_file *r = file_open(RAMDISK_PATH);
    if (r != 0) {
      rsize = file_length(r);
      file_close(r);
    }
  }

  // bios layout first: kernel, ramdisk, then room for the kernel first
  // allocations. firmware keeps pages down here, so fall back if it will not.
  block = lo;
  if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                   pages_for(hi - lo + rsize + KERNEL_SLACK),
                                   &block))) {
    block = lo;
    if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                     pages_for(hi - lo + rsize), &block))) {
      report_conflict(lo, hi + rsize);
      panic("firmware will not give up the kernel's load address");
    }
  }

  for (i = 0; i < eh.phnum; i++) {
    if (ph[i].type != 1)
      continue;
    if (file_pread(f, ph[i].off, (void *)(uintptr_t)ph[i].paddr, ph[i].filesz,
                   &got) < 0 ||
        got != ph[i].filesz)
      panic("short read on a kernel segment");
    if (ph[i].memsz > ph[i].filesz)
      memset((void *)(uintptr_t)(ph[i].paddr + ph[i].filesz), 0,
             ph[i].memsz - ph[i].filesz);
  }
  file_close(f);

  if (rsize != 0) {
    struct efi_file *r = file_open(RAMDISK_PATH);
    if (r != 0) {
      if (file_pread(r, 0, (void *)(uintptr_t)hi, rsize, &got) == 0 &&
          got == rsize) {
        binfo.module[0].base = hi;
        binfo.module[0].size = rsize;
        memcpy(binfo.module[0].name, "ramdisk", 8);
        binfo.modules = 1;
      } else {
        putstr("boot: ramdisk unreadable, continuing without it\n");
      }
      file_close(r);
    }
  }
  return eh.entry;
}

efi_status EFIAPI efi_main(efi_handle image, struct efi_system_table *st) {
  efi_uintn mapsize = 0, key = 0, descsize = 0, have;
  struct fnu_bootinfo *bi;
  uint64 entry, cr3;
  efi_phys map, info;
  uint32 descver = 0;
  efi_status s = EFI_SUCCESS;
  int tries;

  ST = st;
  BS = st->boot_services;
  IMAGE = image;

  BS->set_watchdog_timer(0, 0, 0, 0);
  putstr("\nOpenProninx UEFI loader\n");

  memset(&binfo, 0, sizeof(binfo));
  binfo.magic = FNU_BOOTINFO_MAGIC;
  binfo.version = FNU_BOOTINFO_VERSION;
  binfo.size = sizeof(binfo);
  binfo.firmware = FNU_FIRMWARE_UEFI;
  binfo.acpi_rsdp = acpi_rsdp();
  binfo.efi_system_table = (uint64)(uintptr_t)st;
  binfo.efi_runtime_services = (uint64)(uintptr_t)st->runtime_services;

  if (file_mount() < 0)
    panic("cannot open the volume this loader came from");

  entry = load_kernel();
  video_setup(&binfo);
  cr3 = paging_setup();

  // bootinfo_init only looks below four gigabytes
  info = alloc_low(sizeof(binfo));
  if (info == 0)
    panic("no low memory for the boot information");
  bi = (struct fnu_bootinfo *)(uintptr_t)info;
  memcpy(bi, &binfo, sizeof(binfo));

  BS->get_memory_map(&mapsize, 0, &key, &descsize, &descver);
  mapsize += MEMMAP_SLACK;
  map = alloc_low(mapsize);
  if (map == 0)
    panic("no room for the memory map");

  for (tries = 0; tries < 8; tries++) {
    have = mapsize;
    s = BS->get_memory_map(&have,
                           (struct efi_memory_descriptor *)(uintptr_t)map, &key,
                           &descsize, &descver);
    if (EFI_ERROR(s))
      panic("cannot read the memory map");
    s = BS->exit_boot_services(image, key);
    if (!EFI_ERROR(s)) {
      __asm__ volatile("cli");
      break;
    }
  }
  if (EFI_ERROR(s))
    panic("firmware will not release the machine");

  // nothing below may call the firmware again
  memory_map(bi, (const void *)(uintptr_t)map, have, descsize);
  enter_kernel(entry, cr3, info);
}
