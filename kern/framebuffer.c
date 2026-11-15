#include "defs.h"
#include "font.h"
#include "inc/abi.h"
#include "inc/bootinfo.h"
#include "realmode.h"
#include "vesa.h"
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
static uchar *canvas;
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

static uint32_t *dirty_lo;
static uint32_t *dirty_hi;
static uint dirty_top;
static uint dirty_bot;

static vtcell_t *rendered_cells;

static int cursor_index = -1;
static int cursor_on;

static int ptr_x, ptr_y, ptr_shown;
static int ptr_drawn_x, ptr_drawn_y, ptr_drawn;

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

static uint16_t direct_palette[16];

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa,
    0xaa5500, 0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static void *mapped;
static uint64_t mapped_phys;
static uint64_t mapped_size;

static struct fnu_framebuffer *fbmode(void);

static void build_direct_palette(void) {
  const struct fnu_framebuffer *info = fbmode();
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
static void pointer_draw(void);
static void pointer_undraw(void);

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

static struct fnu_framebuffer fbinfo;
static int fbinfo_ready;

static struct fnu_framebuffer *fbmode(void) {
  const struct fnu_bootinfo *bi;

  if (!fbinfo_ready) {
    fbinfo_ready = 1;
    bi = bootinfo();
    if (bi && bi->console == FNU_CONSOLE_FRAMEBUFFER && bi->fb.base != 0)
      fbinfo = bi->fb;
  }
  return &fbinfo;
}

uint64_t framebuffer_phys(void) {
  struct fnu_framebuffer *info = fbmode();

  if (info->bpp < 15 || info->bpp > 32 || info->base == 0 ||
      info->width == 0 || info->height == 0 ||
      info->pitch < info->width * ((info->bpp + 7) / 8))
    return 0;
  return info->base;
}

uint64_t framebuffer_vram(void) {
  if ((dispi_read(VBE_DISPI_INDEX_ID) & 0xfff0) == 0xb0c0)
    return (uint64_t)dispi_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K) * 65536;
  return vesa_video_memory_known();
}

uint64_t framebuffer_size(void) {
  struct fnu_framebuffer *info = fbmode();
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

uint64_t framebuffer_boot_physical_base(void) {
  return fbmode()->base;
}

uint framebuffer_boot_width(void) { return fbmode()->width; }

uint framebuffer_boot_height(void) { return fbmode()->height; }

uint framebuffer_boot_pitch(void) { return fbmode()->pitch; }

uint framebuffer_boot_bits_per_pixel(void) {
  return fbmode()->bpp;
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

static void buffers_free(void) {
  contig_free(shadow); shadow = 0; shadow_bytes = 0;
  contig_free(dirty_lo); dirty_lo = 0;
  contig_free(dirty_hi); dirty_hi = 0;
  contig_free(rendered_cells); rendered_cells = 0;
  contig_free(dec_cells); dec_cells = 0;
  contig_free(scanline_buf64); scanline_buf64 = 0;
}

static int buffers_alloc(void) {
  dirty_lo = contig_alloc((uint64_t)height * sizeof(uint32_t));
  dirty_hi = contig_alloc((uint64_t)height * sizeof(uint32_t));
  rendered_cells = contig_alloc((uint64_t)columns * rows * sizeof(vtcell_t));
  dec_cells = contig_alloc((uint64_t)columns * sizeof(struct dec_cell));
  scanline_buf64 =
      contig_alloc((uint64_t)columns * (cell_width / 2) * sizeof(uint64_t));
  return dirty_lo && dirty_hi && rendered_cells && dec_cells && scanline_buf64;
}

static int framebuffer_reshape(int want_shadow) {
  buffers_free();
  if (!buffers_alloc()) {
    buffers_free();
    initialization_error = 4;
    return -1;
  }
  if (want_shadow && !on_gpu) {
    uint64_t need = (uint64_t)pitch * height;
    shadow = contig_alloc(need);
    if (shadow != 0) {
      shadow_bytes = need;
      memset(shadow, 0, need);
    }
  }
  canvas = shadow ? shadow : (uchar *)pixels;
  dirty_top = height;
  dirty_bot = 0;
  memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(vtcell_t));
  if (shadow != 0)
    mark_dirty(0, height, 0, pitch);
  return 0;
}

void framebuffer_shadow_init(void) {
  if (pixels == 0 || on_gpu)
    return;
  if (shadow == 0 && framebuffer_reshape(1) < 0) {
    printf("VIDEO: console arena too small for %dx%d\n", width, height);
    return;
  }
  if (shadow == 0)
    printf("VIDEO: no shadow, drawing straight to vram\n");
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
  struct fnu_framebuffer *info = fbmode();

  if (!framebuffer_phys()) {
    initialization_error = 1;
    return;
  }
  pixels = (volatile uchar *)framebuffer_virt();
  geometry(info->width, info->height, info->pitch, info->bpp);
  initialization_error = 0;
  canvas = (uchar *)pixels;
  on_gpu = 0;

  dispi_detected = (dispi_read(VBE_DISPI_INDEX_ID) & 0xfff0) == 0xb0c0;
  if (framebuffer_reshape(1) < 0) {
    pixels = 0;
    canvas = 0;
    return;
  }

  if (rendered_cells)
    memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(vtcell_t));
  dirty_reset();
  cursor_on = 0;
  memset(canvas, 0, (uint64_t)pitch * height);
}

