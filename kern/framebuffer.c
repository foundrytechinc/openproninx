#include "defs.h"
#include "font.h"
#include "inc/abi.h"
#include "inc/bootinfo.h"
#include "realmode.h"
#include "vesa.h"
#include "inc/framebuffer.h"
#include "memlayout.h"
#include "virtio_gpu.h"
#include "x86.h"


#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF
#define VBE_DISPI_INDEX_ID 0x0
#define VBE_DISPI_INDEX_XRES 0x1
#define VBE_DISPI_INDEX_YRES 0x2
#define VBE_DISPI_INDEX_BPP 0x3
#define VBE_DISPI_INDEX_ENABLE 0x4
#define VBE_DISPI_INDEX_VIRT_WIDTH 0x6
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa

static volatile uchar *pixels;
static uchar *canvas; // where glyphs land: the ram shadow, or vram directly
static uchar *shadow;
static uint64_t shadow_bytes;
static const struct console_font *face;
static const uchar *font;
static uint font_stride;
static uint width;
static uint height;
static uint pitch;
static uint bytes_per_pixel;
static uint columns;
static uint rows;
static uint cell_width;
static uint cell_height;
static int initialization_error;
static int dispi_detected;
static int on_gpu;

// byte span of each scanline that differs from what the display holds
static uint32_t *dirty_lo;
static uint32_t *dirty_hi;
static uint dirty_top;
static uint dirty_bot;

static ushort *rendered_cells;

struct dec_cell {
  const uchar *glyph;
  uint64_t lut[4];
};

static struct dec_cell *dec_cells;
static uint64_t *scanline_buf64;

static inline void dispi_write(uint16_t reg, uint16_t val) {
  outw(VBE_DISPI_IOPORT_INDEX, reg);
  outw(VBE_DISPI_IOPORT_DATA, val);
}

static inline uint16_t dispi_read(uint16_t reg) {
  outw(VBE_DISPI_IOPORT_INDEX, reg);
  return inw(VBE_DISPI_IOPORT_DATA);
}

// sixteen colours packed once per mode, the way rasops keeps ri_devcmap
static uint16_t direct_palette[16];

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa,
    0xaa5500, 0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

// one aperture mapping, grown when a bigger mode needs more of it
static void *mapped;
static uint64_t mapped_phys;
static uint64_t mapped_size;

static struct boot_framebuffer_info *boot_framebuffer_info(void);

static void build_direct_palette(void) {
  const struct boot_framebuffer_info *info = boot_framebuffer_info();
  uint i, r, g, b;

  for (i = 0; i < 16; i++) {
    r = (vga_palette[i] >> 16) & 0xff;
    g = (vga_palette[i] >> 8) & 0xff;
    b = vga_palette[i] & 0xff;
    direct_palette[i] =
        (uint16_t)(((r >> (8 - info->red_size)) << info->red_pos) |
                   ((g >> (8 - info->green_size)) << info->green_pos) |
                   ((b >> (8 - info->blue_size)) << info->blue_pos));
  }
}

static int framebuffer_reshape(int want_shadow);

// widest face that belongs on this screen, as OpenBSD rasops_init picks it.
// nothing is ever stretched.
static const struct console_font *pick_face(uint w) {
  uint i;

  for (i = 0; i + 1 < console_font_count; i++)
    if (w >= console_fonts[i].min_screen_width)
      return &console_fonts[i];
  return &console_fonts[console_font_count - 1];
}

static void mark_dirty(uint y0, uint y1, uint xb0, uint xb1) {
  uint y;

  if (dirty_lo == 0)
    return;
  if (y1 > height)
    y1 = height;
  if (xb1 > pitch)
    xb1 = pitch;
  if (y0 >= y1 || xb0 >= xb1)
    return;
  for (y = y0; y < y1; y++) {
    if (dirty_lo[y] > dirty_hi[y]) {
      dirty_lo[y] = xb0;
      dirty_hi[y] = xb1;
    } else {
      if (xb0 < dirty_lo[y])
        dirty_lo[y] = xb0;
      if (xb1 > dirty_hi[y])
        dirty_hi[y] = xb1;
    }
  }
  if (y0 < dirty_top)
    dirty_top = y0;
  if (y1 > dirty_bot)
    dirty_bot = y1;
}

