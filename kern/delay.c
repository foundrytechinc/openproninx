// DELAY

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
#define PIT_CH2_LATCH 0x80

static uint64_t tsc_hz;
static int pit_runs;

#define MSR_TSC_ADJUST 0x3b
static int tsc_adjust; // CPUID.(7,0).EBX[1]

// the firmware moves TSC_ADJUST behind our back, блядь
// how far this cpu's TSC ran ahead
static int64_t adjust(int cpu) {
  int64_t v;

  if (!tsc_adjust || (v = (int64_t)rdmsr(MSR_TSC_ADJUST)) == 0)
    return 0;
  wrmsr(MSR_TSC_ADJUST, 0);
  printf("cpu%d: the firmware moved TSC_ADJUST to %ld, put back to 0\n", cpu, (long)v);
  return -v;
}

// after percpu_attach
int64_t tsc_adjust_check(void) { return adjust(cpuid()); }

static void tsc_adjust_init(void) {
  uint32_t a, b, c, d;

  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 7)
    return;
  cpuid_count(7, 0, &a, &b, &c, &d);
  tsc_adjust = (b & (1u << 1)) != 0;
  adjust(0);
}

static void ch2_start(uint16_t count) {
  outb(PIT_GATE, (inb(PIT_GATE) & ~PIT_SPKR) | PIT_GATE_ON);
  outb(PIT_CTRL, PIT_CH2_MODE0);
  outb(PIT_CH2, (uchar)count);
  outb(PIT_CH2, (uchar)(count >> 8));
}

static void ch2_stop(void) {
  outb(PIT_GATE, inb(PIT_GATE) & ~(PIT_GATE_ON | PIT_SPKR));
}

static uint16_t ch2_count(void) {
  uint16_t lo, hi;

  outb(PIT_CTRL, PIT_CH2_LATCH);
  lo = inb(PIT_CH2);
  hi = inb(PIT_CH2);
  return (uint16_t)(lo | (hi << 8));
}

static int ch2_counts_down(void) {
  uint16_t first;
  int i;

  ch2_start(0xffff);
  first = ch2_count();
  for (i = 0; i < 20000; i++) {
    if (ch2_count() != first) {
      ch2_stop();
      return 1;
    }
  }
  ch2_stop();
  return 0;
}

static int ch2_wait(uint16_t count) {
  uint32_t spins, limit = (uint32_t)count * 8 + 100000;

  for (spins = 0; spins < limit; spins++)
    if (inb(PIT_GATE) & PIT_OUT2)
      return 0;
  return -1;
}

// two TSC reads around one counter read: no SMI inside. hopefully
#define SMI_THRESHOLD 50000
#define REF_RETRIES 64

int tsc_bracket(uint32_t (*read)(void), uint64_t *tsc, uint32_t *value) {
  uint64_t t1, t2;
  int i;

  for (i = 0; i < REF_RETRIES; i++) {
    t1 = rdtsc();
    *value = read();
    t2 = rdtsc();
    if (t2 - t1 < SMI_THRESHOLD) {
      *tsc = t1 + (t2 - t1) / 2;
      return 0;
    }
  }
  return -1;
}

static uint32_t ch2_read(void) { return ch2_count(); }

// 50 ms of channel 2, median of three
#define MEASURE_TICKS (PIT_HZ / 20)

static uint64_t measure_once(void) {
  uint64_t t0, t1;
  uint32_t c0, c1, spins;

  ch2_start(0xffff);
  for (spins = 0; ch2_count() > 0xfff0; spins++)
    if (spins > 100000) {
      ch2_stop();
      return 0;
    }
  if (tsc_bracket(ch2_read, &t0, &c0) < 0) {
    ch2_stop();
    return 0;
  }
  for (spins = 0; c0 - ch2_count() < MEASURE_TICKS; spins++)
    if (spins > 10000000) {
      ch2_stop();
      return 0;
    }
  if (tsc_bracket(ch2_read, &t1, &c1) < 0 || c1 > c0 || c0 - c1 < MEASURE_TICKS || t1 <= t0) {
    ch2_stop();
    return 0;
  }
  ch2_stop();
  return (t1 - t0) * PIT_HZ / (c0 - c1);
}