void framebuffer_init_gpu(void *fb_mem, uint w, uint h, uint p) {

  pixels = (volatile uchar *)fb_mem;
  geometry(w, h, p, 32);
  initialization_error = 0;
  on_gpu = 1;
  dispi_detected = 0;
  if (framebuffer_reshape(0) < 0) {
    pixels = 0;
    canvas = 0;
    return;
  }
  canvas = (uchar *)fb_mem;
  if (rendered_cells)
    memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(vtcell_t));
  dirty_reset();
}

static inline uchar glyph_bits(const uchar *row, uint px) {
  return row[px >> 3];
}

static void draw_span_32(uint row, const vtcell_t *crt_row, uint c0, uint c1) {
  uint y0 = row * cell_height;
  uint qwords = (c1 - c0) * (cell_width / 2);
  uint gy, col, i, idx, px;

  for (col = c0; col < c1; col++) {
    vtcell_t cell = crt_row[col];
    uchar attr = CELL_ATTR(cell);
    uint64_t fg = vga_palette[attr & 0x0f];
    uint64_t bg = vga_palette[(attr >> 4) & 0x0f];
    dec_cells[col].lut[0] = (bg << 32) | bg;
    dec_cells[col].lut[1] = (fg << 32) | bg;
    dec_cells[col].lut[2] = (bg << 32) | fg;
    dec_cells[col].lut[3] = (fg << 32) | fg;
    dec_cells[col].glyph =
        font + (uint64_t)font_glyph(face, CELL_CP(cell)) * cell_height * font_stride;
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

static void draw_span_24(uint row, const vtcell_t *crt_row, uint c0, uint c1) {
  uint y0 = row * cell_height;
  uint gy, col, px;

  for (gy = 0; gy < cell_height; gy++) {
    uchar *line = canvas + (uint64_t)(y0 + gy) * pitch +
                  (uint64_t)c0 * cell_width * 3;

    for (col = c0; col < c1; col++) {
      vtcell_t cell = crt_row[col];
      uchar attr = CELL_ATTR(cell);
      uint32_t fg = vga_palette[attr & 0x0f];
      uint32_t bg = vga_palette[(attr >> 4) & 0x0f];
      const uchar *g = font + (uint64_t)font_glyph(face, CELL_CP(cell)) * cell_height *
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

static void draw_span_16(uint row, const vtcell_t *crt_row, uint c0, uint c1) {
  uint y0 = row * cell_height;
  uint gy, col, px;

  for (gy = 0; gy < cell_height; gy++) {
    uint16_t *line = (uint16_t *)(canvas + (uint64_t)(y0 + gy) * pitch +
                                  (uint64_t)c0 * cell_width * 2);

    for (col = c0; col < c1; col++) {
      vtcell_t cell = crt_row[col];
      uchar attr = CELL_ATTR(cell);
      uint16_t fg = direct_palette[attr & 0x0f];
      uint16_t bg = direct_palette[(attr >> 4) & 0x0f];
      const uchar *g = font + (uint64_t)font_glyph(face, CELL_CP(cell)) * cell_height *
                                  font_stride + (uint64_t)gy * font_stride;

      for (px = 0; px < cell_width; px++)
        *line++ = (glyph_bits(g, px) & (0x80 >> (px & 7))) ? fg : bg;
    }
  }
  mark_dirty(y0, y0 + cell_height, c0 * cell_width * 2, c1 * cell_width * 2);
}

void framebuffer_draw_row_span(uint row, const vtcell_t *crt_row, uint start_col, uint end_col) {
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

void framebuffer_draw_row(uint row, const vtcell_t *crt_row) {
  framebuffer_draw_row_span(row, crt_row, 0, columns);
}

void framebuffer_draw_cell(int cell_index, vtcell_t cell) {
  uint row, col;
  vtcell_t one = cell;

  if (!canvas || !rendered_cells || cell_index < 0 ||
      cell_index >= (int)(columns * rows))
    return;
  rendered_cells[cell_index] = cell;
  row = (uint)cell_index / columns;
  col = (uint)cell_index % columns;
  framebuffer_draw_row_span(row, &one - col, col, col + 1);
  fb_present();
}

void framebuffer_redraw_cells(const vtcell_t *cells, int count) {
  uint total_rows, r;

  if (!canvas || !rendered_cells || !cells)
    return;
  total_rows = ((uint)count + columns - 1) / columns;
  if (total_rows > rows)
    total_rows = rows;

  cursor_on = 0;
  ptr_drawn = 0;
  for (r = 0; r < total_rows; r++) {
    framebuffer_draw_row(r, cells + r * columns);
    memmove(rendered_cells + r * columns, cells + r * columns,
            columns * sizeof(vtcell_t));
  }
  pointer_draw();
  fb_present();
}

static void cursor_invert(int index) {
  uint row = (uint)index / columns, col = (uint)index % columns;
  uint x0 = col * cell_width, y0 = row * cell_height, gy, px;

  if (!canvas || index < 0 || (uint)index >= columns * rows)
    return;
  for (gy = 0; gy < cell_height; gy++) {
    uchar *line = canvas + (uint64_t)(y0 + gy) * pitch +
                  (uint64_t)x0 * bytes_per_pixel;

    if (bytes_per_pixel == 4) {
      uint32_t *p = (uint32_t *)line;
      for (px = 0; px < cell_width; px++)
        p[px] ^= 0x00ffffffu;
    } else if (bytes_per_pixel == 2) {
      uint16_t *p = (uint16_t *)line;
      for (px = 0; px < cell_width; px++)
        p[px] ^= 0xffffu;
    } else {
      for (px = 0; px < cell_width * 3; px++)
        line[px] ^= 0xff;
    }
  }
  mark_dirty(y0, y0 + cell_height, x0 * bytes_per_pixel,
             (x0 + cell_width) * bytes_per_pixel);
}

static void cursor_hide(void) {
  if (cursor_on) {
    cursor_invert(cursor_index);
    cursor_on = 0;
  }
}

static void cursor_show(int index) {
  if (index < 0 || (uint)index >= columns * rows)
    return;
  cursor_index = index;
  cursor_invert(index);
  cursor_on = 1;
}

#define PTR_W 10
#define PTR_H 16
static const uchar ptr_map[PTR_H][2] = {
    {0x00, 0x00}, {0x40, 0x00}, {0x60, 0x00}, {0x70, 0x00},
    {0x78, 0x00}, {0x7c, 0x00}, {0x7e, 0x00}, {0x7f, 0x00},
    {0x7f, 0x80}, {0x7c, 0x00}, {0x6c, 0x00}, {0x46, 0x00},
    {0x06, 0x00}, {0x03, 0x00}, {0x03, 0x00}, {0x00, 0x00}};
static const uchar ptr_mask[PTR_H][2] = {
    {0xc0, 0x00}, {0xe0, 0x00}, {0xf0, 0x00}, {0xf8, 0x00},
    {0xfc, 0x00}, {0xfe, 0x00}, {0xff, 0x00}, {0xff, 0x80},
    {0xff, 0xc0}, {0xff, 0xc0}, {0xfe, 0x00}, {0xef, 0x00},
    {0xcf, 0x00}, {0x07, 0x80}, {0x07, 0x80}, {0x03, 0x00}};

static void put_pixel(uint x, uint y, uint color) {
  uchar *p = canvas + (uint64_t)y * pitch + (uint64_t)x * bytes_per_pixel;

  if (bytes_per_pixel == 4) {
    *(uint32_t *)p = vga_palette[color];
  } else if (bytes_per_pixel == 2) {
    *(uint16_t *)p = direct_palette[color];
  } else {
    p[0] = (uchar)vga_palette[color];
    p[1] = (uchar)(vga_palette[color] >> 8);
    p[2] = (uchar)(vga_palette[color] >> 16);
  }
}

static void pointer_undraw(void) {
  uint c0, c1, r0, r1, r, c;

  if (!ptr_drawn)
    return;
  ptr_drawn = 0;
  c0 = (uint)ptr_drawn_x / cell_width;
  r0 = (uint)ptr_drawn_y / cell_height;
  c1 = ((uint)ptr_drawn_x + PTR_W - 1) / cell_width;
  r1 = ((uint)ptr_drawn_y + PTR_H - 1) / cell_height;
  for (r = r0; r <= r1 && r < rows; r++)
    for (c = c0; c <= c1 && c < columns; c++)
      rendered_cells[r * columns + c] = 0xffffffffu;
}

static void pointer_draw(void) {
  uint x, y, bit, w = columns * cell_width, h = rows * cell_height;

  if (!ptr_shown || (uint)ptr_x >= w || (uint)ptr_y >= h)
    return;
  for (y = 0; y < PTR_H && ptr_y + y < h; y++)
    for (x = 0; x < PTR_W && ptr_x + x < w; x++) {
      bit = 0x80u >> (x & 7);
      if (!(ptr_mask[y][x >> 3] & bit))
        continue;
      put_pixel(ptr_x + x, ptr_y + y, ptr_map[y][x >> 3] & bit ? 15 : 0);
    }
  mark_dirty(ptr_y, ptr_y + PTR_H, ptr_x * bytes_per_pixel,
             (ptr_x + PTR_W) * bytes_per_pixel);
  ptr_drawn_x = ptr_x;
  ptr_drawn_y = ptr_y;
  ptr_drawn = 1;
}

void framebuffer_pointer(int x, int y, int shown) {
  ptr_x = x;
  ptr_y = y;
  ptr_shown = shown;
}

uint framebuffer_cell_width(void) { return cell_width ? cell_width : 8; }
uint framebuffer_cell_height(void) { return cell_height ? cell_height : 16; }

void framebuffer_flush_cells(const vtcell_t *cells, int count, int cursor) {
  uint total_rows, r, min_col, max_col;

  if (!canvas || !rendered_cells || !cells)
    return;
  total_rows = ((uint)count + columns - 1) / columns;
  if (total_rows > rows)
    total_rows = rows;

  cursor_hide();
  pointer_undraw();
  for (r = 0; r < total_rows; r++) {
    const vtcell_t *cur = cells + r * columns;
    vtcell_t *rend = rendered_cells + r * columns;
    if (memcmp(cur, rend, columns * sizeof(vtcell_t)) == 0)
      continue;
    min_col = 0;
    while (min_col < columns && cur[min_col] == rend[min_col])
      min_col++;
    max_col = columns - 1;
    while (max_col > min_col && cur[max_col] == rend[max_col])
      max_col--;
    framebuffer_draw_row_span(r, cur, min_col, max_col + 1);
    memmove(rend + min_col, cur + min_col,
            (max_col - min_col + 1) * sizeof(vtcell_t));
  }
  cursor_show(cursor);
  pointer_draw();
  fb_present();
}

void framebuffer_clear(void) {
  if (!canvas || !rendered_cells)
    return;
  memset(canvas, 0, (uint64_t)pitch * height);
  if (rendered_cells)
    memset(rendered_cells, 0xff, (uint64_t)columns * rows * sizeof(vtcell_t));
  cursor_on = 0;
  ptr_drawn = 0;
  mark_dirty(0, height, 0, pitch);
  fb_present();
}

void framebuffer_scroll_lines(int n, vtcell_t fill_cell) {
  uint moved, blank, y;
  uchar attr;
  uint32_t bg;

  if (!canvas || !rendered_cells || n <= 0)
    return;
  if (n >= (int)rows) {
    framebuffer_clear();
    return;
  }

  cursor_hide();
  pointer_undraw();
  blank = (uint)n * cell_height;
  moved = rows * cell_height - blank;
  memmove(canvas, canvas + (uint64_t)blank * pitch, (uint64_t)moved * pitch);

  attr = CELL_ATTR(fill_cell);
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
          (rows - (uint)n) * columns * sizeof(vtcell_t));
  for (y = (rows - (uint)n) * columns; y < rows * columns; y++)
    rendered_cells[y] = fill_cell;

  mark_dirty(0, moved + blank, 0, pitch);
}

void framebuffer_scroll_up(void) { framebuffer_scroll_lines(1, ' ' | 0x0700); }

static int vesa_mode_now = -1;

static int display_set_vesa_mode(uint w, uint h) {
  struct vesa_modeinfo mi;
  uint ow = width, oh = height, op = pitch, obpp = bytes_per_pixel * 8;
  uint32_t obase;
  uint64_t need;

  if (!vesa_available())
    return -1;
  obase = fbmode()->base;
  if (vesa_set_mode(w, h, &mi) < 0)
    return -1;
  if (mi.phys_base == 0 || mi.pitch < mi.width * (mi.bpp / 8)) {
    vesa_set_mode(ow, oh, &mi);
    return -1;
  }

  fbmode()->base = mi.phys_base;
  fbmode()->width = mi.width;
  fbmode()->height = mi.height;
  fbmode()->pitch = mi.pitch;
  fbmode()->bpp = mi.bpp;
  fbmode()->red_pos = mi.red_pos;
  fbmode()->red_size = mi.red_size;
  fbmode()->green_pos = mi.green_pos;
  fbmode()->green_size = mi.green_size;
  fbmode()->blue_pos = mi.blue_pos;
  fbmode()->blue_size = mi.blue_size;
  need = (uint64_t)mi.pitch * mi.height;
  if (vesa_video_memory() > need)
    need = vesa_video_memory();
  pixels = (volatile uchar *)framebuffer_remap(mi.phys_base, need);
  if (pixels == 0) {
    fbmode()->base = obase;
    fbmode()->width = ow;
    fbmode()->height = oh;
    fbmode()->pitch = op;
    fbmode()->bpp = obpp;
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
  vesa_mode_now = mi.mode;
  return 0;
}

// screen back after S3?
int display_back_after_s3(void) {
  return virtio_gpu_available() || dispi_detected || pixels == 0 || display_resume_bios();
}

// for the VIDEO line
const char *display_resume_how(void) {
  if (virtio_gpu_available())
    return "by the virtio device";
  if (dispi_detected)
    return "by its own registers";
  if (display_resume_bios())
    return realmode_available() ? "by the video bios in real mode" : "by the video bios in x86emu";
  return "dark, no video bios to post";
}

// SLEEP
// bios POST or not? under UEFI only the rom the loader kept works
int display_resume_bios(void) {
  if (virtio_gpu_available())
    return 0;
  if (!realmode_available())
    return !dispi_detected && pixels != 0 && x86bios_has_rom();
  return pixels == 0 || (!dispi_detected && vesa_available());
}

// the bios's record of the adapter, for a resume without POST
void display_suspend(void) {
  if (display_resume_bios() && pixels != 0 && vesa_available() && vesa_state_save() < 0)
    printf("display: the video bios keeps no state to come back to\n");
}

static int fits(uint16_t mode __unused, uint w, uint h, uint bpp, void *arg) {
  uint *best = arg;

  if (w <= width && h <= height && bpp >= 24 && w * h > best[0] * best[1]) {
    best[0] = w;
    best[1] = h;
  }
  return 0;
}

// old mode, else the biggest the bios has
static void resume_mode(void) {
  struct vesa_modeinfo mi;
  uint best[2] = {0, 0};

  if (vesa_set_mode(width, height, &mi) == 0 && mi.phys_base == fbmode()->base &&
      mi.pitch == pitch && mi.bpp == bytes_per_pixel * 8)
    return;
  if (display_set_vesa_mode(width, height) == 0) {
    console_redraw();
    return;
  }
  vesa_each_mode(fits, best);
  if (best[0] != 0 && display_set_vesa_mode(best[0], best[1]) == 0)
    console_redraw();
  else
    printf("display: the video bios has no mode for the console\n");
}

// POST again (x86emu), mode, saved state
// after S4 under UEFI the GOP did it already
void display_resume(int state, int posted) {
  struct realregs r;
  int bios = display_resume_bios() && (state == 3 || realmode_available());

  if (virtio_gpu_available())
    return;
  if (bios && !posted) {
    pm_trace_step(PMT_EMUPOST, 0);
    if (x86bios_post() < 0) {
      printf("display: the video bios could not be posted again, not calling it\n");
      return;
    }
  }
  if (pixels == 0) {
    if (realmode_available()) {
      memset(&r, 0, sizeof(r));
      r.ax = 0x0003;
      video_bios_int(&r);
    }
    console_redraw();
    return;
  }
  if (dispi_detected) {
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    dispi_write(VBE_DISPI_INDEX_BPP, (uint16_t)(bytes_per_pixel * 8));
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)(pitch / bytes_per_pixel));
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0x01 | 0x40);
    inb(0x3da);
    outb(0x3c0, 0x20);
  } else if (bios && vesa_available()) {
    resume_mode();
  }
  if (bios) {
    pm_trace_step(PMT_VBESTATE, 0);
    vesa_state_restore();
  }
  dirty_reset();
  mark_dirty(0, height, 0, pitch);
  fb_present();
}

int display_set_resolution(uint w, uint h) {
  uint ow = width, oh = height, op = pitch;
  uint32_t new_pitch = w * 4;
  uint64_t need = (uint64_t)new_pitch * h;
  uint64_t vram;

  if (virtio_gpu_available())
    return virtio_gpu_set_resolution(w, h);
  if (!pixels)
    return -1;
  if (w < 320 || h < 200 || w > 8192 || h > 8192 || (w % 8) != 0)
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
  printf("VBE: resolution changed to %dx%d\n", w, h);
  return 0;
}

int display_can_modeset(void) {
  return virtio_gpu_available() ||
         (pixels != 0 && (dispi_detected || realmode_available()));
}

#define ANYMODE_MAX 64
static struct {
  uint16_t w, h;
} anymodes[ANYMODE_MAX];
static int nanymodes;

static int listed(void) {
  return !virtio_gpu_available() && !dispi_detected && pixels != 0 &&
         realmode_available() && vesa_available();
}

static int anymode_number(uint w, uint h) {
  int i;

  for (i = 0; i < nanymodes; i++)
    if (anymodes[i].w == w && anymodes[i].h == h)
      return FNU_M_VESA_BASE + i;
  if (nanymodes == ANYMODE_MAX)
    return -1;
  anymodes[nanymodes].w = (uint16_t)w;
  anymodes[nanymodes].h = (uint16_t)h;
  return FNU_M_VESA_BASE + nanymodes++;
}

static void mode_fill(struct fnu_video_info *vi, int mode, uint w, uint h,
                      uint bpp) {
  const struct fnu_framebuffer *info = fbmode();
  const struct console_font *f = pick_face(w);
  uint cw, ch;

  memset(vi, 0, sizeof(*vi));
  vi->vi_mode = mode;
  vi->vi_flags = FNU_V_INFO_COLOR | FNU_V_INFO_GRAPHICS | FNU_V_INFO_LINEAR |
                 (listed() ? FNU_V_INFO_VESA : FNU_V_INFO_NONVGA);
  vi->vi_width = (int)w;
  vi->vi_height = (int)h;
  vi->vi_cwidth = (int)f->width;
  vi->vi_cheight = (int)f->height;
  vi->vi_depth = (int)bpp;
  vi->vi_planes = 1;
  vi->vi_mem_model = FNU_V_INFO_MM_DIRECT;
  vi->vi_pixel_size = (int)((bpp + 7) / 8);
  cw = w;
  ch = h;
  display_get_resolution(&cw, &ch, 0);
  if (cw == w && ch == h) {
    vi->vi_buffer = framebuffer_phys();
    vi->vi_buffer_size = (uint64_t)pitch * height;
    vi->vi_pixel_fields[0] = info->red_pos;
    vi->vi_pixel_fields[1] = info->green_pos;
    vi->vi_pixel_fields[2] = info->blue_pos;
    vi->vi_pixel_fsizes[0] = info->red_size;
    vi->vi_pixel_fsizes[1] = info->green_size;
    vi->vi_pixel_fsizes[2] = info->blue_size;
  }
}

static int vesa_current_number(void) {
  uint w, h, bpp;
  int i;

  if (vesa_mode_now >= 0)
    return vesa_mode_now;
  display_get_resolution(&w, &h, &bpp);
  for (i = FNU_M_VESA_BASE; i <= FNU_M_VESA_MODE_MAX; i++) {
    uint mw, mh, mbpp;

    if (vesa_mode_lookup((uint16_t)i, &mw, &mh, &mbpp) == 0 && mw == w &&
        mh == h && mbpp == bpp)
      return vesa_mode_now = i;
  }
  return -1;
}

// FNU_FBGETMODE
int display_mode_current(void) {
  uint w, h;

  if (listed())
    return vesa_current_number();
  display_get_resolution(&w, &h, 0);
  return anymode_number(w, h);
}

// FNU_FBMODEINFO
int display_mode_info(int mode, struct fnu_video_info *vi) {
  uint w, h, bpp;

  if (listed()) {
    if (mode < FNU_M_VESA_BASE || mode > FNU_M_VESA_MODE_MAX ||
        vesa_mode_lookup((uint16_t)mode, &w, &h, &bpp) < 0)
      return -1;
    mode_fill(vi, mode, w, h, bpp);
    return 0;
  }
  display_mode_current();
  if (mode < FNU_M_VESA_BASE || mode >= FNU_M_VESA_BASE + nanymodes)
    return -1;
  display_get_resolution(0, 0, &bpp);
  mode_fill(vi, mode, anymodes[mode - FNU_M_VESA_BASE].w,
            anymodes[mode - FNU_M_VESA_BASE].h, bpp);
  return 0;
}

// FNU_FBFINDMODE
int display_mode_find(uint w, uint h) {
  uint cw, ch;
  int i, best = -1;
  uint bestbpp = 0;

  if (listed()) {
    for (i = FNU_M_VESA_BASE; i <= FNU_M_VESA_MODE_MAX; i++) {
      uint mw, mh, mbpp;

      if (vesa_mode_lookup((uint16_t)i, &mw, &mh, &mbpp) == 0 && mw == w &&
          mh == h && mbpp > bestbpp) {
        best = i;
        bestbpp = mbpp;
      }
    }
    return best;
  }
  display_get_resolution(&cw, &ch, 0);
  if (!display_can_modeset() && (cw != w || ch != h))
    return -1;
  display_mode_current();
  return anymode_number(w, h);
}

// FNU_FBSETMODE
int display_mode_set(int mode) {
  struct fnu_video_info vi;

  if (display_mode_info(mode, &vi) < 0)
    return -1;
  return display_set_resolution((uint)vi.vi_width, (uint)vi.vi_height);
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
