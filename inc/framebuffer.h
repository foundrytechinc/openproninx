#ifndef PRONINX_FRAMEBUFFER_H
#define PRONINX_FRAMEBUFFER_H

/*
 * The BIOS boot stage places this structure below 1 MiB.  It remains mapped
 * at P2V(BOOT_FRAMEBUFFER_INFO_PHYS) after the kernel installs its page
 * tables.  All fields are physical addresses or pixel dimensions.
 */
#define BOOT_FRAMEBUFFER_INFO_PHYS 0x900
#define BOOT_FRAMEBUFFER_MAGIC 0x46524246 /* "FBRF" */

/* Build-time upper bound for the VBE mode selected by the BIOS bootloader. */
#ifndef VBE_MAX_WIDTH
#define VBE_MAX_WIDTH 1920
#endif
#ifndef VBE_MAX_HEIGHT
#define VBE_MAX_HEIGHT 1080
#endif

/* Standard Bochs/QEMU VBE RGB modes, used when firmware mode enumeration
 * is incomplete.  The selected mode never exceeds the requested bounds. */
#if VBE_MAX_WIDTH >= 1600 && VBE_MAX_HEIGHT >= 1200
#define VBE_FALLBACK_MODE 0x145 /* 1280x1024x32 */
#elif VBE_MAX_WIDTH >= 1280 && VBE_MAX_HEIGHT >= 1024
#define VBE_FALLBACK_MODE 0x145 /* 1280x1024x32 */
#elif VBE_MAX_WIDTH >= 1024 && VBE_MAX_HEIGHT >= 768
#define VBE_FALLBACK_MODE 0x143 /* 1024x768x32 */
#elif VBE_MAX_WIDTH >= 800 && VBE_MAX_HEIGHT >= 600
#define VBE_FALLBACK_MODE 0x140 /* 800x600x32 */
#else
#define VBE_FALLBACK_MODE 0x141 /* 640x480x32 */
#endif

#ifndef __ASSEMBLER__
struct boot_framebuffer_info {
  uint32_t magic;
  uint32_t physical_base;
  uint16_t width;
  uint16_t height;
  uint16_t pitch;
  uint8_t bits_per_pixel;
  uint8_t reserved;
  uint32_t font_physical_base;
};
#endif

#endif
