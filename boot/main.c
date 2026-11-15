#include "boot.h"

#define LOW_4G 0x100000000ull
#define FLOOR 0x100000ull
#define DMAP_PML4 256
#define BOOT_DMAP_GB 4
#define PTE_P 0x001
#define PTE_W 0x002
#define PTE_PS 0x080

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

static struct fnu_bootinfo bi;
static uint64 pml4[512] __aligned(4096);
static uint64 pdpt_lo[512] __aligned(4096);
static uint64 pdpt_hi[512] __aligned(4096);
static uint64 pdpt_dmap[512] __aligned(4096);
static uint64 pd[512] __aligned(4096);

static const uint8 esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8,
                                   0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0,
                                   0xc9, 0x3e, 0xc9, 0x3b};

void *memcpy(void *d, const void *s, uint32 n) {
  uint8 *a = d;
  const uint8 *b = s;
  while (n--)
    *a++ = *b++;
  return d;
}

void *memset(void *d, int c, uint32 n) {
  uint8 *a = d;
  while (n--)
    *a++ = (uint8)c;
  return d;
}

int memcmp(const void *a, const void *b, uint32 n) {
  const uint8 *x = a, *y = b;
  while (n--) {
    if (*x != *y)
      return *x - *y;
    x++;
    y++;
  }
  return 0;
}

static uint64 tsc0;
static uint32 logged;
static int at_line_start = 1;

static uint64 rdtsc(void) {
  uint32 lo, hi;

  __asm __volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64)hi << 32) | lo;
}

static void putdec(char *out, uint32 v) {
  char num[24];
  int i = (int)sizeof(num) - 1, n = 0;

  num[i] = 0;
  do {
    num[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (num[i])
    out[n++] = num[i++];
  out[n] = 0;
}

// the loader's words for the kernel. stamps in 2^20 tsc ticks, don't ask
static void logc(char c) {
  char num[24];
  uint32 i;

  if (at_line_start) {
    num[0] = '+';
    putdec(num + 1, (uint32)((rdtsc() - tsc0) >> 20));
    for (i = 0; num[i] && logged < FNU_BOOTLOG_MAX - 2; i++)
      bi.log[logged++] = num[i];
    if (logged < FNU_BOOTLOG_MAX - 2)
      bi.log[logged++] = ' ';
    at_line_start = 0;
  }
  if (logged < FNU_BOOTLOG_MAX - 1)
    bi.log[logged++] = c;
  at_line_start = c == '\n';
}

void putstr(const char *s) {
  for (; *s; s++) {
    logc(*s);
    if (*s == '\n')
      bios_putc('\r');
    bios_putc(*s);
  }
}

void panic(const char *s) {
  putstr("\nboot: ");
  putstr(s);
  putstr("\n");
  for (;;)
    __asm __volatile("hlt");
}

void e820_map(struct fnu_bootinfo *out) {
  uint32 cont = 0;
  struct {
    uint64 base;
    uint64 length;
    uint32 type;
    uint32 attr;
  } e;

  do {
    memset(&e, 0, sizeof(e));
    if (bios_e820(&cont, &e) < 0)
      break;
    if (e.length != 0 && out->memranges < FNU_MEM_MAX) {
      out->mem[out->memranges].base = e.base;
      out->mem[out->memranges].length = e.length;
      out->mem[out->memranges].type =
          (e.type >= FNU_MEM_RAM && e.type <= FNU_MEM_UNUSABLE)
              ? e.type
              : FNU_MEM_RESERVED;
      out->memranges++;
    }
  } while (cont != 0);
}

static int rsdp_at(uint32 linear) {
  uint8 buf[20];
  uint32 i, sum = 0;

  flat_copy(buf, linear, sizeof(buf));
  if (memcmp(buf, "RSD PTR ", 8) != 0)
    return 0;
  for (i = 0; i < sizeof(buf); i++)
    sum += buf[i];
  return (sum & 0xff) == 0;
}

uint64 acpi_scan(void) {
  uint16 ebda;
  uint32 a;

  flat_copy(&ebda, 0x40e, sizeof(ebda));
  if (ebda != 0) {
    uint32 base = (uint32)ebda << 4;
    for (a = base; a < base + 1024; a += 16)
      if (rsdp_at(a))
        return a;
  }
  for (a = 0xe0000; a < 0x100000; a += 16)
    if (rsdp_at(a))
      return a;
  return 0;
}

static uint64 smbios_scan(void) {
  uint8 buf[32];
  uint32 a, i, sum, len, v3 = 0;

  for (a = 0xf0000; a < 0x100000; a += 16) {
    flat_copy(buf, a, sizeof(buf));
    if (memcmp(buf, "_SM_", 4) == 0 && buf[5] >= 0x1e)
      len = buf[5];
    else if (memcmp(buf, "_SM3_", 5) == 0 && buf[6] >= 0x18)
      len = buf[6];
    else
      continue;
    if (len > sizeof(buf))
      continue;
    for (sum = 0, i = 0; i < len; i++)
      sum += buf[i];
    if ((sum & 0xff) != 0)
      continue;
    if (buf[3] == '_')
      return a;
    if (v3 == 0)
      v3 = a;
  }
  return v3;
}

static int has_1g_pages(void) {
  uint32 a, b, c, d;

  __asm __volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x80000000u));
  if (a < 0x80000001u)
    return 0;
  __asm __volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(0x80000001u));
  return (d & (1u << 26)) != 0;
}

