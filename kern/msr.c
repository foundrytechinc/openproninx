
#include "defs.h"
#include "proc.h"
#include "x86.h"

int msr_read_try(uint32_t msr, uint64_t *val, uintptr_t *onfault);
int msr_write_try(uint32_t msr, uint64_t val, uintptr_t *onfault);

int rdmsr_safe(uint32_t msr, uint64_t *val) {
  int err;

  spinlock_enter();
  err = msr_read_try(msr, val, &curcpu()->onfault);
  spinlock_exit();
  return err;
}

int wrmsr_safe(uint32_t msr, uint64_t val) {
  int err;

  spinlock_enter();
  err = msr_write_try(msr, val, &curcpu()->onfault);
  spinlock_exit();
  return err;
}

// SMIs since reset (Nehalem on); -1 without the counter
#define MSR_SMI_COUNT 0x34

int64_t smi_count(void) {
  uint32_t a, b, c, d, model;
  uint64_t v;

  cpuid_count(0, 0, &a, &b, &c, &d);
  if (b != 0x756e6547 || d != 0x49656e69 || c != 0x6c65746e)
    return -1;
  cpuid_count(1, 0, &a, &b, &c, &d);
  model = ((a >> 4) & 0xf) | ((a >> 12) & 0xf0);
  if (((a >> 8) & 0xf) != 6 || model < 0x1a)
    return -1;
  if (rdmsr_safe(MSR_SMI_COUNT, &v) != 0)
    return -1;
  return (int64_t)(v & 0xffffffff);
}
