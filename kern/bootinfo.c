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
  if (info.pciroms > FNU_PCIROM_MAX)
    info.pciroms = FNU_PCIROM_MAX;
  info.env[FNU_BOOTENV_MAX - 2] = 0;
  info.env[FNU_BOOTENV_MAX - 1] = 0;

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

const char *kgetenv(const char *name) {
  const char *p;
  size_t n = strlen(name);

  if (!valid)
    return 0;
  for (p = info.env; *p != 0; p += strlen(p) + 1)
    if (strncmp(p, name, n) == 0 && p[n] == '=')
      return p + n + 1;
  return 0;
}

// set unless it says no
int getenv_flag(const char *name) {
  const char *v = kgetenv(name);

  return v != 0 && strcmp(v, "NO") != 0 && strcmp(v, "no") != 0 && strcmp(v, "0") != 0;
}

int getenv_int(const char *name, int *data) {
  const char *v = kgetenv(name);
  int n = 0, neg;

  if (v == 0 || *v == 0)
    return 0;
  if ((neg = *v == '-') != 0)
    v++;
  if (*v < '0' || *v > '9')
    return 0;
  while (*v >= '0' && *v <= '9')
    n = n * 10 + (*v++ - '0');
  if (*v != 0)
    return 0;
  *data = neg ? -n : n;
  return 1;
}

int bootinfo_is_ram(uint64_t base, uint64_t end) {
  uint64_t at = base;
  uint i;
  int progress;

  if (!valid)
    return 0;
  while (at < end) {
    progress = 0;
    for (i = 0; i < info.memranges; i++) {
      if (info.mem[i].type != FNU_MEM_RAM || info.mem[i].base > at ||
          info.mem[i].base + info.mem[i].length <= at)
        continue;
      at = info.mem[i].base + info.mem[i].length;
      progress = 1;
      break;
    }
    if (!progress)
      return 0;
  }
  return 1;
}

void bootinfo_print(void) {
  if (!valid)
    printf("BOOT: no bootinfo from the loader\n");
}