static uint32 paging_setup(void) {
  uint32 i;

  memset(pml4, 0, sizeof(pml4));
  memset(pdpt_lo, 0, sizeof(pdpt_lo));
  memset(pdpt_dmap, 0, sizeof(pdpt_dmap));
  memset(pdpt_hi, 0, sizeof(pdpt_hi));

  pml4[0] = vtophys(pdpt_lo) | PTE_P | PTE_W;
  pml4[DMAP_PML4] = vtophys(pdpt_dmap) | PTE_P | PTE_W;
  pml4[511] = vtophys(pdpt_hi) | PTE_P | PTE_W;

  if (has_1g_pages()) {
    for (i = 0; i < BOOT_DMAP_GB; i++) {
      pdpt_lo[i] = ((uint64)i << 30) | PTE_P | PTE_W | PTE_PS;
      pdpt_dmap[i] = ((uint64)i << 30) | PTE_P | PTE_W | PTE_PS;
    }
    pdpt_hi[510] = PTE_P | PTE_W | PTE_PS;
  } else {
    for (i = 0; i < 512; i++)
      pd[i] = ((uint64)i << 21) | PTE_P | PTE_W | PTE_PS;
    pdpt_lo[0] = vtophys(pd) | PTE_P | PTE_W;
    pdpt_dmap[0] = vtophys(pd) | PTE_P | PTE_W;
    pdpt_hi[510] = vtophys(pd) | PTE_P | PTE_W;
  }
  return vtophys(pml4);
}

// all of [lo, hi) is E820 RAM
static int ram_covers(uint64 lo, uint64 hi) {
  uint32 i;
  int progress;

  while (lo < hi) {
    progress = 0;
    for (i = 0; i < bi.memranges; i++)
      if (bi.mem[i].type == FNU_MEM_RAM && bi.mem[i].base <= lo &&
          bi.mem[i].base + bi.mem[i].length > lo) {
        lo = bi.mem[i].base + bi.mem[i].length;
        progress = 1;
      }
    if (!progress)
      return 0;
  }
  return 1;
}

// highest E820 ram run under 4 GB that dodges avoid[], or 0
static uint64 place_high(uint64 size, uint64 (*avoid)[2], int navoid) {
  uint64 best = 0, top, base;
  uint32 i;
  int j, moved;

  for (i = 0; i < bi.memranges; i++) {
    if (bi.mem[i].type != FNU_MEM_RAM)
      continue;
    top = bi.mem[i].base + bi.mem[i].length;
    if (top > LOW_4G)
      top = LOW_4G;
    for (;;) {
      top &= ~0xfffull;
      if (top < size || (base = top - size) < bi.mem[i].base || base < FLOOR)
        break;
      for (moved = 0, j = 0; j < navoid; j++)
        if (base < avoid[j][1] && avoid[j][0] < top) {
          top = avoid[j][0];
          moved = 1;
          break;
        }
      if (!moved) {
        if (base > best)
          best = base;
        break;
      }
    }
  }
  return best;
}

static uint64 stage, stage_len; // where the kernel file waits for its place

