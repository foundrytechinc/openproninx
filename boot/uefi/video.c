#include "boot/uefi/uefi.h"

// The size to ask for when the panel will not say. VBE_WIDTH/VBE_HEIGHT
// override it outright, exactly as on the bios path.
#ifndef VBE_FALLBACK_WIDTH
#define VBE_FALLBACK_WIDTH 1024
#endif
#ifndef VBE_FALLBACK_HEIGHT
#define VBE_FALLBACK_HEIGHT 768
#endif

#define EFI_BY_PROTOCOL 2

static const struct efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_GUID;
static const struct efi_guid edid_active_guid = EFI_EDID_ACTIVE_GUID;
static const struct efi_guid edid_discovered_guid = EFI_EDID_DISCOVERED_GUID;

static uint32 want_w = VBE_FALLBACK_WIDTH;
static uint32 want_h = VBE_FALLBACK_HEIGHT;

// the preferred detailed timing in EDID is the panel's native size
static int edid_native(const uint8 *e, uint32 size, uint32 *w, uint32 *h) {
  const uint8 *d;
  uint32 sum = 0;
  int i;

  if (e == 0 || size < 128)
    return 0;
  if (e[0] != 0x00 || e[1] != 0xff || e[2] != 0xff || e[3] != 0xff ||
      e[4] != 0xff || e[5] != 0xff || e[6] != 0xff || e[7] != 0x00)
    return 0;
  for (i = 0; i < 128; i++)
    sum += e[i];
  if ((sum & 0xff) != 0)
    return 0;

  d = e + 54;
  if (d[0] == 0 && d[1] == 0)
    return 0;
  *w = ((uint32)(d[4] & 0xf0) << 4) | d[2];
  *h = ((uint32)(d[7] & 0xf0) << 4) | d[5];
  return *w >= 320 && *h >= 200 && (*w % 8) == 0;
}

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

static uint32 dispi_set(uint32 w, uint32 h) {
  uint32 need = w * h * 4;
  uint32 vram = (uint32)dispi_read(DISPI_VIDEO_MEMORY_64K) * 65536;

  if (w == 0 || h == 0 || w > 0xffff || h > 0xffff)
    return 0;
  if (vram != 0 && need > vram)
    return 0;
  dispi_write(DISPI_ENABLE, 0);
  dispi_write(DISPI_XRES, (uint16)w);
  dispi_write(DISPI_YRES, (uint16)h);
  dispi_write(DISPI_BPP, 32);
  dispi_write(DISPI_VIRT_WIDTH, (uint16)w);
  dispi_write(DISPI_BANK, 0);
  dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);
  if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h)
    return 0;
  return w * 4;
}

static struct efi_graphics_output *find_gop(efi_handle *owner) {
  struct efi_graphics_output *gop = 0;
  efi_handle *handles = 0;
  efi_uintn count = 0, i;

  *owner = 0;
  if (EFI_ERROR(BS->locate_handle_buffer(EFI_BY_PROTOCOL, &gop_guid, 0, &count,
                                         &handles)) ||
      handles == 0)
    return 0;
  for (i = 0; i < count; i++) {
    if (EFI_ERROR(BS->handle_protocol(handles[i], &gop_guid, (void **)&gop)))
      continue;
    if (gop->mode == 0 || gop->mode->max_mode == 0)
      continue;
    *owner = handles[i];
    break;
  }
  BS->free_pool(handles);
  return *owner != 0 ? gop : 0;
}

static int panel_size(efi_handle owner, uint32 *w, uint32 *h) {
  struct efi_edid *e;

  if (!EFI_ERROR(BS->handle_protocol(owner, &edid_active_guid, (void **)&e)) &&
      edid_native(e->edid, e->size, w, h))
    return 1;
  if (!EFI_ERROR(
          BS->handle_protocol(owner, &edid_discovered_guid, (void **)&e)) &&
      edid_native(e->edid, e->size, w, h))
    return 1;
  return 0;
}

static void channels(struct fnu_bootinfo *bi,
                     const struct efi_gop_mode_info *m) {
  const struct efi_pixel_bitmask *pm = &m->pixel_information;
  uint32 mask, pos, size;
  int i;

  bi->fb.bpp = 32;
  switch (m->pixel_format) {
  case EFI_PIXEL_RGBX:
    bi->fb.red_pos = 0;
    bi->fb.green_pos = 8;
    bi->fb.blue_pos = 16;
    bi->fb.red_size = bi->fb.green_size = bi->fb.blue_size = 8;
    return;
  case EFI_PIXEL_BGRX:
    bi->fb.blue_pos = 0;
    bi->fb.green_pos = 8;
    bi->fb.red_pos = 16;
    bi->fb.red_size = bi->fb.green_size = bi->fb.blue_size = 8;
    return;
  default:
    break;
  }
  for (i = 0; i < 3; i++) {
    mask = i == 0 ? pm->red : i == 1 ? pm->green : pm->blue;
    for (pos = 0; pos < 32 && (mask & (1u << pos)) == 0; pos++)
      ;
    for (size = 0; pos + size < 32 && (mask & (1u << (pos + size))) != 0;
         size++)
      ;
    if (i == 0) {
      bi->fb.red_pos = (uint8)pos;
      bi->fb.red_size = (uint8)size;
    } else if (i == 1) {
      bi->fb.green_pos = (uint8)pos;
      bi->fb.green_size = (uint8)size;
    } else {
      bi->fb.blue_pos = (uint8)pos;
      bi->fb.blue_size = (uint8)size;
    }
  }
}

