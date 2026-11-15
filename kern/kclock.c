#include "kclock.h"
#include "acpi.h"
#include "defs.h"
#include "proc.h"
#include "inc/abi.h"
#include "inc/syscall.h"
#include "param.h"
#include "x86.h"

uint mc146818_read(uint reg) {
  outb(IO_RTC, reg);
  return inb(IO_RTC + 1);
}

void mc146818_write(uint reg, uint datum) {
  outb(IO_RTC, reg);
  outb(IO_RTC + 1, datum);
}

#define RTC_REG_A 0x0A
#define RTC_REG_B 0x0B
#define RTC_UIP 0x80 /* update in progress */
#define RTC_SET 0x80 /* register B: updates held */
#define RTC_BIN 0x04 /* register B: set means binary, clear means BCD */
#define RTC_24H 0x02 /* register B: set means 24 hour */
#define RTC_SEC 0x00
#define RTC_MIN 0x02
#define RTC_HOUR 0x04
#define RTC_WDAY 0x06
#define RTC_MDAY 0x07
#define RTC_MON 0x08
#define RTC_YEAR 0x09

#define FEBRUARY 2
#define SECDAY (24 * 60 * 60)
#define EPOCH_YEAR 1970

static const int month_days[12] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};

static int leapyear(int year) {
  int rv = 0;

  if ((year & 3) == 0) {
    rv = 1;
    if ((year % 100) == 0) {
      rv = 0;
      if ((year % 400) == 0)
        rv = 1;
    }
  }
  return rv;
}

#define days_in_year(a) (leapyear(a) ? 366 : 365)
#define days_in_month(a) (month_days[(a) - 1])

// clock_ymdhms_to_secs
int64_t clock_ymdhms_to_secs(const struct fnu_rtctime *dt) {
  int64_t secs;
  int i, days = 0;

  for (i = EPOCH_YEAR; i < dt->year; i++)
    days += days_in_year(i);
  if (leapyear(dt->year) && dt->mon > FEBRUARY)
    days++;
  for (i = 1; i < dt->mon; i++)
    days += days_in_month(i);
  days += dt->mday - 1;

  secs = (int64_t)(((int64_t)days * 24 + dt->hour) * 60 + dt->min) * 60 +
         dt->sec;
  return secs;
}

// clock_secs_to_ymdhms
void clock_secs_to_ymdhms(int64_t secs, struct fnu_rtctime *dt) {
  int mthdays[12];
  int i, days, rsec;

  memmove(mthdays, month_days, sizeof(mthdays));
  if (secs < 0)
    secs = 0;
  days = (int)(secs / SECDAY);
  rsec = (int)(secs % SECDAY);

  dt->wday = (uint8)((days + 4) % 7);

  for (i = EPOCH_YEAR; days >= days_in_year(i); i++)
    days -= days_in_year(i);
  dt->year = (uint16)i;

  if (leapyear(i))
    mthdays[FEBRUARY - 1] = 29;
  for (i = 1; days >= mthdays[i - 1]; i++)
    days -= mthdays[i - 1];
  dt->mon = (uint8)i;
  dt->mday = (uint8)(days + 1);

  dt->hour = (uint8)(rsec / 3600);
  rsec %= 3600;
  dt->min = (uint8)(rsec / 60);
  dt->sec = (uint8)(rsec % 60);
}

static inline uint bcd_to_bin(uint bcd) {
  return (bcd & 0x0F) + ((bcd >> 4) & 0x0F) * 10;
}

static void rtc_wait_not_updating(void) {
  int spins;

  for (spins = 0; spins < 1000000; spins++)
    if ((mc146818_read(RTC_REG_A) & RTC_UIP) == 0)
      return;
}

// a reading, in order: time, century, register B. do not reorder
void rtc_raw_regs(uint8 *regs) {
  int idx = acpi_century_index();

  regs[0] = RTC_SEC;
  regs[1] = RTC_MIN;
  regs[2] = RTC_HOUR;
  regs[3] = RTC_WDAY;
  regs[4] = RTC_MDAY;
  regs[5] = RTC_MON;
  regs[6] = RTC_YEAR;
  regs[7] = idx != 0 ? (uint8)idx : RTC_YEAR;
  regs[8] = RTC_REG_B;
  regs[RTC_RAW] = 0xff;
}

static void rtc_raw(uint8 *raw) {
  uint8 regs[RTC_RAW + 1];
  int i;

  rtc_raw_regs(regs);
  rtc_wait_not_updating();
  for (i = 0; i < RTC_RAW; i++)
    raw[i] = (uint8)mc146818_read(regs[i]);
}

