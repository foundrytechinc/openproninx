//
// DELAY
//

#include "defs.h"
#include "x86.h"

#define PIT_HZ 1193182
#define PIT_CTRL 0x43
#define PIT_CH2 0x42
#define PIT_GATE 0x61
#define PIT_GATE_ON 0x01
#define PIT_SPKR 0x02
#define PIT_OUT2 0x20
#define PIT_CH2_MODE0 0xb0

static uint64_t tsc_hz;

// channel 2. channel 0 belongs to the interrupt timer.
static void ch2_start(uint16_t count) {
  outb(PIT_GATE, (inb(PIT_GATE) & ~PIT_SPKR) | PIT_GATE_ON);
  outb(PIT_CTRL, PIT_CH2_MODE0);
  outb(PIT_CH2, (uchar)count);
  outb(PIT_CH2, (uchar)(count >> 8));
}

static void ch2_stop(void) {
  outb(PIT_GATE, inb(PIT_GATE) & ~(PIT_GATE_ON | PIT_SPKR));
}

// mode 0 raises OUT at terminal count. bounded, boards do lack a pit.
static int ch2_wait(void) {
  uint32_t spins;

  for (spins = 0; spins < 200000000; spins++)
    if (inb(PIT_GATE) & PIT_OUT2)
      return 0;
  return -1;
}

// shortest of three 10 ms gates
static uint64_t measure_tsc(void) {
  const uint16_t count = PIT_HZ / 100;
  uint64_t t0, t1, best = 0;
  int i;

  for (i = 0; i < 3; i++) {
    ch2_start(count);
    t0 = rdtsc();
    if (ch2_wait() < 0) {
      ch2_stop();
      return 0;
    }
    t1 = rdtsc();
    ch2_stop();
    if (t1 <= t0)
      return 0;
    if (best == 0 || t1 - t0 < best)
      best = t1 - t0;
  }
  return best * PIT_HZ / count;
}

// leaf 15h and 16h. a cross check, never the answer.
static uint64_t cpuid_tsc(void) {
  uint32_t a, b, c, d, maxleaf;

  cpuid_count(0, 0, &maxleaf, &b, &c, &d);
  if (maxleaf >= 0x15) {
    cpuid_count(0x15, 0, &a, &b, &c, &d);
    if (a != 0 && b != 0 && c != 0)
      return (uint64_t)c * b / a;
  }
  if (maxleaf >= 0x16) {
    cpuid_count(0x16, 0, &a, &b, &c, &d);
    if ((a & 0xffff) != 0)
      return (uint64_t)(a & 0xffff) * 1000000;
  }
  return 0;
}

static void pit_delay(uint us) {
  uint64_t ticks = ((uint64_t)us * PIT_HZ + 999999) / 1000000;
  uint16_t chunk;

  while (ticks != 0) {
    chunk = ticks > 0xffff ? 0xffff : (uint16_t)ticks;
    ch2_start(chunk);
    if (ch2_wait() < 0) {
      ch2_stop();
      return;
    }
    ch2_stop();
    ticks -= chunk;
  }
}

void delay(uint us) {
  uint64_t target;

  if (tsc_hz == 0) {
    pit_delay(us);
    return;
  }
  target = rdtsc() + (uint64_t)us * tsc_hz / 1000000;
  while ((int64_t)(rdtsc() - target) < 0)
    __asm__ volatile("pause");
}

void delay_ms(uint ms) {
  while (ms-- != 0)
    delay(1000);
}

uint64_t delay_tsc_hz(void) { return tsc_hz; }

void delay_init(void) {
  uint32_t a, b, c, d;
  uint64_t reported;

  cpuid_count(1, 0, &a, &b, &c, &d);
  if ((d & (1u << 4)) == 0) {
    cprintf("CLOCK: no tsc, delays run off the i8254\n");
    return;
  }

  tsc_hz = measure_tsc();
  if (tsc_hz == 0) {
    tsc_hz = cpuid_tsc();
    if (tsc_hz == 0) {
      cprintf("CLOCK: tsc calibration failed, delays run off the i8254\n");
      return;
    }
    cprintf("CLOCK: no usable i8254, tsc %d kHz from cpuid\n",
            (uint)(tsc_hz / 1000));
    return;
  }

  reported = cpuid_tsc();
  cprintf("CLOCK: tsc %d kHz, calibrated against the i8254\n",
          (uint)(tsc_hz / 1000));
  if (reported != 0) {
    uint64_t lo = reported - reported / 50, hi = reported + reported / 50;
    if (tsc_hz < lo || tsc_hz > hi)
      cprintf("CLOCK: cpuid reports %d kHz, using the measurement\n",
              (uint)(reported / 1000));
  }
}