static void dirty_reset(void) {
  uint y;

  if (dirty_lo != 0)
    for (y = dirty_top; y < dirty_bot; y++) {
      dirty_lo[y] = 1;
      dirty_hi[y] = 0;
    }
  dirty_top = height;
  dirty_bot = 0;
}

// push the dirty spans at the display and forget them
static void fb_present(void) {
  uint y, lo, hi;

  if (dirty_top >= dirty_bot)
    return;

  if (on_gpu) {
    virtio_gpu_flush_rect(0, dirty_top, width, dirty_bot - dirty_top);
    dirty_reset();
    return;
  }

  if (shadow != 0) {
    for (y = dirty_top; y < dirty_bot; y++) {
      lo = dirty_lo[y];
      hi = dirty_hi[y];
      if (lo >= hi)
        continue;
      memmove((void *)(pixels + (uint64_t)y * pitch + lo),
              shadow + (uint64_t)y * pitch + lo,
              hi - lo);
    }
  }
  dirty_reset();
}

static struct boot_framebuffer_info fbinfo;
static int fbinfo_ready;

static struct boot_framebuffer_info *boot_framebuffer_info(void) {
  const struct fnu_bootinfo *bi;

  if (!fbinfo_ready) {
    fbinfo_ready = 1;
    bi = bootinfo();
    if (bi && bi->console == FNU_CONSOLE_FRAMEBUFFER && bi->fb.base != 0) {
      fbinfo.magic = BOOT_FRAMEBUFFER_MAGIC;
      fbinfo.physical_base = bi->fb.base;
      fbinfo.width = (uint16_t)bi->fb.width;
      fbinfo.height = (uint16_t)bi->fb.height;
      fbinfo.pitch = (uint16_t)bi->fb.pitch;
      fbinfo.bits_per_pixel = bi->fb.bpp;
      fbinfo.red_pos = bi->fb.red_pos;
      fbinfo.red_size = bi->fb.red_size;
      fbinfo.green_pos = bi->fb.green_pos;
      fbinfo.green_size = bi->fb.green_size;
      fbinfo.blue_pos = bi->fb.blue_pos;
      fbinfo.blue_size = bi->fb.blue_size;
    }
  }
  return &fbinfo;
}

uint64_t framebuffer_phys(void) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();

  if (info->magic != BOOT_FRAMEBUFFER_MAGIC ||
      info->bits_per_pixel < 15 || info->bits_per_pixel > 32 ||
      info->physical_base == 0 || info->width == 0 || info->height == 0 ||
      info->pitch < (uint)info->width * ((info->bits_per_pixel + 7) / 8))
    return 0;
  return info->physical_base;
}

// all of the adapter video memory when it says how much it has
uint64_t framebuffer_vram(void) {
  if ((dispi_read(VBE_DISPI_INDEX_ID) & 0xfff0) == 0xb0c0)
    return (uint64_t)dispi_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K) * 65536;
  return vesa_video_memory_known();
}

uint64_t framebuffer_size(void) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();
  uint64_t mode, vram;

  if (!framebuffer_phys())
    return 0;
  mode = (uint64_t)info->pitch * info->height;
  vram = framebuffer_vram();
  if (vram > mode)
    mode = vram;
  return mode;
}

int framebuffer_available(void) { return pixels != 0; }

const char *framebuffer_font_name(void) {
  return face ? face->name : "none";
}

uint32_t framebuffer_boot_tag(void) { return boot_framebuffer_info()->magic; }

uint64_t framebuffer_boot_physical_base(void) {
  return boot_framebuffer_info()->physical_base;
}

uint framebuffer_boot_width(void) { return boot_framebuffer_info()->width; }

uint framebuffer_boot_height(void) { return boot_framebuffer_info()->height; }

uint framebuffer_boot_pitch(void) { return boot_framebuffer_info()->pitch; }

uint framebuffer_boot_bits_per_pixel(void) {
  return boot_framebuffer_info()->bits_per_pixel;
}

int framebuffer_initialization_error(void) { return initialization_error; }

uint framebuffer_width(void) { return width; }

uint framebuffer_height(void) { return height; }

uint framebuffer_columns(void) { return columns; }

uint framebuffer_rows(void) { return rows; }

int framebuffer_has_dispi(void) { return dispi_detected; }

