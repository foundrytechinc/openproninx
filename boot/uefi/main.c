#include "boot/uefi/uefi.h"

#define KERNEL_PATH "/fnu/kernel"
#define RAMDISK_PATH "/fnu/ramdisk"

#define KERNEL_SLACK (8 * 1024 * 1024)

#define MEMMAP_SLACK 8192
#define LOW_LIMIT 0xffffffffULL
#define STAGE_LIMIT 0x10000000ULL
#define AUX_RETRY 8

extern char tramp_start[], tramp_args[], tramp_end[];

struct place {
  uint64 dest;
  uint64 stage;
  uint64 length;
  uint64 want;
};

static struct place place;

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

static struct fnu_bootinfo binfo;
static uint64 tsc0;
static uint32 logged;
static int at_line_start = 1;

static uint64 rdtsc(void) {
  uint32 lo, hi;
  __asm __volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64)hi << 32) | lo;
}

static void logc(char c) {
  if (at_line_start) {
    uint64 t = (rdtsc() - tsc0) >> 20;
    char num[24];
    int i = (int)sizeof(num) - 1;

    num[i] = 0;
    do {
      num[--i] = (char)('0' + t % 10);
      t /= 10;
    } while (t != 0);
    num[--i] = '+';
    for (; num[i] && logged < FNU_BOOTLOG_MAX - 2; i++)
      binfo.log[logged++] = num[i];
    if (logged < FNU_BOOTLOG_MAX - 2)
      binfo.log[logged++] = ' ';
    at_line_start = 0;
  }
  if (logged < FNU_BOOTLOG_MAX - 1)
    binfo.log[logged++] = c;
  at_line_start = c == '\n';
}

void putstr(const char *s) {
  efi_char line[128];
  uint32 n = 0;

  for (const char *p = s; *p; p++)
    logc(*p);
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
    __asm __volatile("hlt");
}

static const struct efi_guid acpi20 = EFI_ACPI_20_TABLE_GUID;
static const struct efi_guid acpi10 = EFI_ACPI_10_TABLE_GUID;
static const struct efi_guid smbios = EFI_SMBIOS_TABLE_GUID;
static const struct efi_guid smbios3 = EFI_SMBIOS3_TABLE_GUID;
static const struct efi_guid hoblist = EFI_HOB_LIST_GUID;

static uint64 hob_list(uint64 *len) {
  struct efi_configuration_table *t = ST->configuration_table;
  const uint8 *p, *start = 0;
  efi_uintn i;

  *len = 0;
  for (i = 0; i < ST->table_entries; i++)
    if (guid_eq(&t[i].vendor_guid, &hoblist))
      start = t[i].vendor_table;
  if (start == 0)
    return 0;
  for (p = start; p - start < 16 * 1024 * 1024;) {
    uint16 type = *(const uint16 *)p, hlen = *(const uint16 *)(p + 2);

    if (type == 0xffff) {
      *len = (uint64)(p - start) + 8;
      return (uint64)(uintptr_t)start;
    }
    if (hlen < 8)
      return 0;
    p += hlen;
  }
  return 0;
}

static uint64 smbios_entry(void) {
  struct efi_configuration_table *t = ST->configuration_table;
  uint64 v3 = 0;
  efi_uintn i;

  for (i = 0; i < ST->table_entries; i++) {
    if (guid_eq(&t[i].vendor_guid, &smbios))
      return (uint64)(uintptr_t)t[i].vendor_table;
    if (guid_eq(&t[i].vendor_guid, &smbios3))
      v3 = (uint64)(uintptr_t)t[i].vendor_table;
  }
  return v3;
}

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

static efi_phys alloc_below(uint64 bytes, uint64 limit) {
  efi_phys at = limit;

  if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_DATA,
                                   pages_for(bytes), &at)))
    return 0;
  return at;
}

static int overlaps(uint64 base, uint64 len, uint64 lo, uint64 hi) {
  return hi > lo && base < hi && lo < base + len;
}

static int efi_getkey(void) {
  struct efi_input_key k;

  if (ST->con_in->read_key_stroke(ST->con_in, &k) != EFI_SUCCESS)
    return -1;
  return k.unicode != 0 ? (int)k.unicode : -1;
}

static void efi_nap(void) { BS->stall(100000); }

efi_phys boot_alloc(uint64 bytes) {
  efi_phys keep[AUX_RETRY];
  efi_phys at = 0;
  uint32 kept = 0, i;

  for (;;) {
    at = alloc_below(bytes, LOW_LIMIT);
    if (at == 0)
      break;
    if (!overlaps(at, bytes, place.dest, place.dest + place.want))
      break;
    if (kept == AUX_RETRY) {
      at = 0;
      break;
    }
    keep[kept++] = at;
  }
  for (i = 0; i < kept; i++)
    BS->free_pages(keep[i], pages_for(bytes));
  return at;
}

