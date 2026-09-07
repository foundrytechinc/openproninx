#include "boot.h"

#define STAGE_ADDR 0x01000000u
#define STAGE_MAX 0x00400000u  // staging room for the kernel image
#define KERNEL_SLACK 0x00400000u // what kinit1 frees past the modules
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
static uint64 pml4[512] __attribute__((aligned(4096)));
static uint64 pdpt_lo[512] __attribute__((aligned(4096)));
static uint64 pdpt_hi[512] __attribute__((aligned(4096)));
static uint64 pdpt_dmap[512] __attribute__((aligned(4096)));
static uint64 pd[512] __attribute__((aligned(4096)));

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

void putstr(const char *s) {
  for (; *s; s++) {
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
    __asm__ volatile("hlt");
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

// identity map for the switch itself, the direct map and the kernel image.
// the kernel rebuilds all of this over the full memory map once it runs.
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

// bytes of ram following base inside its own range
static uint64 ram_room(uint64 base) {
  uint32 i;

  for (i = 0; i < bi.memranges; i++) {
    if (bi.mem[i].type != FNU_MEM_RAM)
      continue;
    if (base >= bi.mem[i].base && base < bi.mem[i].base + bi.mem[i].length)
      return bi.mem[i].base + bi.mem[i].length - base;
  }
  return 0;
}

static uint64 load_kernel(uint32 *end) {
  struct elfhdr eh;
  struct proghdr ph;
  uint32 i, top = 0;

  if (fat_load("/fnu/kernel", STAGE_ADDR, STAGE_MAX, 0) < 0)
    panic("no /fnu/kernel");

  flat_copy(&eh, STAGE_ADDR, sizeof(eh));
  if (eh.magic != 0x464c457fu)
    panic("kernel not elf");
  if (eh.phentsize != sizeof(ph))
    panic("bad phentsize");

  for (i = 0; i < eh.phnum; i++) {
    flat_copy(&ph, STAGE_ADDR + (uint32)eh.phoff + i * sizeof(ph), sizeof(ph));
    if (ph.type != 1)
      continue;
    if (ph.memsz < ph.filesz)
      panic("bad segment");
    flat_move((uint32)ph.paddr, STAGE_ADDR + (uint32)ph.off, (uint32)ph.filesz);
    if (ph.memsz > ph.filesz)
      flat_zero((uint32)ph.paddr + (uint32)ph.filesz,
                (uint32)(ph.memsz - ph.filesz));
    if ((uint32)ph.paddr + (uint32)ph.memsz > top)
      top = (uint32)ph.paddr + (uint32)ph.memsz;
  }
  if (top == 0)
    panic("kernel has no load segments");

  *end = (top + 0xfff) & ~0xfffu;
  return eh.entry;
}

void main(void) {
  uint64 esp_start, esp_count, entry;
  uint32 kend, rsize = 0, cr3;
  uint64 room;

  memset(&bi, 0, sizeof(bi));
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

  if (gpt_find(esp_type, &esp_start, &esp_count) < 0)
    panic("no esp");
  if (fat_mount(esp_start) < 0)
    panic("esp not fat32");

  if (ram_room(STAGE_ADDR) < STAGE_MAX)
    panic("not enough ram");

  entry = load_kernel(&kend);

  room = ram_room(kend);
  room = room > KERNEL_SLACK ? room - KERNEL_SLACK : 0;
  if (room > 0xf0000000ull)
    room = 0xf0000000ull;
  if (fat_load("/fnu/ramdisk", kend, (uint32)room, &rsize) == 0 &&
      rsize != 0) {
    bi.module[0].base = kend;
    bi.module[0].size = rsize;
    memcpy(bi.module[0].name, "ramdisk", 8);
    bi.modules = 1;
  }

  video_setup(&bi);

  cr3 = paging_setup();
  enter_long(entry, cr3, vtophys(&bi));
}