uint framebuffer_mode_source_code(void) {
  const struct fnu_bootinfo *bi = bootinfo();

  return bi ? bi->fb.source : 0;
}

// who chose the size we are running at
const char *framebuffer_mode_source(void) {
  static const char *name[] = {"default", "edid", "firmware", "coreboot",
                               "build"};
  const struct fnu_bootinfo *bi = bootinfo();
  uint s = bi ? bi->fb.source : 0;

  return s < sizeof(name) / sizeof(name[0]) ? name[s] : "unknown";
}

static void *framebuffer_remap(uint64_t phys, uint64_t bytes) {
  if (phys == 0 || bytes == 0)
    return 0;
  if (mapped != 0 && phys == mapped_phys && bytes <= mapped_size)
    return mapped;
  mapped = ioremap_wc(phys, bytes);
  if (mapped != 0) {
    mapped_phys = phys;
    mapped_size = bytes;
  }
  return mapped;
}

void *framebuffer_virt(void) {
  return framebuffer_remap(framebuffer_phys(), framebuffer_size());
}

// scratch that follows the mode. the shadow is optional; without it we
// draw straight into vram, as rasops does with a null ri_hwbits.
static void buffers_free(void) {
  vmem_free(shadow); shadow = 0; shadow_bytes = 0;
  vmem_free(dirty_lo); dirty_lo = 0;
  vmem_free(dirty_hi); dirty_hi = 0;
  vmem_free(rendered_cells); rendered_cells = 0;
  vmem_free(dec_cells); dec_cells = 0;
  vmem_free(scanline_buf64); scanline_buf64 = 0;
}

static int buffers_alloc(void) {
  dirty_lo = vmem_alloc((uint64_t)height * sizeof(uint32_t));
  dirty_hi = vmem_alloc((uint64_t)height * sizeof(uint32_t));
  rendered_cells = vmem_alloc((uint64_t)columns * rows * sizeof(ushort));
  dec_cells = vmem_alloc((uint64_t)columns * sizeof(struct dec_cell));
  scanline_buf64 =
      vmem_alloc((uint64_t)columns * (cell_width / 2) * sizeof(uint64_t));
  return dirty_lo && dirty_hi && rendered_cells && dec_cells && scanline_buf64;
}

// call with the geometry already set
static int framebuffer_reshape(int want_shadow) {
  buffers_free();
  if (!buffers_alloc()) {
    buffers_free();
    initialization_error = 4;
    return -1;
  }
  if (want_shadow && !on_gpu) {
    uint64_t need = (uint64_t)pitch * height;
    shadow = vmem_alloc(need);
    if (shadow != 0) {
      shadow_bytes = need;
      memset(shadow, 0, need);
    }
  }
  canvas = shadow ? shadow : (uchar *)pixels;
  dirty_top = height;
  dirty_bot = 0;
  memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(ushort));
  if (shadow != 0)
    mark_dirty(0, height, 0, pitch);
  return 0;
}

void framebuffer_shadow_init(void) {
  if (pixels == 0 || on_gpu)
    return;
  if (shadow == 0 && framebuffer_reshape(1) < 0) {
    cprintf("VIDEO: console arena too small for %dx%d\n", width, height);
    return;
  }
  if (shadow == 0)
    cprintf("VIDEO: no shadow, drawing straight to vram\n");
  else
    cprintf("VIDEO: %d KB ram shadow (arena %d MB)\n",
            (uint)(shadow_bytes / 1024),
            (uint)(vmem_capacity() / (1024 * 1024)));
}

static void geometry(uint w, uint h, uint p, uint bpp) {
  width = w;
  height = h;
  pitch = p;
  bytes_per_pixel = (bpp + 7) / 8;
  build_direct_palette();
  face = pick_face(width);
  font = face->data;
  font_stride = face->stride;
  cell_width = face->width;
  cell_height = face->height;
  columns = width / cell_width;
  rows = height / cell_height;
}

void framebuffer_init(void) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();

  if (!framebuffer_phys()) {
    initialization_error = 1;
    return;
  }
  pixels = (volatile uchar *)framebuffer_virt();
  geometry(info->width, info->height, info->pitch, info->bits_per_pixel);
  initialization_error = 0;
  canvas = (uchar *)pixels;
  on_gpu = 0;

  dispi_detected = (dispi_read(VBE_DISPI_INDEX_ID) & 0xfff0) == 0xb0c0;
  // the shadow comes up with the mode: scrolling must never read vram back
  if (framebuffer_reshape(1) < 0) {
    pixels = 0;
    canvas = 0;
    return;
  }

  if (rendered_cells)
    memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(ushort));
  dirty_reset();
  memset(canvas, 0, (uint64_t)pitch * height);
}

