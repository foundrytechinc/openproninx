#include "boot.h"

// The size to ask for when the monitor will not say. VBE_WIDTH/VBE_HEIGHT
// override the monitor outright, for a panel that reports nonsense.
#ifndef VBE_FALLBACK_WIDTH
#define VBE_FALLBACK_WIDTH 1024
#endif
#ifndef VBE_FALLBACK_HEIGHT
#define VBE_FALLBACK_HEIGHT 768
#endif

#define VBE_OK 0x004f

static uint16 want_w = VBE_FALLBACK_WIDTH;
static uint16 want_h = VBE_FALLBACK_HEIGHT;

struct vbeinfo {
  char sig[4];
  uint16 version;
  uint32 oem_string;
  uint32 caps;
  uint32 mode_ptr;
  uint16 total_memory;
  uint8 rest[492];
} __attribute__((packed));

struct modeinfo {
  uint16 attributes;
  uint8 win_a, win_b;
  uint16 granularity, win_size, seg_a, seg_b;
  uint32 win_func;
  uint16 pitch;
  uint16 width, height;
  uint8 xchar, ychar, planes, bpp;
  uint8 banks, memory_model, bank_size, image_pages;
  uint8 rsvd1;
  uint8 red_size, red_pos;
  uint8 green_size, green_pos;
  uint8 blue_size, blue_pos;
  uint8 alpha_size, alpha_pos;
  uint8 direct_color;
  uint32 phys_base;
  uint32 off_screen;
  uint16 off_screen_size;
  uint16 lin_pitch;
  uint8 bnk_pages, lin_pages;
  uint8 lin_red_size, lin_red_pos;
  uint8 lin_green_size, lin_green_pos;
  uint8 lin_blue_size, lin_blue_pos;
  uint8 lin_alpha_size, lin_alpha_pos;
  uint32 max_clock;
  uint8 rsvd4[190];
} __attribute__((packed));

_Static_assert(sizeof(struct vbeinfo) == 512, "vbeinfo");
_Static_assert(sizeof(struct modeinfo) == 256, "modeinfo");

static struct vbeinfo info;
static struct modeinfo mi;

// the preferred detailed timing in EDID is the panel's native size, which is
// how the BSD loaders pick a mode instead of guessing
static uint8 edid[128];

static int edid_native(uint16 *w, uint16 *h) {
  const uint8 *d;
  uint32 sum = 0;
  int i;

  if (bios_vbe_edid(edid) != VBE_OK)
    return 0;
  if (edid[0] != 0x00 || edid[1] != 0xff || edid[2] != 0xff ||
      edid[3] != 0xff || edid[4] != 0xff || edid[5] != 0xff ||
      edid[6] != 0xff || edid[7] != 0x00)
    return 0;
  for (i = 0; i < 128; i++)
    sum += edid[i];
  if ((sum & 0xff) != 0)
    return 0;

  d = edid + 54;                 // first detailed timing descriptor
  if (d[0] == 0 && d[1] == 0)    // not a timing, just a text block
    return 0;
  *w = (uint16)(((uint32)(d[4] & 0xf0) << 4) | d[2]);
  *h = (uint16)(((uint32)(d[7] & 0xf0) << 4) | d[5]);
  return *w >= 320 && *h >= 200 && (*w % 8) == 0;
}

// bochs dispi: the vbe mode list stops at 1024x768, this sets any size
#define DISPI_INDEX 0x01ce
#define DISPI_DATA 0x01cf
#define DISPI_ID 0x0
#define DISPI_XRES 0x1
#define DISPI_YRES 0x2
#define DISPI_BPP 0x3
#define DISPI_ENABLE 0x4
#define DISPI_BANK 0x5
#define DISPI_VIRT_WIDTH 0x6
#define DISPI_VIDEO_MEMORY_64K 0xa
#define DISPI_ENABLED 0x01
#define DISPI_LFB_ENABLED 0x40

static inline void outw16(uint16 port, uint16 v) {
  __asm__ volatile("outw %0,%1" : : "a"(v), "d"(port));
}

static inline uint16 inw16(uint16 port) {
  uint16 v;
  __asm__ volatile("inw %1,%0" : "=a"(v) : "d"(port));
  return v;
}

static void dispi_write(uint16 reg, uint16 val) {
  outw16(DISPI_INDEX, reg);
  outw16(DISPI_DATA, val);
}

static uint16 dispi_read(uint16 reg) {
  outw16(DISPI_INDEX, reg);
  return inw16(DISPI_DATA);
}

static int dispi_present(void) {
  return (dispi_read(DISPI_ID) & 0xfff0) == 0xb0c0;
}

// returns the pitch, or 0 if the adapter will not take the mode
static uint32 dispi_set(uint16 w, uint16 h) {
  uint32 need = (uint32)w * h * 4;
  uint32 vram = (uint32)dispi_read(DISPI_VIDEO_MEMORY_64K) * 65536;

  if (w == 0 || h == 0 || (vram != 0 && need > vram))
    return 0;
  dispi_write(DISPI_ENABLE, 0);
  dispi_write(DISPI_XRES, w);
  dispi_write(DISPI_YRES, h);
  dispi_write(DISPI_BPP, 32);
  dispi_write(DISPI_VIRT_WIDTH, w);
  dispi_write(DISPI_BANK, 0);
  dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);
  if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h)
    return 0;
  return (uint32)w * 4;
}