static int destination_is_free(const void *map, uint64 bytes, uint64 descsize,
                               uint64 lo, uint64 hi) {
  const uint8 *p = map;
  const uint8 *end = p + bytes;
  uint64 covered = lo;

  for (; p + descsize <= end; p += descsize) {
    const struct efi_memory_descriptor *d =
        (const struct efi_memory_descriptor *)p;
    uint64 dend = d->physical_start + d->pages * EFI_PAGE_SIZE;

    if (dend <= lo || d->physical_start >= hi)
      continue;
    switch (d->type) {
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
    case EFI_CONVENTIONAL_MEMORY:
      break;
    default:
      return 0;
    }
    if (d->physical_start > covered)
      return 0;
    if (dend > covered)
      covered = dend;
  }
  return covered >= hi;
}

static uint64 load_kernel(int ramdisk) {
  struct efi_file *f;
  struct elfhdr eh;
  struct proghdr ph[PHDR_MAX];
  efi_phys block;
  uint64 lo = ~0ULL, hi = 0, got, rsize = 0, delta;
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

  // the live root only; a root on disk needs none
  if (ramdisk) {
    struct efi_file *r = file_open(RAMDISK_PATH);
    if (r != 0) {
      rsize = file_length(r);
      file_close(r);
    }
  }

  place.dest = lo;
  place.length = hi - lo;
  place.want = place.length + KERNEL_SLACK;

  block = lo;
  if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                   pages_for(place.want), &block))) {
    block = lo;
    if (EFI_ERROR(BS->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                     pages_for(place.length), &block)))
      block = 0;
  }
  if (block == 0) {
    block = alloc_below(place.length, STAGE_LIMIT);
    if (block != 0 && overlaps(block, place.length, lo, lo + place.want)) {
      BS->free_pages(block, pages_for(place.length));
      block = 0;
    }
    if (block == 0)
      block = alloc_below(place.length, LOW_LIMIT);
    if (block == 0) {
      report_conflict(lo, lo + place.want);
      panic("nowhere to put the kernel");
    }
  }
  place.stage = block;
  delta = block - lo;

  for (i = 0; i < eh.phnum; i++) {
    if (ph[i].type != 1)
      continue;
    if (file_pread(f, ph[i].off, (void *)(uintptr_t)(ph[i].paddr + delta),
                   ph[i].filesz, &got) < 0 ||
        got != ph[i].filesz)
      panic("short read on a kernel segment");
    if (ph[i].memsz > ph[i].filesz)
      memset((void *)(uintptr_t)(ph[i].paddr + delta + ph[i].filesz), 0,
             ph[i].memsz - ph[i].filesz);
  }
  file_close(f);

  if (rsize != 0) {
    struct efi_file *r = file_open(RAMDISK_PATH);
    efi_phys at = boot_alloc(rsize);
    if (r != 0 && at != 0) {
      if (file_pread(r, 0, (void *)(uintptr_t)at, rsize, &got) == 0 &&
          got == rsize) {
        binfo.module[0].base = at;
        binfo.module[0].size = rsize;
        memcpy(binfo.module[0].name, "ramdisk", 8);
        binfo.modules = 1;
      } else {
        putstr("boot: ramdisk unreadable, continuing without it\n");
      }
    }
    if (r != 0)
      file_close(r);
  }
  return eh.entry;
}