void framebuffer_init_gpu(void *fb_mem, uint w, uint h, uint p) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();

  pixels = (volatile uchar *)fb_mem;
  geometry(w, h, p, 32);
  initialization_error = 0;
  // the gpu buffer already lives in ram and is pushed by virtio commands
  on_gpu = 1;
  dispi_detected = 0;
  if (framebuffer_reshape(0) < 0) {
    pixels = 0;
    canvas = 0;
    return;
  }
  canvas = (uchar *)fb_mem;
  if (rendered_cells)
    memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(ushort));
  dirty_reset();
}

// a glyph row is `stride` bytes, msb leftmost. Spleen 12x24 uses twelve
// bits of sixteen, so index by bit.
static inline uchar glyph_bits(const uchar *row, uint px) {
  return row[px >> 3];
}

// two pixels per qword, for any face width
static void draw_span_32(uint row, const ushort *crt_row, uint c0, uint c1) {
  uint y0 = row * cell_height;
  uint qwords = (c1 - c0) * (cell_width / 2);
  uint gy, col, i, idx, px;

  for (col = c0; col < c1; col++) {
    ushort cell = crt_row[col];
    uchar attr = cell >> 8;
    uint64_t fg = vga_palette[attr & 0x0f];
    uint64_t bg = vga_palette[(attr >> 4) & 0x0f];
    dec_cells[col].lut[0] = (bg << 32) | bg;
    dec_cells[col].lut[1] = (fg << 32) | bg;
    dec_cells[col].lut[2] = (bg << 32) | fg;
    dec_cells[col].lut[3] = (fg << 32) | fg;
    dec_cells[col].glyph =
        font + (uint64_t)(cell & 0xff) * cell_height * font_stride;
  }

  for (gy = 0; gy < cell_height; gy++) {
    idx = 0;
    for (col = c0; col < c1; col++) {
      const uchar *g = dec_cells[col].glyph + (uint64_t)gy * font_stride;
      const uint64_t *lut = dec_cells[col].lut;

      for (px = 0; px < cell_width; px += 2)
        scanline_buf64[idx++] =
            lut[(glyph_bits(g, px) >> (6 - (px & 7))) & 3];
    }
    uint64_t *dst = (uint64_t *)(canvas + (uint64_t)(y0 + gy) * pitch +
                                 (uint64_t)c0 * cell_width * 4);
    for (i = 0; i < qwords; i++)
      dst[i] = scanline_buf64[i];
  }
  mark_dirty(y0, y0 + cell_height, c0 * cell_width * 4, c1 * cell_width * 4);
}

static void draw_span_24(uint row, const ushort *crt_row, uint c0, uint c1) {
  uint y0 = row * cell_height;
  uint gy, col, px;

  for (gy = 0; gy < cell_height; gy++) {
    uchar *line = canvas + (uint64_t)(y0 + gy) * pitch +
                  (uint64_t)c0 * cell_width * 3;

    for (col = c0; col < c1; col++) {
      ushort cell = crt_row[col];
      uchar attr = cell >> 8;
      uint32_t fg = vga_palette[attr & 0x0f];
      uint32_t bg = vga_palette[(attr >> 4) & 0x0f];
      const uchar *g = font + (uint64_t)(cell & 0xff) * cell_height *
                                  font_stride + (uint64_t)gy * font_stride;

      for (px = 0; px < cell_width; px++) {
        uint32_t c = (glyph_bits(g, px) & (0x80 >> (px & 7))) ? fg : bg;
        line[0] = (uchar)c;
        line[1] = (uchar)(c >> 8);
        line[2] = (uchar)(c >> 16);
        line += 3;
      }
    }
  }
  mark_dirty(y0, y0 + cell_height, c0 * cell_width * 3, c1 * cell_width * 3);
}