static int rtc_decode(const uint8 *in, struct fnu_rtctime *t) {
  uint raw[7], century = 0, b = in[8];
  int i, bcd, pm;

  for (i = 0; i < 7; i++)
    raw[i] = in[i];
  if (acpi_century_index() != 0)
    century = in[7];
  bcd = (b & RTC_BIN) == 0;
  pm = (raw[2] & 0x80) != 0;
  raw[2] &= 0x7F;

  t->sec = (uint8)(bcd ? bcd_to_bin(raw[0]) : raw[0]);
  t->min = (uint8)(bcd ? bcd_to_bin(raw[1]) : raw[1]);
  t->hour = (uint8)(bcd ? bcd_to_bin(raw[2]) : raw[2]);
  if ((b & RTC_24H) == 0) {
    if (t->hour == 12)
      t->hour = 0;
    if (pm)
      t->hour = (uint8)(t->hour + 12);
  }
  t->wday = (uint8)((bcd ? bcd_to_bin(raw[3]) : raw[3]) & 0x07);
  t->mday = (uint8)(bcd ? bcd_to_bin(raw[4]) : raw[4]);
  t->mon = (uint8)(bcd ? bcd_to_bin(raw[5]) : raw[5]);
  t->year = (uint16)(bcd ? bcd_to_bin(raw[6]) : raw[6]);
  if (century != 0 && century != 0xFF)
    t->year = (uint16)(t->year + (bcd ? bcd_to_bin(century) : century) * 100);
  else
    t->year = (uint16)(t->year + (t->year < 70 ? 2000 : 1900));

  if (t->mon < 1 || t->mon > 12 || t->mday < 1 || t->mday > 31 ||
      t->hour > 23 || t->min > 59 || t->sec > 60)
    return -1;
  if (t->wday > 6)
    t->wday = 0;
  return 0;
}

static int rtc_read(struct fnu_rtctime *t) {
  uint8 raw[RTC_RAW];

  rtc_raw(raw);
  return rtc_decode(raw, t);
}

// pm_trace lives in the clock; a value is an hour of 2000
// ???
#define TRACE_BASE 946684800LL // 2000-01-01 00:00 UTC
#define TRACE_YEAR 2000
#define TRACE_SPAN 8760

static int traced = -1;
static int clock_known, untrace_pending;

static int trace_of(const struct fnu_rtctime *t) {
  int64_t h;

  if (t->year != TRACE_YEAR)
    return -1;
  h = (clock_ymdhms_to_secs(t) - TRACE_BASE) / 3600 - 1;
  return h >= 0 && h < TRACE_SPAN ? (int)h : -1;
}

#define NSEC 1000000000LL
#define SLEW_PPM 5000

static struct mutex clock_lock;
static uint64_t clock_gen;
static int64_t boot_ns;    // wall less uptime
static int64_t slew_start;
static int64_t slew_total;

static int64_t ns_of(const struct fnu_timespec *t) {
  return t->sec * NSEC + t->nsec;
}

static void ts_of(int64_t ns, struct fnu_timespec *t) {
  t->sec = ns / NSEC;
  t->nsec = (int32_t)(ns % NSEC);
  if (t->nsec < 0) {
    t->sec--;
    t->nsec += (int32_t)NSEC;
  }
  t->pad = 0;
}

int64_t uptime_ns(void) {
  struct fnu_timespec up;

  uptime_now(&up);
  return ns_of(&up);
}

static int64_t slewed(int64_t up, int64_t start, int64_t total) {
  int64_t done = (up - start) / (1000000 / SLEW_PPM);

  if (total >= 0)
    return done < total ? done : total;
  return done < -total ? -done : total;
}

static void clock_begin(void) {
  __atomic_add_fetch(&clock_gen, 1, __ATOMIC_ACQ_REL);
}

static void clock_end(void) {
  __atomic_add_fetch(&clock_gen, 1, __ATOMIC_RELEASE);
}

static void clock_offset(int64_t boot) {
  mtx_enter(&clock_lock);
  clock_begin();
  boot_ns = boot;
  slew_total = 0;
  clock_end();
  mtx_leave(&clock_lock);
}

void clockinit(void) {
  struct fnu_rtctime t;

  mtx_init(&clock_lock, "clock");
  if (rtc_read(&t) < 0) {
    printf("CLOCK: the battery clock reads nothing usable\n");
    boot_ns = -uptime_ns();
    return;
  }
  if ((traced = trace_of(&t)) >= 0) {
    printf("CLOCK: the battery clock holds a pm_trace record, not the time\n");
    boot_ns = -uptime_ns();
    return;
  }
  clock_known = 1;
  boot_ns = clock_ymdhms_to_secs(&t) * NSEC - uptime_ns();
  printf("CLOCK: %04d-%02d-%02d %02d:%02d:%02d UTC from the battery clock\n",
          t.year, t.mon, t.mday, t.hour, t.min, t.sec);
}

