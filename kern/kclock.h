#ifndef PRONINX_X86_64_KCLOCK_H
#define PRONINX_X86_64_KCLOCK_H

#include "inc/types.h"

#define IO_RTC 0x070

#define MC_NVRAM_START 0xe
#define MC_NVRAM_SIZE 50

#define NVRAM_BASELO (MC_NVRAM_START + 7)
#define NVRAM_BASEHI (MC_NVRAM_START + 8)

#define NVRAM_EXTLO (MC_NVRAM_START + 9)
#define NVRAM_EXTHI (MC_NVRAM_START + 10)

#define NVRAM_EXT16LO (MC_NVRAM_START + 38)
#define NVRAM_EXT16HI (MC_NVRAM_START + 39)

uint mc146818_read(uint reg);
void mc146818_write(uint reg, uint datum);

// reg/value pairs, 0xff ends
#define RTC_IMAGE_MAX 24
// the registers a reading takes, and the 0xff after them
#define RTC_RAW 9

void rtc_trace(uint value);
void rtc_trace_image(uint value, uint8 *image);
int rtc_traced(void);
void rtc_raw_regs(uint8 *regs);
void clock_woke(const uint8 *raw);
void clock_asleep_at(int64_t wall);
void rtc_untrace(void);
void clock_base(int64_t secs);

#endif /* PRONINX_X86_64_KCLOCK_H */