efi_status EFIAPI efi_main(efi_handle image, struct efi_system_table *st) {
  efi_uintn mapsize = 0, key = 0, descsize = 0, have;
  struct fnu_bootinfo *bi;
  uint64 entry, cr3, *args, hobs, hobslen;
  efi_phys map, info, tramp = 0;
  uint32 descver = 0;
  efi_status s = EFI_SUCCESS;
  struct bootdisk disk;
  struct bootenv env;
  uint8 guid[16];
  char uuid[37];
  int tries, disk_root = 0;

  ST = st;
  BS = st->boot_services;
  IMAGE = image;
  tsc0 = rdtsc();
  memset(&binfo, 0, sizeof(binfo));

  BS->set_watchdog_timer(0, 0, 0, 0);
  putstr("\nOpenProninx UEFI loader\n");

  binfo.magic = FNU_BOOTINFO_MAGIC;
  binfo.version = FNU_BOOTINFO_VERSION;
  binfo.size = sizeof(binfo);
  binfo.firmware = FNU_FIRMWARE_UEFI;
  binfo.acpi_rsdp = acpi_rsdp();
  binfo.smbios = smbios_entry();
  hobs = hob_list(&hobslen);
  binfo.efi_system_table = (uint64)(uintptr_t)st;
  binfo.efi_runtime_services = (uint64)(uintptr_t)st->runtime_services;
  env_init(&env, binfo.env, sizeof(binfo.env));
  if (file_partition_guid(guid) == 0) {
    guid_text(guid, uuid);
    env_set(&env, "boot.partuuid", uuid);
  }
  if (file_bootdisk(&disk) == 0 && root_env(&disk, &env) == 0)
    disk_root = 1;
  if (st->con_in != 0)
    autoboot(&env, efi_getkey, efi_nap);

  if (file_mount() < 0)
    panic("cannot open the volume this loader came from");

  entry = load_kernel(!disk_root || env_true(&env, "ramdisk_load"));
  if (place.stage != place.dest) {
    mapsize = 0;
    BS->get_memory_map(&mapsize, 0, &key, &descsize, &descver);
    mapsize += MEMMAP_SLACK;
    if ((map = boot_alloc(mapsize)) != 0) {
      have = mapsize;
      if (!EFI_ERROR(BS->get_memory_map(
              &have, (struct efi_memory_descriptor *)(uintptr_t)map, &key,
              &descsize, &descver)) &&
          !destination_is_free((const void *)(uintptr_t)map, have, descsize,
                               place.dest, place.dest + place.length)) {
        report_conflict(place.dest, place.dest + place.length);
        panic("the kernel's address is not the firmware's to lend");
      }
      BS->free_pages(map, pages_for(mapsize));
    }
  }
  video_setup(&binfo);
  pci_roms(&binfo);
  cr3 = paging_setup();

  info = boot_alloc(sizeof(binfo));
  if (info == 0)
    panic("no low memory for the boot information");
  bi = (struct fnu_bootinfo *)(uintptr_t)info;
  memcpy(bi, &binfo, sizeof(binfo));

  if (place.stage != place.dest) {
    tramp = boot_alloc((uint64)(tramp_end - tramp_start));
    if (tramp == 0)
      panic("no room for the trampoline");
    memcpy((void *)(uintptr_t)tramp, tramp_start,
           (uint64)(tramp_end - tramp_start));
  }

  BS->get_memory_map(&mapsize, 0, &key, &descsize, &descver);
  mapsize += MEMMAP_SLACK;
  map = boot_alloc(mapsize);
  if (map == 0)
    panic("no room for the memory map");

  if (tramp != 0) {
    have = mapsize;
    if (EFI_ERROR(BS->get_memory_map(
            &have, (struct efi_memory_descriptor *)(uintptr_t)map, &key,
            &descsize, &descver)))
      panic("cannot read the memory map");
    if (!destination_is_free((const void *)(uintptr_t)map, have, descsize,
                             place.dest, place.dest + place.length)) {
      report_conflict(place.dest, place.dest + place.length);
      panic("the kernel's address is not the firmware's to lend");
    }
  }

  for (tries = 0; tries < 8; tries++) {
    have = mapsize;
    s = BS->get_memory_map(&have,
                           (struct efi_memory_descriptor *)(uintptr_t)map, &key,
                           &descsize, &descver);
    if (s == EFI_BUFFER_TOO_SMALL) {
      BS->free_pages(map, pages_for(mapsize));
      mapsize = have + MEMMAP_SLACK;
      map = boot_alloc(mapsize);
      if (map == 0)
        panic("no room for the memory map");
      continue;
    }
    if (EFI_ERROR(s))
      panic("cannot read the memory map");
    s = BS->exit_boot_services(image, key);
    if (!EFI_ERROR(s)) {
      __asm __volatile("cli");
      break;
    }
  }
  if (EFI_ERROR(s))
    panic("firmware will not release the machine");

  memcpy(bi->log, binfo.log, sizeof(bi->log));
  memory_map(bi, hobs, hobslen, (const void *)(uintptr_t)map, have, descsize);
  if (tramp == 0)
    enter_kernel(entry, cr3, info);

  args = (uint64 *)(uintptr_t)(tramp + (uint64)(tramp_args - tramp_start));
  args[0] = place.dest;
  args[1] = place.stage;
  args[2] = place.length;
  args[3] = entry;
  args[4] = cr3;
  args[5] = info;
  ((void (*)(void))(uintptr_t)tramp)();
  __unreachable();
}