// a linear packed-pixel mode we can draw into. size is not part of this
static int usable(const struct efi_gop_mode_info *m) {
  if (m->width == 0 || m->height == 0)
    return 0;
  if (m->pixels_per_scanline < m->width)
    return 0;
  if (m->pixel_format == EFI_PIXEL_BITMASK)
    return m->pixel_information.red != 0 && m->pixel_information.green != 0 &&
           m->pixel_information.blue != 0;
  return m->pixel_format == EFI_PIXEL_RGBX || m->pixel_format == EFI_PIXEL_BGRX;
}

void video_setup(struct fnu_bootinfo *bi) {
  struct efi_graphics_output *gop;
  struct efi_gop_mode_info *m;
  efi_handle owner;
  efi_uintn n;
  uint32 best = 0, best_score = 0, mode, ew, eh;

  gop = find_gop(&owner);
  if (gop == 0) {
    putstr("boot: no graphics output protocol\n");
    return;
  }

#if defined(VBE_WIDTH) && defined(VBE_HEIGHT)
  want_w = VBE_WIDTH;
  want_h = VBE_HEIGHT;
  bi->fb.source = FNU_FB_SOURCE_BUILD;
#else
  if (panel_size(owner, &ew, &eh)) {
    want_w = ew;
    want_h = eh;
    bi->fb.source = FNU_FB_SOURCE_EDID;
  } else if (gop->mode->info != 0 && usable(gop->mode->info)) {
    want_w = gop->mode->info->width;
    want_h = gop->mode->info->height;
    bi->fb.source = FNU_FB_SOURCE_FIRMWARE;
  } else {
    bi->fb.source = FNU_FB_SOURCE_DEFAULT;
  }
#endif
  (void)ew;
  (void)eh;

  // exact beats anything; then the biggest that still fits the panel; an
  // oversized mode only when the adapter offers nothing smaller
  for (mode = 0; mode < gop->mode->max_mode; mode++) {
    uint32 score;

    if (EFI_ERROR(gop->query_mode(gop, mode, &n, &m)) || n < sizeof(*m))
      continue;
    if (!usable(m))
      continue;
    if (m->width == want_w && m->height == want_h)
      score = 0x80000000u;
    else if (m->width <= want_w && m->height <= want_h)
      score = 0x40000000u + m->width * m->height;
    else
      score = 0x20000000u - m->width * m->height / 64;
    if (score > best_score) {
      best_score = score;
      best = mode;
    }
  }
  if (best_score == 0) {
    putstr("boot: no usable graphics mode\n");
    return;
  }

  if (best != gop->mode->mode && EFI_ERROR(gop->set_mode(gop, best))) {
    putstr("boot: the adapter refused the mode we picked\n");
    return;
  }
  m = gop->mode->info;
  if (gop->mode->framebuffer_base == 0 || m == 0 || !usable(m)) {
    putstr("boot: no linear framebuffer behind this mode\n");
    return;
  }

  bi->fb.base = gop->mode->framebuffer_base;
  bi->fb.width = m->width;
  bi->fb.height = m->height;
  bi->fb.pitch = m->pixels_per_scanline * 4;
  channels(bi, m);
  bi->console = FNU_CONSOLE_FRAMEBUFFER;

  // the linear framebuffer stays put, so the size can still be raised
  if ((bi->fb.width != want_w || bi->fb.height != want_h) && dispi_present()) {
    uint32 pitch = dispi_set(want_w, want_h);
    if (pitch != 0) {
      bi->fb.width = want_w;
      bi->fb.height = want_h;
      bi->fb.pitch = pitch;
      bi->fb.bpp = 32;
      bi->fb.red_pos = 16;
      bi->fb.red_size = 8;
      bi->fb.green_pos = 8;
      bi->fb.green_size = 8;
      bi->fb.blue_pos = 0;
      bi->fb.blue_size = 8;
    }
  }

  putstr("boot: ");
  putdec(bi->fb.width);
  putstr("x");
  putdec(bi->fb.height);
  putstr(" at ");
  puthex(bi->fb.base);
  putstr("\n");
}
