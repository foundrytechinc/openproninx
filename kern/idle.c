// IDLE: deepest MWAIT if ARAT and invariant TSC, else hlt

#include "defs.h"
#include "param.h"
#include "proc.h"
#include "x86.h"

static int hint = -2;

static int deepest(void) {
  uint32_t a, b, c, d, max;
  int n;

  cpuid_count(0, 0, &max, &b, &c, &d);
  if (max < 6)
    return -1;
  cpuid_count(1, 0, &a, &b, &c, &d);
  if (!(c & (1u << 3)))
    return -1;
  cpuid_count(6, 0, &a, &b, &c, &d);
  if (!(a & (1u << 2)))
    return -1;
  cpuid_count(0x80000000, 0, &max, &b, &c, &d);
  if (max < 0x80000007)
    return -1;
  cpuid_count(0x80000007, 0, &a, &b, &c, &d);
  if (!(d & (1u << 8)))
    return -1;
  cpuid_count(5, 0, &a, &b, &c, &d);
  if ((c & 3) != 3)
    return -1;
  for (n = 7; n >= 1; n--)
    if ((d >> (4 * n)) & 0xf)
      return (n - 1) << 4 | (int)(((d >> (4 * n)) & 0xf) - 1);
  return -1;
}

// interrupts off on entry, on when it returns
void cpu_idle(volatile int *watch) {
  if (hint == -2)
    hint = deepest();
  if (hint < 0) {
    __asm __volatile("sti; hlt");
    return;
  }
  __asm __volatile("monitor" ::"a"(watch), "c"(0), "d"(0));
  if (*watch) {
    __asm __volatile("sti");
    return;
  }
  __asm __volatile("sti; mwait" ::"a"(hint), "c"(1));
}

static int idle_text(void *arg __unused, char *out, size_t max) {
  if (hint == -2)
    hint = deepest();
  return hint < 0 ? snprintf(out, max, "hlt") : snprintf(out, max, "mwait 0x%x", hint);
}

void idle_sysctl(void) { sysctl_add_string_fn("hw.cpu.idle", idle_text, 0, 0); }