// 15 and 16 bit modes. widescreen sizes are often offered only here.
static void draw_span_16(uint row, const ushort *crt_row, uint c0, uint c1) {
  uint y0 = row * cell_height;
  uint gy, col, px;

  for (gy = 0; gy < cell_height; gy++) {
    uint16_t *line = (uint16_t *)(canvas + (uint64_t)(y0 + gy) * pitch +
                                  (uint64_t)c0 * cell_width * 2);

    for (col = c0; col < c1; col++) {
      ushort cell = crt_row[col];
      uchar attr = cell >> 8;
      uint16_t fg = direct_palette[attr & 0x0f];
      uint16_t bg = direct_palette[(attr >> 4) & 0x0f];
      const uchar *g = font + (uint64_t)(cell & 0xff) * cell_height *
                                  font_stride + (uint64_t)gy * font_stride;

      for (px = 0; px < cell_width; px++)
        *line++ = (glyph_bits(g, px) & (0x80 >> (px & 7))) ? fg : bg;
    }
  }
  mark_dirty(y0, y0 + cell_height, c0 * cell_width * 2, c1 * cell_width * 2);
}

void framebuffer_draw_row_span(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  if (!canvas || !dec_cells || !crt_row || row >= rows || start_col >= end_col)
    return;
  if (end_col > columns)
    end_col = columns;

  if (bytes_per_pixel == 2)
    draw_span_16(row, crt_row, start_col, end_col);
  else if (bytes_per_pixel == 3)
    draw_span_24(row, crt_row, start_col, end_col);
  else
    draw_span_32(row, crt_row, start_col, end_col);
}

void framebuffer_draw_row(uint row, const ushort *crt_row) {
  framebuffer_draw_row_span(row, crt_row, 0, columns);
}

void framebuffer_draw_cell(int cell_index, ushort cell) {
  uint row, col;
  ushort one = cell;

  if (!canvas || !rendered_cells || cell_index < 0 ||
      cell_index >= (int)(columns * rows))
    return;
  rendered_cells[cell_index] = cell;
  row = (uint)cell_index / columns;
  col = (uint)cell_index % columns;
  framebuffer_draw_row_span(row, &one - col, col, col + 1);
  fb_present();
}

void framebuffer_redraw_cells(const ushort *cells, int count) {
  uint total_rows, r;

  if (!canvas || !rendered_cells || !cells)
    return;
  total_rows = ((uint)count + columns - 1) / columns;
  if (total_rows > rows)
    total_rows = rows;

  for (r = 0; r < total_rows; r++) {
    framebuffer_draw_row(r, cells + r * columns);
    memmove(rendered_cells + r * columns, cells + r * columns,
            columns * sizeof(ushort));
  }
  fb_present();
}

void framebuffer_flush_cells(const ushort *cells, int count) {
  uint total_rows, r, min_col, max_col;

  if (!canvas || !rendered_cells || !cells)
    return;
  total_rows = ((uint)count + columns - 1) / columns;
  if (total_rows > rows)
    total_rows = rows;

  for (r = 0; r < total_rows; r++) {
    const ushort *cur = cells + r * columns;
    ushort *rend = rendered_cells + r * columns;
    if (memcmp(cur, rend, columns * sizeof(ushort)) == 0)
      continue;
    min_col = 0;
    while (min_col < columns && cur[min_col] == rend[min_col])
      min_col++;
    max_col = columns - 1;
    while (max_col > min_col && cur[max_col] == rend[max_col])
      max_col--;
    framebuffer_draw_row_span(r, cur, min_col, max_col + 1);
    memmove(rend + min_col, cur + min_col,
            (max_col - min_col + 1) * sizeof(ushort));
  }
  fb_present();
}

void framebuffer_clear(void) {
  if (!canvas || !rendered_cells)
    return;
  memset(canvas, 0, (uint64_t)pitch * height);
  if (rendered_cells)
    memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(ushort));
  mark_dirty(0, height, 0, pitch);
  fb_present();
}

