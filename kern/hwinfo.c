#include "defs.h"
#include "inc/abi.h"
#include "memlayout.h"
#include "proc.h"
#include "blockdev.h"
#include "x86.h"

static void put4(char *d, uint32_t v) {
  d[0] = (char)v;
  d[1] = (char)(v >> 8);
  d[2] = (char)(v >> 16);
  d[3] = (char)(v >> 24);
}

static void cpu_brand(char *out) {
  uint32_t a, b, c, d, leaf;
  int i;

  cpuid_count(0x80000000, 0, &a, &b, &c, &d);
  if (a < 0x80000004) {
    strlcpy(out, "unknown", FNU_HW_BRAND_MAX);
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

static uint64_t bus_clock; // the ratios' unit, 0 when not known

uint64_t cpu_bus_clock(void) { return bus_clock; }

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
    bus_clock = (uint64_t)(c & 0xffff) * 1000000;
    if (*base != 0)
      return;
    *max = 0;
  }

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
  bus_clock = bclk;
  *base = (uint64_t)ratio * bclk;

  cpuid_count(6, 0, &a, &b, &c, &d);
  if ((a & 0x2) == 0)
    return;
  turbo = (uint32_t)(rdmsr(MSR_TURBO_RATIO_LIMIT) & 0xff);
  if (turbo > ratio)
    *max = (uint64_t)turbo * bclk;
}

void hwinfo_fill(struct fnu_hwinfo *out) {
  struct fnu_hwinfo hw;
  uint32_t a, b, c, d;

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
  if (hw.cpu_family == 0x6 || hw.cpu_family == 0xf)
    hw.cpu_model += ((a >> 16) & 0xf) << 4;
  if (hw.cpu_family == 0xf)
    hw.cpu_family += (a >> 20) & 0xff;

  hw.cpu_count = (uint32_t)ncpus;
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
  *out = hw;
}

int sys_hwinfo(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_hwinfo hw;

  hwinfo_fill(&hw);
  if (copyout(p->p_p->pgdir, (uintptr_t)uap[0], &hw, sizeof(hw)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

int sys_diskinfo(struct proc *p, const register_t *uap, register_t *retval) {
  struct fnu_diskinfo e;
  uint64_t dst;
  int capacity, n = 0, i;

  dst = uap[0];
  capacity = (int)uap[1];
  if (capacity < 0)
    return FNU_ERR_INVALID;

  for (i = 0;; i++) {
    uint dev;

    if (blockdev_nth_disk((uint)i, &dev) < 0)
      break;
    if (blockdev_ident(dev, &e) < 0)
      continue;
    if (n < capacity &&
        copyout(p->p_p->pgdir, (uintptr_t)(dst + n * sizeof(e)), &e, sizeof(e)) < 0)
      return FNU_ERR_FAULT;
    n++;
  }
  *retval = capacity == 0 || n < capacity ? n : capacity;
  return 0;
}