void time_now(struct fnu_timespec *out) {
  int64_t up, boot, start, total;
  uint64_t g;

  do {
    while ((g = __atomic_load_n(&clock_gen, __ATOMIC_ACQUIRE)) & 1)
      ;
    boot = boot_ns;
    start = slew_start;
    total = slew_total;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
  } while (__atomic_load_n(&clock_gen, __ATOMIC_ACQUIRE) != g);
  up = uptime_ns();
  ts_of(up + boot + (total != 0 ? slewed(up, start, total) : 0), out);
}

int64_t time_second(void) {
  struct fnu_timespec now;

  time_now(&now);
  return now.sec;
}

static uint64_t boot_tsc;

void uptime_init(void) { boot_tsc = rdtsc(); }

static inline uint bin_to_bcd(uint bin) { return (bin / 10) << 4 | bin % 10; }

// the writes that set the clock; updates held around them
static void rtc_image(int64_t secs, uint8 *p) {
  struct fnu_rtctime t;
  uint b, idx, hour, n = 0;
  int bcd;

  clock_secs_to_ymdhms(secs, &t);
  b = mc146818_read(RTC_REG_B);
  bcd = (b & RTC_BIN) == 0;
  hour = t.hour;
  if ((b & RTC_24H) == 0)
    hour = (hour % 12 == 0 ? 12 : hour % 12);
  hour = bcd ? bin_to_bcd(hour) : hour;
  if ((b & RTC_24H) == 0 && t.hour >= 12)
    hour |= 0x80;
#define PUT(r, v) (p[n++] = (uint8)(r), p[n++] = (uint8)(v))
  PUT(RTC_REG_B, b | RTC_SET);
  PUT(RTC_SEC, bcd ? bin_to_bcd(t.sec) : t.sec);
  PUT(RTC_MIN, bcd ? bin_to_bcd(t.min) : t.min);
  PUT(RTC_HOUR, hour);
  PUT(RTC_WDAY, t.wday + 1u);
  PUT(RTC_MDAY, bcd ? bin_to_bcd(t.mday) : t.mday);
  PUT(RTC_MON, bcd ? bin_to_bcd(t.mon) : t.mon);
  PUT(RTC_YEAR, bcd ? bin_to_bcd(t.year % 100u) : t.year % 100u);
  if ((idx = (uint)acpi_century_index()) != 0)
    PUT(idx, bcd ? bin_to_bcd(t.year / 100u) : t.year / 100u);
  PUT(RTC_REG_B, b);
#undef PUT
  p[n] = 0xff;
}

static void rtc_write(int64_t secs) {
  uint8 p[RTC_IMAGE_MAX];
  int i;

  rtc_image(secs, p);
  for (i = 0; p[i] != 0xff; i += 2)
    mc146818_write(p[i], p[i + 1]);
}

void rtc_trace(uint value) { rtc_write(TRACE_BASE + ((int64_t)value + 1) * 3600); }

void rtc_trace_image(uint value, uint8 *image) {
  rtc_image(TRACE_BASE + ((int64_t)value + 1) * 3600, image);
}

int rtc_traced(void) { return traced; }

// ???
void rtc_untrace(void) {
  traced = -1;
  if (clock_known)
    rtc_write(time_second());
  else
    untrace_pending = 1;
}

// battery clock is dead, the last fs write will do
void clock_base(int64_t secs) {
  struct fnu_rtctime t;

  if (clock_known || secs <= 0)
    return;
  clock_offset(secs * NSEC - uptime_ns());
  clock_known = 1;
  clock_secs_to_ymdhms(secs, &t);
  printf("CLOCK: %04d-%02d-%02d %02d:%02d:%02d UTC from the root filesystem's last write; "
         "check the date\n",
         t.year, t.mon, t.mday, t.hour, t.min, t.sec);
  if (untrace_pending) {
    untrace_pending = 0;
    rtc_write(secs);
  }
}

// SLEEP
static struct fnu_timespec slept_uptime;
static int64_t slept_wall;
static uint8 woke_raw[RTC_RAW];
static int woke_taken;

void clock_suspend(void) {
  uptime_now(&slept_uptime);
  slept_wall = time_second();
  woke_taken = 0;
}

// S4 hour. ???
void clock_asleep_at(int64_t wall) { slept_wall = wall; }

// the clock before the trace stepped on it
void clock_woke(const uint8 *raw) {
  if (raw != 0)
    memmove(woke_raw, raw, RTC_RAW);
  else
    rtc_raw(woke_raw);
  woke_taken = 1;
}