// scrolling moves ram, never reads the display back
void framebuffer_scroll_lines(int n, ushort fill_cell) {
  uint moved, blank, y;
  uchar attr;
  uint32_t bg;

  if (!canvas || !rendered_cells || n <= 0)
    return;
  if (n >= (int)rows) {
    framebuffer_clear();
    return;
  }

  blank = (uint)n * cell_height;
  moved = rows * cell_height - blank;
  memmove(canvas, canvas + (uint64_t)blank * pitch, (uint64_t)moved * pitch);

  attr = fill_cell >> 8;
  bg = vga_palette[(attr >> 4) & 0x0f];
  if (bg == 0) {
    memset(canvas + (uint64_t)moved * pitch, 0, (uint64_t)blank * pitch);
  } else {
    for (y = moved; y < moved + blank; y++) {
      if (bytes_per_pixel == 4) {
        uint32_t *line = (uint32_t *)(canvas + (uint64_t)y * pitch);
        for (uint x = 0; x < width; x++)
          line[x] = bg;
      } else if (bytes_per_pixel == 2) {
        uint16_t *line = (uint16_t *)(canvas + (uint64_t)y * pitch);
        for (uint x = 0; x < width; x++)
          line[x] = direct_palette[(attr >> 4) & 0x0f];
      } else {
        uchar *line = canvas + (uint64_t)y * pitch;
        for (uint x = 0; x < width; x++) {
          line[x * 3] = (uchar)bg;
          line[x * 3 + 1] = (uchar)(bg >> 8);
          line[x * 3 + 2] = (uchar)(bg >> 16);
        }
      }
    }
  }

  memmove(rendered_cells, rendered_cells + (uint)n * columns,
          (rows - (uint)n) * columns * sizeof(ushort));
  for (y = (rows - (uint)n) * columns; y < rows * columns; y++)
    rendered_cells[y] = fill_cell;

  mark_dirty(0, moved + blank, 0, pitch);
}

void framebuffer_scroll_up(void) { framebuffer_scroll_lines(1, ' ' | 0x0700); }

// real hardware has no dispi. the mode comes from the video bios, as
// FreeBSD vesa_set_mode() does it, and the adapter reports what it got.
static int display_set_vesa_mode(uint w, uint h) {
  struct vesa_modeinfo mi;
  uint ow = width, oh = height, op = pitch, obpp = bytes_per_pixel * 8;
  uint32_t obase;
  uint64_t need;

  if (!vesa_available())
    return -1;
  obase = boot_framebuffer_info()->physical_base;
  if (vesa_set_mode(w, h, &mi) < 0)
    return -1;
  if (mi.phys_base == 0 || mi.pitch < mi.width * (mi.bpp / 8)) {
    vesa_set_mode(ow, oh, &mi);
    return -1;
  }

  boot_framebuffer_info()->physical_base = mi.phys_base;
  boot_framebuffer_info()->width = (uint16_t)mi.width;
  boot_framebuffer_info()->height = (uint16_t)mi.height;
  boot_framebuffer_info()->pitch = (uint16_t)mi.pitch;
  boot_framebuffer_info()->bits_per_pixel = (uint8_t)mi.bpp;
  boot_framebuffer_info()->red_pos = mi.red_pos;
  boot_framebuffer_info()->red_size = mi.red_size;
  boot_framebuffer_info()->green_pos = mi.green_pos;
  boot_framebuffer_info()->green_size = mi.green_size;
  boot_framebuffer_info()->blue_pos = mi.blue_pos;
  boot_framebuffer_info()->blue_size = mi.blue_size;
  need = (uint64_t)mi.pitch * mi.height;
  if (vesa_video_memory() > need)
    need = vesa_video_memory();
  pixels = (volatile uchar *)framebuffer_remap(mi.phys_base, need);
  if (pixels == 0) {
    boot_framebuffer_info()->physical_base = obase;
    boot_framebuffer_info()->width = (uint16_t)ow;
    boot_framebuffer_info()->height = (uint16_t)oh;
    boot_framebuffer_info()->pitch = (uint16_t)op;
    boot_framebuffer_info()->bits_per_pixel = (uint8_t)obpp;
    vesa_set_mode(ow, oh, &mi);
    pixels = (volatile uchar *)framebuffer_remap(obase, (uint64_t)op * oh);
    return -1;
  }

  geometry(mi.width, mi.height, mi.pitch, mi.bpp);
  if (framebuffer_reshape(1) < 0) {
    geometry(ow, oh, op, obpp);
    vesa_set_mode(ow, oh, &mi);
    framebuffer_reshape(1);
    return -1;
  }
  memset(canvas, 0, (uint64_t)pitch * height);
  dirty_reset();
  mark_dirty(0, height, 0, pitch);
  fb_present();
  console_switch_to_gpu();
  cprintf("VESA: mode %dx%dx%d at 0x%x, pitch %d\n", mi.width, mi.height,
          mi.bpp, mi.phys_base, mi.pitch);
  return 0;
}

