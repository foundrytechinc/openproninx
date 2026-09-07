#include "defs.h"
#include "inc/abi.h"
#include "memlayout.h"
#include "proc.h"
#include "storage/ahci.h"
#include "x86.h"

static void put4(char *d, uint32_t v) {
  d[0] = (char)v;
  d[1] = (char)(v >> 8);
  d[2] = (char)(v >> 16);
  d[3] = (char)(v >> 24);
}

// leaves 80000002..4 hold the brand string, 48 bytes, space padded
static void cpu_brand(char *out) {
  uint32_t a, b, c, d, leaf;
  int i;

  cpuid_count(0x80000000, 0, &a, &b, &c, &d);
  if (a < 0x80000004) {
    safestrcpy(out, "unknown", HW_BRAND_MAX);
    return;
  }
  for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
    cpuid_count(leaf, 0, &a, &b, &c, &d);
    i = (int)(leaf - 0x80000002) * 16;
    put4(out + i, a);
    put4(out + i + 4, b);
    put4(out + i + 8, c);
    put4(out + i + 12, d);
  }
  out[48] = 0;
  for (i = 47; i >= 0 && (out[i] == ' ' || out[i] == 0); i--)
    out[i] = 0;
  // intel pads the brand on the left. printed raw it shoves the field sideways
  for (i = 0; out[i] == ' '; i++)
    ;
  if (i > 0)
    memmove(out, out + i, (uint)(49 - i));
}

#define MSR_PLATFORM_INFO 0x0ce
#define MSR_TURBO_RATIO_LIMIT 0x1ad

static int near(uint64_t a, uint64_t b) {
  uint64_t d = a > b ? a - b : b - a;
  return b != 0 && d * 32 < b;
}

// CPUID 16h is the part quoting its own datasheet. before Skylake there is
// no such leaf: take the ratios from the MSRs and let the calibrated tsc
// pick the bus clock. FreeBSD reads leaf 16h the same way in x86/tsc.c.
static void cpu_clocks(const char *vendor, uint64_t *base, uint64_t *max) {
  uint64_t tsc = delay_tsc_hz(), bclk = 0;
  uint32_t a, b, c, d, high, ratio, turbo;

  *base = 0;
  *max = 0;
  if (strncmp(vendor, "GenuineIntel", 12) != 0)
    return;

  cpuid_count(0, 0, &high, &b, &c, &d);
  if (high >= 0x16) {
    cpuid_count(0x16, 0, &a, &b, &c, &d);
    *base = (uint64_t)(a & 0xffff) * 1000000;
    *max = (uint64_t)(b & 0xffff) * 1000000;
    if (*base != 0)
      return;
    *max = 0;
  }

  // leaf 0bh and MSR_PLATFORM_INFO both arrived with Nehalem
  if (high < 0x0b)
    return;
  ratio = (uint32_t)((rdmsr(MSR_PLATFORM_INFO) >> 8) & 0xff);
  if (ratio == 0)
    return;
  if (near((uint64_t)ratio * 100000000ULL, tsc))
    bclk = 100000000ULL;
  else if (near((uint64_t)ratio * 400000000ULL / 3, tsc))
    bclk = 400000000ULL / 3;
  else
    return;
  *base = (uint64_t)ratio * bclk;

  cpuid_count(6, 0, &a, &b, &c, &d);
  if ((a & 0x2) == 0)
    return;
  turbo = (uint32_t)(rdmsr(MSR_TURBO_RATIO_LIMIT) & 0xff);
  if (turbo > ratio)
    *max = (uint64_t)turbo * bclk;
}

int64_t sys_hwinfo(void) {
  struct hwinfo hw, *dst;
  uint32_t a, b, c, d;

  if (argptr(0, (char **)&dst, sizeof(*dst)) < 0)
    return -1;

  memset(&hw, 0, sizeof(hw));
  cpuid_count(0, 0, &a, &b, &c, &d);
  put4(hw.cpu_vendor, b);
  put4(hw.cpu_vendor + 4, d);
  put4(hw.cpu_vendor + 8, c);
  hw.cpu_vendor[12] = 0;
  cpu_brand(hw.cpu_brand);

  cpuid_count(1, 0, &a, &b, &c, &d);
  hw.cpu_stepping = a & 0xf;
  hw.cpu_model = (a >> 4) & 0xf;
  hw.cpu_family = (a >> 8) & 0xf;
  if (hw.cpu_family == 0xf)
    hw.cpu_family += (a >> 20) & 0xff;
  if (hw.cpu_family == 0x6 || hw.cpu_family == 0xf)
    hw.cpu_model += ((a >> 16) & 0xf) << 4;

  hw.cpu_count = (uint32_t)ncpu;
  hw.tsc_hz = delay_tsc_hz();
  cpu_clocks(hw.cpu_vendor, &hw.cpu_base_hz, &hw.cpu_max_hz);
  hw.pci_devices = (uint32_t)driver_device_count();
  hw.pci_total = (uint32_t)driver_pci_function_count();
  hw.fb_modeset = (uint32_t)(display_can_modeset() ? 1 : 0);
  hw.fb_source = framebuffer_mode_source_code();
  hw.fb_vram = framebuffer_available() ? framebuffer_vram() : 0;
  hw.ram_total = get_total_ram();
  hw.ram_free = get_free_ram();
  display_get_resolution(&hw.fb_width, &hw.fb_height, &hw.fb_bpp);
  hw.fb_columns = framebuffer_available() ? framebuffer_columns() : 80;
  hw.fb_rows = framebuffer_available() ? framebuffer_rows() : 25;

  if (copyout(myproc()->pgdir, (uintptr_t)dst, &hw, sizeof(hw)) < 0)
    return -1;
  return 0;
}

int64_t sys_diskinfo(void) {
  struct diskinfo *dst;
  struct diskinfo e;
  int capacity, n = 0, i;

  if (argptr(0, (char **)&dst, sizeof(*dst)) < 0 || argint(1, &capacity) < 0)
    return -1;
  if (capacity < 1)
    return -1;
  if (capacity > DISK_ENTRIES_MAX)
    capacity = DISK_ENTRIES_MAX;

  for (i = 0; n < capacity; i++) {
    memset(&e, 0, sizeof(e));
    if (ahci_disk_info(i, &e) < 0)
      break;
    if (copyout(myproc()->pgdir, (uintptr_t)(dst + n), &e, sizeof(e)) < 0)
      return -1;
    n++;
  }
  return n;
}