static uint64_t measure_tsc(void) {
  uint64_t r[3], x;
  int i, j;

  for (i = 0; i < 3; i++)
    if ((r[i] = measure_once()) == 0)
      return 0;
  for (i = 0; i < 3; i++)
    for (j = i + 1; j < 3; j++)
      if (r[j] < r[i]) {
        x = r[i];
        r[i] = r[j];
        r[j] = x;
      }
  return r[1];
}

// leaf 15h exact, 16h within a percent
static uint64_t cpuid_tsc(int *exact) {
  uint32_t a, b, c, d, maxleaf;

  *exact = 0;
  cpuid_count(0, 0, &maxleaf, &b, &c, &d);
  if (maxleaf >= 0x15) {
    cpuid_count(0x15, 0, &a, &b, &c, &d);
    if (a != 0 && b != 0 && c != 0) {
      *exact = 1;
      return (uint64_t)c * b / a;
    }
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
    if (ch2_wait(chunk) < 0) {
      ch2_stop();
      pit_runs = 0;
      return;
    }
    ch2_stop();
    ticks -= chunk;
  }
}

static void blind_delay(uint us) {
  uint64_t i, n = (uint64_t)us * 100;

  for (i = 0; i < n; i++)
    __asm __volatile("pause");
}

void delay(uint us) {
  uint64_t target;

  if (tsc_hz == 0) {
    if (pit_runs)
      pit_delay(us);
    else
      blind_delay(us);
    return;
  }
  target = rdtsc() + (uint64_t)us * tsc_hz / 1000000;
  while ((int64_t)(rdtsc() - target) < 0)
    __asm __volatile("pause");
}

void delay_ms(uint ms) {
  while (ms-- != 0)
    delay(1000);
}

uint64_t delay_tsc_hz(void) { return tsc_hz; }

void delay_init(void) {
  uint32_t a, b, c, d;
  uint64_t reported;
  int exact;

  pit_runs = ch2_counts_down();

  cpuid_count(1, 0, &a, &b, &c, &d);
  if ((d & (1u << 4)) == 0) {
    if (pit_runs)
      printf("CLOCK: no tsc, delays run off the i8254\n");
    else
      printf("CLOCK: no tsc and no i8254, delays are a spin\n");
    return;
  }

  tsc_adjust_init();
  reported = cpuid_tsc(&exact);
  if (exact) {
    tsc_hz = reported;
    printf("CLOCK: tsc %d kHz from the crystal cpuid states\n", (uint)(tsc_hz / 1000));
    return;
  }
  if (!pit_runs) {
    tsc_hz = reported;
    if (tsc_hz == 0) {
      printf("CLOCK: i8254 is gated off and cpuid states no tsc frequency, "
              "delays are a spin\n");
      return;
    }
    printf("CLOCK: i8254 is gated off, tsc %d kHz from cpuid\n",
            (uint)(tsc_hz / 1000));
    return;
  }

  tsc_hz = measure_tsc();
  if (tsc_hz == 0) {
    tsc_hz = reported;
    if (tsc_hz == 0) {
      printf("CLOCK: tsc calibration failed, delays run off the i8254\n");
      return;
    }
    printf("CLOCK: no usable i8254, tsc %d kHz from cpuid\n",
            (uint)(tsc_hz / 1000));
    return;
  }

  printf("CLOCK: tsc %d kHz, calibrated against the i8254\n",
          (uint)(tsc_hz / 1000));
  if (reported != 0) {
    uint64_t lo = reported - reported / 50, hi = reported + reported / 50;
    if (tsc_hz < lo || tsc_hz > hi)
      printf("CLOCK: cpuid reports %d kHz, using the measurement\n",
              (uint)(reported / 1000));
  }
}