// the adapter states its own limits. nothing here caps a resolution.
int display_set_resolution(uint w, uint h) {
  uint ow = width, oh = height, op = pitch;
  uint32_t new_pitch = w * 4;
  uint64_t need = (uint64_t)new_pitch * h;
  uint64_t vram;

  if (virtio_gpu_available())
    return virtio_gpu_set_resolution(w, h);
  if (!pixels)
    return -1;
  if (w < 320 || h < 200 || (w % 8) != 0)
    return -1;
  if (!dispi_detected)
    return display_set_vesa_mode(w, h);
  vram = (uint64_t)dispi_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K) * 65536;
  if (vram != 0 && need > vram)
    return -1;

  dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
  dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
  dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
  dispi_write(VBE_DISPI_INDEX_BPP, 32);
  dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
  dispi_write(VBE_DISPI_INDEX_ENABLE, 0x01 | 0x40);
  if (dispi_read(VBE_DISPI_INDEX_XRES) != w ||
      dispi_read(VBE_DISPI_INDEX_YRES) != h) {
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)ow);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)oh);
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0x01 | 0x40);
    return -1;
  }

  geometry(w, h, new_pitch, 32);
  if (framebuffer_reshape(1) < 0) {
    geometry(ow, oh, op, 32);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)ow);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)oh);
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0x01 | 0x40);
    framebuffer_reshape(1);
    return -1;
  }
  memset(canvas, 0, need);
  dirty_reset();
  mark_dirty(0, height, 0, pitch);
  fb_present();
  console_switch_to_gpu();
  cprintf("VBE: resolution changed to %dx%d\n", w, h);
  return 0;
}

// dispi in a vm, the video bios on real iron, or the gpu. route only;
// probing the bios waits for a mode change
int display_can_modeset(void) {
  return virtio_gpu_available() ||
         (pixels != 0 && (dispi_detected || realmode_available()));
}

// The adapter lists what it can do; nothing here filters it
struct modelist_ctx {
  struct fb_modelist *out;
};

static int modelist_add(uint16_t mode, uint w, uint h, uint bpp, void *arg) {
  struct fb_modelist *l = ((struct modelist_ctx *)arg)->out;
  uint i;

  (void)mode;
  for (i = 0; i < l->count; i++)
    if (l->mode[i].width == w && l->mode[i].height == h) {
      if (bpp > l->mode[i].bpp)
        l->mode[i].bpp = (uint16_t)bpp;
      return 0;
    }
  if (l->count >= FB_MODELIST_MAX)
    return 1;
  l->mode[l->count].width = (uint16_t)w;
  l->mode[l->count].height = (uint16_t)h;
  l->mode[l->count].bpp = (uint16_t)bpp;
  l->count++;
  return 0;
}

void display_dump_modes(void) {
  if (virtio_gpu_available()) {
    cprintf("VIDEO: virtio-gpu, any size\n");
    return;
  }
  if (dispi_detected) {
    cprintf("VIDEO: dispi, any size up to %d KB of video memory\n",
            (uint)(framebuffer_vram() / 1024));
    return;
  }
  vesa_dump_modes();
}

int display_mode_list(struct fb_modelist *out) {
  struct modelist_ctx ctx;

  memset(out, 0, sizeof(*out));
  ctx.out = out;
  if (virtio_gpu_available() || dispi_detected)
    return 0; // any size fits, there is no list to give
  if (vesa_each_mode(modelist_add, &ctx) < 0)
    return -1;
  return (int)out->count;
}

void display_get_resolution(uint *w, uint *h, uint *bpp) {
  if (virtio_gpu_available()) {
    if (w) *w = virtio_gpu_width();
    if (h) *h = virtio_gpu_height();
    if (bpp) *bpp = 32;
  } else if (framebuffer_available()) {
    if (w) *w = framebuffer_width();
    if (h) *h = framebuffer_height();
    if (bpp) *bpp = bytes_per_pixel * 8;
  } else {
    if (w) *w = 80;
    if (h) *h = 25;
    if (bpp) *bpp = 4;
  }
}
