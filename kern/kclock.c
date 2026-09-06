#include "kclock.h"
#include "defs.h"
#include "proc.h"
#include "inc/abi.h"
#include "x86.h"

uint mc146818_read(uint reg) {
  outb(IO_RTC, reg);
  return inb(IO_RTC + 1);
}

void mc146818_write(uint reg, uint datum) {
  outb(IO_RTC, reg);
  outb(IO_RTC + 1, datum);
}

/* ---- FNU userland: RTC wall-clock syscall (fnudate) -------------------- */

#define RTC_REG_A       0x0A
#define RTC_REG_B       0x0B
#define RTC_UIP         0x80  /* Update-In-Progress bit in Register A */
#define RTC_BCD         0x04  /* Data Mode bit in Register B: 0 = BCD, 1 = bin */
#define RTC_SEC         0x00
#define RTC_MIN         0x02
#define RTC_HOUR        0x04
#define RTC_WDAY        0x06
#define RTC_MDAY        0x07
#define RTC_MON         0x08
#define RTC_YEAR        0x09
#define RTC_CENTURY     0x32  /* Century byte on ACPI-aware hardware */

static inline uint bcd_to_bin(uint bcd) {
  return (bcd & 0x0F) + ((bcd >> 4) & 0x0F) * 10;
}

static void rtc_wait_not_updating(void) {
  int spins;
  for (spins = 0; spins < 1000000; spins++) {
    if ((mc146818_read(RTC_REG_A) & RTC_UIP) == 0)
      return;
  }
}

int64_t
sys_rtctime(void)
{
  uintptr_t dst_addr;
  struct rtctime t;
  uint raw[7], century;
  int bcd;
  struct proc *p;

  if (arg(0, (uint64_t*)&dst_addr) < 0)
    return -1;

  /* Spin until a refresh isn't in flight, then snapshot the 7 time bytes
   * atomically with a second UIP boundary check. */
  rtc_wait_not_updating();
  raw[0] = mc146818_read(RTC_SEC);
  raw[1] = mc146818_read(RTC_MIN);
  raw[2] = mc146818_read(RTC_HOUR);
  raw[3] = mc146818_read(RTC_WDAY);
  raw[4] = mc146818_read(RTC_MDAY);
  raw[5] = mc146818_read(RTC_MON);
  raw[6] = mc146818_read(RTC_YEAR);
  century = mc146818_read(RTC_CENTURY);
  rtc_wait_not_updating();

  /* If the Data-Mode bit of Status B is clear the values are packed BCD. */
  bcd = (mc146818_read(RTC_REG_B) & RTC_BCD) == 0;

  t.sec  = bcd ? bcd_to_bin(raw[0]) : raw[0];
  t.min  = bcd ? bcd_to_bin(raw[1]) : raw[1];
  t.hour = bcd ? bcd_to_bin(raw[2] & 0x7F) : (raw[2] & 0x7F);
  if ((raw[2] & 0x80) && t.hour < 12)  /* 12-hour PM bit */
    t.hour += 12;
  t.wday = bcd ? bcd_to_bin(raw[3] & 0x07) : (raw[3] & 0x07);
  t.mday = bcd ? bcd_to_bin(raw[4]) : raw[4];
  t.mon  = bcd ? bcd_to_bin(raw[5]) : raw[5];
  t.year = bcd ? bcd_to_bin(raw[6]) : raw[6];
  if (bcd && century < 0xFF)
    t.year += bcd_to_bin(century) * 100;
  else if (t.year < 200)
    t.year += 2000;  /* Reasonable default for QEMU / bare-metal */

  /* Clamp obviously-broken fields (some QEMU machines report zero CMOS). */
  if (t.mon  < 1 || t.mon  > 12)  t.mon  = 1;
  if (t.mday < 1 || t.mday > 31)  t.mday = 1;
  if (t.wday > 6)                 t.wday = 0;

  p = myproc();
  if (copyout(p->pgdir, (uintptr_t)dst_addr, &t, sizeof(t)) < 0)
    return -1;
  return 0;
}
