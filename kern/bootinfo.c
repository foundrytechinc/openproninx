#include "inc/bootinfo.h"
#include "defs.h"
#include "memlayout.h"

static struct fnu_bootinfo info;
static int valid;

void bootinfo_init(uint64_t phys) {
  const struct fnu_bootinfo *src;

  if (phys == 0 || phys >= 0x100000000ULL)
    return;
  src = (const struct fnu_bootinfo *)P2V((uintptr_t)phys);
  if (src->magic != FNU_BOOTINFO_MAGIC || src->version != FNU_BOOTINFO_VERSION)
    return;
  if (src->size != sizeof(info))
    return;
  memmove(&info, src, sizeof(info));

  // the direct map is built with a rising watermark, so keep them ordered
  for (uint i = 1; i < info.memranges; i++) {
    struct fnu_memrange t = info.mem[i];
    uint j = i;
    while (j > 0 && info.mem[j - 1].base > t.base) {
      info.mem[j] = info.mem[j - 1];
      j--;
    }
    info.mem[j] = t;
  }
  valid = 1;
}

const struct fnu_bootinfo *bootinfo(void) { return valid ? &info : 0; }

void bootinfo_print(void) {
  static const char *fw[] = {"none", "bios", "coreboot", "uefi"};
  uint i;

  if (!valid) {
    cprintf("BOOT: no bootinfo from the loader\n");
    return;
  }
  cprintf("BOOT: %s, %d memory ranges, rsdp 0x%x\n",
          info.firmware < NELEM(fw) ? fw[info.firmware] : "?", info.memranges,
          (uint64_t)info.acpi_rsdp);
  for (i = 0; i < info.memranges; i++)
    cprintf("  mem 0x%x + 0x%x type %d\n", info.mem[i].base, info.mem[i].length,
            info.mem[i].type);
  for (i = 0; i < info.modules; i++)
    cprintf("  module %s 0x%x + 0x%x\n", info.module[i].name,
            info.module[i].base, info.module[i].size);
}
