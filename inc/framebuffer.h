#ifndef PRONINX_FRAMEBUFFER_H
#define PRONINX_FRAMEBUFFER_H

/* what the loader found. physical, 64 bit: uefi apertures sit above 4G. */
#define BOOT_FRAMEBUFFER_INFO_PHYS 0x900
#define BOOT_FRAMEBUFFER_MAGIC 0x46524246 /* "FBRF" */

#ifndef __ASSEMBLER__
#include "inc/types.h"

struct boot_framebuffer_info {
  uint32_t magic;
  uint64_t physical_base;
  uint16_t width;
  uint16_t height;
  uint16_t pitch;
  uint8_t bits_per_pixel;
  uint8_t reserved;
  uint8_t red_pos, red_size; // channel layout, for 15/16 bit packing
  uint8_t green_pos, green_size;
  uint8_t blue_pos, blue_size;
};
#endif

#endif