// file up high, and where its segments land
static uint64 read_kernel(uint64 *lo, uint64 *hi, uint32 *fsize) {
  struct elfhdr eh;
  struct proghdr ph;
  uint32 i, need;

  *lo = ~0ull;
  *hi = 0;
  if (fat_file_size("/fnu/kernel", &need) < 0)
    panic("no /fnu/kernel");
  stage_len = need;
  if ((stage = place_high(stage_len, 0, 0)) == 0)
    panic("no memory to read the kernel into");
  if (fat_load("/fnu/kernel", (uint32)stage, need, fsize) < 0)
    panic("cannot read /fnu/kernel");

  flat_copy(&eh, (uint32)stage, sizeof(eh));
  if (eh.magic != 0x464c457fu)
    panic("kernel not elf");
  if (eh.phentsize != sizeof(ph))
    panic("bad phentsize");

  for (i = 0; i < eh.phnum; i++) {
    flat_copy(&ph, (uint32)stage + (uint32)eh.phoff + i * sizeof(ph), sizeof(ph));
    if (ph.type != 1)
      continue;
    if (ph.memsz < ph.filesz)
      panic("bad segment");
    if (ph.paddr < *lo)
      *lo = ph.paddr;
    if (ph.paddr + ph.memsz > *hi)
      *hi = ph.paddr + ph.memsz;
  }
  if (*hi == 0)
    panic("kernel has no load segments");
  *lo &= ~0xfffull;
  *hi = (*hi + 0xfff) & ~0xfffull;
  if (!ram_covers(*lo, *hi))
    panic("the kernel's place is not free memory");
  if (*lo < stage + stage_len && stage < *hi)
    panic("the kernel's place overlaps where it was read");
  return eh.entry;
}

// segments to their places, after the last BIOS call
static void place_kernel(void) {
  struct elfhdr eh;
  struct proghdr ph;
  uint32 i;

  flat_copy(&eh, (uint32)stage, sizeof(eh));
  for (i = 0; i < eh.phnum; i++) {
    flat_copy(&ph, (uint32)stage + (uint32)eh.phoff + i * sizeof(ph), sizeof(ph));
    if (ph.type != 1)
      continue;
    flat_move((uint32)ph.paddr, (uint32)stage + (uint32)ph.off, (uint32)ph.filesz);
    if (ph.memsz > ph.filesz)
      flat_zero((uint32)ph.paddr + (uint32)ph.filesz, (uint32)(ph.memsz - ph.filesz));
  }
}

static void bios_nap(void) { bios_wait(100000); }

void main(void) {
  struct bootdisk disk;
  struct bootenv env;
  struct gpt_part esp;
  char uuid[37];
  uint64 entry;
  uint64 klo, khi, rbase, avoid[2][2];
  uint32 ksize = 0, rsize = 0, rneed, cr3;
  int disk_root;
  char num[24];

  tsc0 = rdtsc();
  memset(&bi, 0, sizeof(bi));
  logged = 0;
  bi.magic = FNU_BOOTINFO_MAGIC;
  bi.version = FNU_BOOTINFO_VERSION;
  bi.size = sizeof(bi);
  bi.boot_drive = (uint32)boot_drive;
  bi.boot_partition_lba = boot_part_lba;

  if (!bios_disk_extcheck(boot_drive))
    panic("no int13 extensions");

  if (coreboot_probe(&bi)) {
    bi.firmware = FNU_FIRMWARE_COREBOOT;
  } else {
    bi.firmware = FNU_FIRMWARE_BIOS;
    e820_map(&bi);
  }
  if (bi.memranges == 0)
    panic("no memory map");
  if (bi.acpi_rsdp == 0)
    bi.acpi_rsdp = acpi_scan();
  bi.smbios = smbios_scan();

  bootdisk_open(&disk);
  if (gpt_scan(&disk, esp_type, 0, &esp) != 0)
    panic("no esp");
  if (fat_mount(esp.start) < 0)
    panic("esp not fat32");
  env_init(&env, bi.env, sizeof(bi.env));
  guid_text(esp.guid, uuid);
  env_set(&env, "boot.partuuid", uuid);
  disk_root = root_env(&disk, &env) == 0;
  autoboot(&env, bios_getkey, bios_nap);

  entry = read_kernel(&klo, &khi, &ksize);
  putdec(num, ksize >> 10);
  putstr("boot: kernel ");
  putstr(num);
  putstr(" KB\n");

  // the live root only; a root on disk needs none
  if ((!disk_root || env_true(&env, "ramdisk_load")) &&
      fat_file_size("/fnu/ramdisk", &rneed) == 0 && rneed != 0) {
    avoid[0][0] = stage;
    avoid[0][1] = stage + stage_len;
    avoid[1][0] = klo;
    avoid[1][1] = khi;
    if ((rbase = place_high(rneed, avoid, 2)) == 0)
      putstr("boot: no room for the ramdisk\n");
    else if (fat_load("/fnu/ramdisk", (uint32)rbase, rneed, &rsize) == 0 && rsize != 0) {
      bi.module[0].base = rbase;
      bi.module[0].size = rsize;
      memcpy(bi.module[0].name, "ramdisk", 8);
      bi.modules = 1;
      putdec(num, rsize >> 10);
      putstr("boot: ramdisk ");
      putstr(num);
      putstr(" KB\n");
    }
  }

  video_setup(&bi);

  // no BIOS from here on
  usb_handoff();
  place_kernel();
  cr3 = paging_setup();
  enter_long(entry, cr3, vtophys(&bi));
}