void clock_resume(void) {
  struct fnu_rtctime t;
  int64_t now = slept_wall, gone = 0;
  uint64_t hz = delay_tsc_hz();

  // what the fuck is this hour
  if (rtc_read(&t) == 0 && trace_of(&t) < 0) {
    now = clock_ymdhms_to_secs(&t);
    gone = now > slept_wall ? now - slept_wall : 0;
  } else if (woke_taken && rtc_decode(woke_raw, &t) == 0 && t.year == TRACE_YEAR &&
             clock_ymdhms_to_secs(&t) >= TRACE_BASE + 3600) {
    gone = clock_ymdhms_to_secs(&t) - (TRACE_BASE + 3600);
    now = slept_wall + gone;
  }
  if (hz != 0)
    boot_tsc = rdtsc() - (uint64_t)(slept_uptime.sec + gone) * hz -
               (uint64_t)slept_uptime.nsec * hz / 1000000000u;
  clock_offset(now * NSEC - uptime_ns());
}

int sys_settime(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_timespec t;

  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if (copyin(p->p_p->pgdir, &t, (uintptr_t)uap[0], sizeof(t)) < 0)
    return FNU_ERR_FAULT;
  if (t.sec < 0 || t.nsec < 0 || t.nsec >= NSEC || t.sec > __INT64_MAX__ / NSEC - 1)
    return FNU_ERR_INVALID;
  clock_offset(ns_of(&t) - uptime_ns());
  clock_known = 1;
  untrace_pending = 0;
  rtc_write(t.sec);
  printf("CLOCK: set by pid %d (%s)\n", p->p_p->pid, p->p_p->name);
  return 0;
}

int sys_slewtime(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_timespec d, left;
  int64_t up, done, delta = 0;
  int error;

  if (uap[0] != 0) {
    if ((error = pledge_need(p, FNU_PLEDGE_SETTIME, SYS_slewtime)) != 0)
      return error;
    if (p->p_p->uid != 0)
      return FNU_ERR_NOTPERMITTED;
    if (copyin(p->p_p->pgdir, &d, (uintptr_t)uap[0], sizeof(d)) < 0)
      return FNU_ERR_FAULT;
    if (d.nsec < 0 || d.nsec >= NSEC || d.sec > __INT64_MAX__ / NSEC - 1 ||
        d.sec < -__INT64_MAX__ / NSEC + 1)
      return FNU_ERR_INVALID;
    delta = ns_of(&d);
  }
  mtx_enter(&clock_lock);
  up = uptime_ns();
  done = slew_total != 0 ? slewed(up, slew_start, slew_total) : 0;
  ts_of(slew_total - done, &left);
  if (uap[0] != 0) {
    clock_begin();
    boot_ns += done;
    slew_start = up;
    slew_total = delta;
    clock_end();
  }
  mtx_leave(&clock_lock);
  if (uap[1] != 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)uap[1], &left, sizeof(left)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

static void clock_grain(struct fnu_timespec *out) {
  uint64_t hz = delay_tsc_hz();

  out->sec = 0;
  out->nsec = hz != 0 ? (int32_t)((1000000000u + hz - 1) / hz)
                      : (int32_t)(1000000000 / HZ);
  out->pad = 0;
}

void uptime_now(struct fnu_timespec *out) {
  uint64_t hz = delay_tsc_hz();

  if (hz != 0) {
    uint64_t t = rdtsc() - boot_tsc;

    out->sec = (int64_t)(t / hz);
    out->nsec = (int32_t)((t % hz) * 1000000000u / hz);
  } else {
    out->sec = (int64_t)(ticks / HZ);
    out->nsec = (int32_t)((ticks % HZ) * (1000000000 / HZ));
  }
  out->pad = 0;
}

int sys_gettime(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_timespec now;
  uintptr_t dst;
  int which = FNU_CLOCK_WALL;

  dst = uap[0];
  which = (int)uap[1];
  if ((which & ~FNU_CLOCK_GRAIN) != FNU_CLOCK_UPTIME &&
      (which & ~FNU_CLOCK_GRAIN) != FNU_CLOCK_WALL)
    return FNU_ERR_INVALID;
  if (which & FNU_CLOCK_GRAIN)
    clock_grain(&now);
  else if (which == FNU_CLOCK_UPTIME)
    uptime_now(&now);
  else
    time_now(&now);
  if (copyout(p->p_p->pgdir, dst, &now, sizeof(now)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

int sys_rtctime(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uintptr_t dst;
  struct fnu_rtctime t;

  dst = uap[0];
  clock_secs_to_ymdhms(time_second(), &t);
  if (copyout(p->p_p->pgdir, dst, &t, sizeof(t)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}