// a linear packed-pixel graphics mode we can actually draw into. size is not
// part of this: nothing here decides a maximum resolution
static int usable(const struct modeinfo *m) {
  if ((m->attributes & 0x01) == 0)
    return 0;
  if ((m->attributes & 0x10) == 0)
    return 0;
  if ((m->attributes & 0x80) == 0)
    return 0;
  if (m->planes != 1)
    return 0;
  if (m->memory_model != 6)
    return 0;
  // a panel's native size is often offered at 16 bits and nothing else
  return m->bpp == 15 || m->bpp == 16 || m->bpp == 24 || m->bpp == 32;
}

// the mode the firmware already programmed. on coreboot that is the panel
// talking, and it beats any guess we could make
static int firmware_mode(uint16 *w, uint16 *h) {
  int mode = bios_vbe_get_mode();

  if (mode == 0xffff)
    return 0;
  mode &= 0x1ff;
  if (bios_vbe_mode_info(mode, &mi) != VBE_OK || !usable(&mi))
    return 0;
  *w = mi.width;
  *h = mi.height;
  return *w >= 320 && *h >= 200;
}

static void fill(struct fnu_bootinfo *bi, const struct modeinfo *m) {
  int v3 = info.version >= 0x0300 && m->lin_pitch != 0;

  bi->fb.base = m->phys_base;
  bi->fb.width = m->width;
  bi->fb.height = m->height;
  bi->fb.pitch = v3 ? m->lin_pitch : m->pitch;
  bi->fb.bpp = m->bpp;
  bi->fb.red_pos = v3 ? m->lin_red_pos : m->red_pos;
  bi->fb.red_size = v3 ? m->lin_red_size : m->red_size;
  bi->fb.green_pos = v3 ? m->lin_green_pos : m->green_pos;
  bi->fb.green_size = v3 ? m->lin_green_size : m->green_size;
  bi->fb.blue_pos = v3 ? m->lin_blue_pos : m->blue_pos;
  bi->fb.blue_size = v3 ? m->lin_blue_size : m->blue_size;
  bi->console = FNU_CONSOLE_FRAMEBUFFER;
}

void video_setup(struct fnu_bootinfo *bi) {
  uint32 list, best_score = 0;
  int best = -1;

  // coreboot already gave us a framebuffer
  if (bi->console == FNU_CONSOLE_FRAMEBUFFER)
    return;

  bi->console = FNU_CONSOLE_VGA_TEXT;

  {
    uint16 ew, eh;
#if defined(VBE_WIDTH) && defined(VBE_HEIGHT)
    want_w = VBE_WIDTH;
    want_h = VBE_HEIGHT;
    bi->fb.source = FNU_FB_SOURCE_BUILD;
#else
    if (edid_native(&ew, &eh)) {
      want_w = ew;
      want_h = eh;
      bi->fb.source = FNU_FB_SOURCE_EDID;
    } else if (firmware_mode(&ew, &eh)) {
      want_w = ew;
      want_h = eh;
      bi->fb.source = FNU_FB_SOURCE_FIRMWARE;
    } else {
      bi->fb.source = FNU_FB_SOURCE_DEFAULT;
    }
#endif
    (void)ew;
    (void)eh;
  }

  memset(&info, 0, sizeof(info));
  info.sig[0] = 'V';
  info.sig[1] = 'B';
  info.sig[2] = 'E';
  info.sig[3] = '2';
  if (bios_vbe_info(&info) != VBE_OK)
    return;
  if (info.sig[0] != 'V' || info.sig[1] != 'E' || info.sig[2] != 'S' ||
      info.sig[3] != 'A')
    return;

  list = (info.mode_ptr >> 16) * 16 + (info.mode_ptr & 0xffff);
  for (;;) {
    uint16 mode;
    uint32 score;

    flat_copy(&mode, list, sizeof(mode));
    list += sizeof(mode);
    if (mode == 0xffff)
      break;

    if (bios_vbe_mode_info(mode, &mi) != VBE_OK)
      continue;
    if (!usable(&mi))
      continue;

    // exact beats anything; then the biggest that still fits the panel; an
    // oversized mode only when the adapter offers nothing smaller
    if (mi.width == want_w && mi.height == want_h)
      score = 0x80000000u;
    else if (mi.width <= want_w && mi.height <= want_h)
      score = 0x40000000u + (uint32)mi.width * mi.height;
    else
      score = 0x20000000u - (uint32)mi.width * mi.height / 64;
    score += mi.bpp;   // same size, deepest pixel wins
    if (score > best_score) {
      best_score = score;
      best = mode;
    }
  }

  if (best < 0)
    return;
  if (bios_vbe_set_mode(best) != VBE_OK)
    return;
  if (bios_vbe_mode_info(best, &mi) != VBE_OK)
    return;
  if (mi.phys_base == 0)
    return;

  fill(bi, &mi);

  // the linear framebuffer stays put, so the size can still be raised
  if ((bi->fb.width != want_w || bi->fb.height != want_h) && dispi_present()) {
    uint32 pitch = dispi_set(want_w, want_h);
    if (pitch != 0) {
      bi->fb.width = want_w;
      bi->fb.height = want_h;
      bi->fb.pitch = pitch;
      bi->fb.bpp = 32;
      bi->fb.red_pos = 16; bi->fb.red_size = 8;
      bi->fb.green_pos = 8; bi->fb.green_size = 8;
      bi->fb.blue_pos = 0; bi->fb.blue_size = 8;
    }
  }
}
