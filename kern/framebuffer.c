#include "defs.h"
#include "inc/framebuffer.h"
#include "memlayout.h"
#include "virtio_gpu.h"
#include "x86.h"

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

static volatile uchar *pixels;
static const uchar *font;
static uint width;
static uint height;
static uint pitch;
static uint bytes_per_pixel;
static uint columns;
static uint rows;
static uint scale;
static uint cell_width;
static uint cell_height;
static int initialization_error;

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF
#define VBE_DISPI_INDEX_ID     0x0
#define VBE_DISPI_INDEX_XRES   0x1
#define VBE_DISPI_INDEX_YRES   0x2
#define VBE_DISPI_INDEX_BPP    0x3
#define VBE_DISPI_INDEX_ENABLE 0x4
#define VBE_DISPI_INDEX_BANK   0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH 0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_X_OFFSET 0x8
#define VBE_DISPI_INDEX_Y_OFFSET 0x9

static inline void dispi_write(uint16_t reg, uint16_t val) {
  outw(VBE_DISPI_IOPORT_INDEX, reg);
  outw(VBE_DISPI_IOPORT_DATA, val);
}

static inline uint16_t dispi_read(uint16_t reg) {
  outw(VBE_DISPI_IOPORT_INDEX, reg);
  return inw(VBE_DISPI_IOPORT_DATA);
}

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa,
    0xaa5500, 0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55, 0x55ffff,
    0xff5555, 0xff55ff, 0xffff55, 0xffffff,
};

static struct boot_framebuffer_info *boot_framebuffer_info(void) {
  return (struct boot_framebuffer_info *)P2V(BOOT_FRAMEBUFFER_INFO_PHYS);
}

uint32_t framebuffer_phys(void) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();
  uint64_t size;

  if (info->magic != BOOT_FRAMEBUFFER_MAGIC ||
      (info->bits_per_pixel != 24 && info->bits_per_pixel != 32) ||
      info->physical_base == 0 || info->width == 0 || info->height == 0 ||
      info->width > 1920 || info->height > 1080 ||
      info->pitch < (uint)info->width * (info->bits_per_pixel / 8))
    return 0;
  size = (uint64_t)info->pitch * info->height;
  if ((uint64_t)info->physical_base + size > 0x100000000ULL)
    return 0;
  return info->physical_base;
}

uint framebuffer_size(void) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();
  uint32_t phys = framebuffer_phys();
  if (!phys)
    return 0;
  outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
  uint16_t id = inw(VBE_DISPI_IOPORT_DATA);
  if ((id & 0xfff0) == 0xb0c0) {
    uint64_t max_size = 16 * 1024 * 1024; // 16 MB VRAM for hardware panning
    if ((uint64_t)phys + max_size <= 0x100000000ULL)
      return (uint)max_size;
  }
  return (uint)info->pitch * info->height;
}

int framebuffer_available(void) { return pixels != 0; }

uint32_t framebuffer_boot_tag(void) { return boot_framebuffer_info()->magic; }

uint32_t framebuffer_boot_physical_base(void) {
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

void *framebuffer_virt(void) {
  uint32_t physical_base = framebuffer_phys();
  return physical_base ? DEVSPACE_P2V(physical_base) : 0;
}

#define FB_MAX_COLUMNS 320
#define FB_MAX_ROWS 100
#define FB_MAX_WIDTH 3840

static ushort rendered_cells[FB_MAX_COLUMNS * FB_MAX_ROWS];
static int dispi_detected = 0;
static uint fb_y_offset = 0;
static uint fb_max_y_offset = 0;

static struct {
  const uchar *glyph;
  uint64_t lut[4];
} dec_cells[FB_MAX_COLUMNS];

static uint64_t scanline_buf64[FB_MAX_COLUMNS * 8];
static uint32_t universal_scanline_buf32[FB_MAX_WIDTH];
static uchar universal_scanline_buf24[FB_MAX_WIDTH * 3];

int framebuffer_has_dispi(void) {
  return dispi_detected;
}

static inline void putpixel(uint x, uint y, uint32_t color) {
  volatile uchar *pixel;

  if (x >= width || y >= height)
    return;
  pixel = pixels + (fb_y_offset + y) * pitch + x * bytes_per_pixel;
  if (bytes_per_pixel == 4) {
    *(volatile uint32_t *)pixel = color;
  } else {
    pixel[0] = color;
    pixel[1] = color >> 8;
    pixel[2] = color >> 16;
  }
}

void framebuffer_init(void) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();
  uint sx, sy;

  if (!framebuffer_phys()) {
    initialization_error = 1;
    return;
  }
  if (info->font_physical_base == 0) {
    initialization_error = 2;
    return;
  }
  pixels = (volatile uchar *)framebuffer_virt();
  font = (const uchar *)P2V((uintptr_t)info->font_physical_base);
  width = info->width;
  height = info->height;
  pitch = info->pitch;
  bytes_per_pixel = info->bits_per_pixel / 8;
  sx = width / (80 * FONT_WIDTH);
  sy = height / (25 * FONT_HEIGHT);
  scale = sx < sy ? sx : sy;
  if (scale == 0) {
    pixels = 0;
    initialization_error = 3;
    return;
  }
  if (scale > 8)
    scale = 8;
  columns = width / (FONT_WIDTH * scale);
  rows = height / (FONT_HEIGHT * scale);
  cell_width = FONT_WIDTH * scale;
  cell_height = FONT_HEIGHT * scale;
  initialization_error = 0;

  memset(rendered_cells, 0xff, sizeof(rendered_cells));

  fb_y_offset = 0;
  fb_max_y_offset = 0;

  uint16_t dispi_id = dispi_read(VBE_DISPI_INDEX_ID);
  if ((dispi_id & 0xfff0) == 0xb0c0) {
    dispi_detected = 1;
    uint max_lines = 16777216 / pitch;
    if (max_lines > 4096)
      max_lines = 4096;
    if (max_lines >= height * 2) {
      dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)max_lines);
      dispi_write(VBE_DISPI_INDEX_X_OFFSET, 0);
      dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
      fb_max_y_offset = max_lines - height;
    }
  }

  uint total_lines = (dispi_detected && fb_max_y_offset > 0) ? (fb_max_y_offset + height) : height;
  memset((void *)pixels, 0, pitch * total_lines);
}

void framebuffer_init_gpu(void *fb_mem, uint w, uint h, uint p) {
  struct boot_framebuffer_info *info = boot_framebuffer_info();
  if (info->font_physical_base) {
    font = (const uchar *)P2V((uintptr_t)info->font_physical_base);
  }
  pixels = (volatile uchar *)fb_mem;
  width = w;
  height = h;
  pitch = p;
  bytes_per_pixel = 4;
  uint sx = width / (80 * FONT_WIDTH);
  uint sy = height / (25 * FONT_HEIGHT);
  scale = sx < sy ? sx : sy;
  if (scale < 1)
    scale = 1;
  if (scale > 8)
    scale = 8;
  columns = width / (FONT_WIDTH * scale);
  rows = height / (FONT_HEIGHT * scale);
  if (columns > FB_MAX_COLUMNS)
    columns = FB_MAX_COLUMNS;
  if (rows > FB_MAX_ROWS)
    rows = FB_MAX_ROWS;
  cell_width = FONT_WIDTH * scale;
  cell_height = FONT_HEIGHT * scale;
  initialization_error = 0;
  dispi_detected = 0;
  fb_y_offset = 0;
  fb_max_y_offset = 0;
  memset(rendered_cells, 0xff, sizeof(rendered_cells));
}

static inline void draw_cell_scale1_32(uint col, uint row, uchar ch, uint32_t fg, uint32_t bg) {
  const uchar *glyph = font + ((uint)ch * FONT_HEIGHT);
  volatile uchar *row_ptr = pixels + ((fb_y_offset + row * FONT_HEIGHT) * pitch) + (col * (FONT_WIDTH * 4));

  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    volatile uint32_t *line = (volatile uint32_t *)row_ptr;
    uchar bits = glyph[gy];
    line[0] = (bits & 0x80) ? fg : bg;
    line[1] = (bits & 0x40) ? fg : bg;
    line[2] = (bits & 0x20) ? fg : bg;
    line[3] = (bits & 0x10) ? fg : bg;
    line[4] = (bits & 0x08) ? fg : bg;
    line[5] = (bits & 0x04) ? fg : bg;
    line[6] = (bits & 0x02) ? fg : bg;
    line[7] = (bits & 0x01) ? fg : bg;
    row_ptr += pitch;
  }
}

static inline void draw_cell_scale2_32(uint col, uint row, uchar ch, uint32_t fg, uint32_t bg) {
  const uchar *glyph = font + ((uint)ch * FONT_HEIGHT);
  volatile uchar *row_ptr = pixels + ((fb_y_offset + row * (FONT_HEIGHT * 2)) * pitch) + (col * (FONT_WIDTH * 2 * 4));

  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uchar bits = glyph[gy];
    volatile uint32_t *line1 = (volatile uint32_t *)row_ptr;
    volatile uint32_t *line2 = (volatile uint32_t *)(row_ptr + pitch);

    for (int gx = 0; gx < 8; gx++) {
      uint32_t c = (bits & (0x80 >> gx)) ? fg : bg;
      line1[gx * 2] = c;
      line1[gx * 2 + 1] = c;
      line2[gx * 2] = c;
      line2[gx * 2 + 1] = c;
    }
    row_ptr += pitch * 2;
  }
}

static void draw_cell_generic(uint col, uint row, uchar ch, uint32_t fg, uint32_t bg) {
  const uchar *glyph = font + ((uint)ch * FONT_HEIGHT);
  uint x0 = col * cell_width;
  uint y0 = fb_y_offset + row * cell_height;

  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uchar bits = glyph[gy];
    for (uint dy = 0; dy < scale; dy++) {
      uint y = y0 + gy * scale + dy;
      for (uint gx = 0; gx < FONT_WIDTH; gx++) {
        uint32_t c = (bits & (0x80 >> gx)) ? fg : bg;
        for (uint dx = 0; dx < scale; dx++) {
          putpixel(x0 + gx * scale + dx, y, c);
        }
      }
    }
  }
}

static void framebuffer_draw_row_span_scale1_32(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  uint y0 = fb_y_offset + row * FONT_HEIGHT;
  uint span_cols = end_col - start_col;
  uint qwords = span_cols * 4;

  for (uint col = start_col; col < end_col; col++) {
    ushort cell = crt_row[col];
    uchar attr = cell >> 8;
    uint64_t fg = vga_palette[attr & 0x0f];
    uint64_t bg = vga_palette[(attr >> 4) & 0x0f];
    dec_cells[col].lut[0] = (bg << 32) | bg;
    dec_cells[col].lut[1] = (fg << 32) | bg;
    dec_cells[col].lut[2] = (bg << 32) | fg;
    dec_cells[col].lut[3] = (fg << 32) | fg;
    dec_cells[col].glyph = font + ((uint)(cell & 0xff) * FONT_HEIGHT);
  }

  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uint idx = 0;
    for (uint col = start_col; col < end_col; col++) {
      uchar bits = dec_cells[col].glyph[gy];
      const uint64_t *lut = dec_cells[col].lut;
      scanline_buf64[idx++] = lut[(bits >> 6) & 3];
      scanline_buf64[idx++] = lut[(bits >> 4) & 3];
      scanline_buf64[idx++] = lut[(bits >> 2) & 3];
      scanline_buf64[idx++] = lut[bits & 3];
    }
    volatile uint64_t *dst64 = (volatile uint64_t *)(pixels + (y0 + gy) * pitch + start_col * (FONT_WIDTH * 4));
    for (uint i = 0; i < qwords; i++)
      dst64[i] = scanline_buf64[i];
  }
}

static void framebuffer_draw_row_span_scale2_32(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  uint y0 = fb_y_offset + row * (FONT_HEIGHT * 2);
  uint span_cols = end_col - start_col;
  uint qwords = span_cols * 8;

  for (uint col = start_col; col < end_col; col++) {
    ushort cell = crt_row[col];
    uchar attr = cell >> 8;
    uint64_t fg = vga_palette[attr & 0x0f];
    uint64_t bg = vga_palette[(attr >> 4) & 0x0f];
    dec_cells[col].lut[0] = (bg << 32) | bg;
    dec_cells[col].lut[1] = (fg << 32) | fg;
    dec_cells[col].glyph = font + ((uint)(cell & 0xff) * FONT_HEIGHT);
  }

  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uint idx = 0;
    for (uint col = start_col; col < end_col; col++) {
      uchar bits = dec_cells[col].glyph[gy];
      const uint64_t *lut = dec_cells[col].lut;
      scanline_buf64[idx++] = lut[(bits >> 7) & 1];
      scanline_buf64[idx++] = lut[(bits >> 6) & 1];
      scanline_buf64[idx++] = lut[(bits >> 5) & 1];
      scanline_buf64[idx++] = lut[(bits >> 4) & 1];
      scanline_buf64[idx++] = lut[(bits >> 3) & 1];
      scanline_buf64[idx++] = lut[(bits >> 2) & 1];
      scanline_buf64[idx++] = lut[(bits >> 1) & 1];
      scanline_buf64[idx++] = lut[bits & 1];
    }
    for (uint dy = 0; dy < 2; dy++) {
      volatile uint64_t *dst64 = (volatile uint64_t *)(pixels + (y0 + gy * 2 + dy) * pitch + start_col * (FONT_WIDTH * 2 * 4));
      for (uint i = 0; i < qwords; i++)
        dst64[i] = scanline_buf64[i];
    }
  }
}

static void framebuffer_draw_row_span_scale1_24(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    volatile uchar *line = pixels + (fb_y_offset + row * FONT_HEIGHT + gy) * pitch + start_col * (FONT_WIDTH * 3);
    for (uint col = start_col; col < end_col; col++) {
      ushort cell = crt_row[col];
      uchar ch = cell & 0xff;
      uchar attr = cell >> 8;
      uint32_t fg = vga_palette[attr & 0x0f];
      uint32_t bg = vga_palette[(attr >> 4) & 0x0f];
      uchar bits = font[((uint)ch * FONT_HEIGHT) + gy];
      for (int bit = 7; bit >= 0; bit--) {
        uint32_t c = (bits & (1 << bit)) ? fg : bg;
        line[0] = c;
        line[1] = c >> 8;
        line[2] = c >> 16;
        line += 3;
      }
    }
  }
}

static void framebuffer_draw_row_span_universal_32(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  uint y0 = fb_y_offset + row * cell_height;
  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uint px = 0;
    for (uint col = start_col; col < end_col; col++) {
      ushort cell = crt_row[col];
      uchar ch = cell & 0xff;
      uchar attr = cell >> 8;
      uint32_t fg = vga_palette[attr & 0x0f];
      uint32_t bg = vga_palette[(attr >> 4) & 0x0f];
      uchar bits = font[((uint)ch * FONT_HEIGHT) + gy];
      for (int gx = 0; gx < 8; gx++) {
        uint32_t c = (bits & (0x80 >> gx)) ? fg : bg;
        for (uint dx = 0; dx < scale; dx++) {
          if (px < FB_MAX_WIDTH)
            universal_scanline_buf32[px++] = c;
        }
      }
    }
    for (uint dy = 0; dy < scale; dy++) {
      uint y = y0 + gy * scale + dy;
      if (y < height) {
        volatile uint32_t *dst = (volatile uint32_t *)(pixels + y * pitch) + start_col * cell_width;
        for (uint x = 0; x < px && (start_col * cell_width + x) < width; x++)
          dst[x] = universal_scanline_buf32[x];
      }
    }
  }
}

static void framebuffer_draw_row_span_universal_24(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  uint y0 = fb_y_offset + row * cell_height;
  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uint px = 0;
    for (uint col = start_col; col < end_col; col++) {
      ushort cell = crt_row[col];
      uchar ch = cell & 0xff;
      uchar attr = cell >> 8;
      uint32_t fg = vga_palette[attr & 0x0f];
      uint32_t bg = vga_palette[(attr >> 4) & 0x0f];
      uchar bits = font[((uint)ch * FONT_HEIGHT) + gy];
      for (int gx = 0; gx < 8; gx++) {
        uint32_t c = (bits & (0x80 >> gx)) ? fg : bg;
        uchar b = (uchar)c;
        uchar g = (uchar)(c >> 8);
        uchar r = (uchar)(c >> 16);
        for (uint dx = 0; dx < scale; dx++) {
          if (px + 3 <= sizeof(universal_scanline_buf24)) {
            universal_scanline_buf24[px++] = b;
            universal_scanline_buf24[px++] = g;
            universal_scanline_buf24[px++] = r;
          }
        }
      }
    }
    for (uint dy = 0; dy < scale; dy++) {
      uint y = y0 + gy * scale + dy;
      if (y < height) {
        volatile uchar *dst = pixels + y * pitch + start_col * cell_width * 3;
        uint max_bytes = width * 3;
        for (uint x = 0; x < px && (start_col * cell_width * 3 + x) < max_bytes; x++)
          dst[x] = universal_scanline_buf24[x];
      }
    }
  }
}


void framebuffer_draw_row_span(uint row, const ushort *crt_row, uint start_col, uint end_col) {
  if (!pixels || !crt_row || row >= rows || start_col >= end_col)
    return;
  if (end_col > columns)
    end_col = columns;

  if (bytes_per_pixel == 4) {
    if (scale == 1) {
      framebuffer_draw_row_span_scale1_32(row, crt_row, start_col, end_col);
    } else if (scale == 2) {
      framebuffer_draw_row_span_scale2_32(row, crt_row, start_col, end_col);
    } else {
      framebuffer_draw_row_span_universal_32(row, crt_row, start_col, end_col);
    }
  } else if (bytes_per_pixel == 3) {
    if (scale == 1) {
      framebuffer_draw_row_span_scale1_24(row, crt_row, start_col, end_col);
    } else {
      framebuffer_draw_row_span_universal_24(row, crt_row, start_col, end_col);
    }
  } else {
    framebuffer_draw_row_span_universal_32(row, crt_row, start_col, end_col);
  }
}

void framebuffer_draw_row(uint row, const ushort *crt_row) {
  framebuffer_draw_row_span(row, crt_row, 0, columns);
}

void framebuffer_draw_cell(int cell_index, ushort cell) {
  if (!pixels || cell_index < 0 || cell_index >= (int)(columns * rows))
    return;

  if (cell_index < FB_MAX_COLUMNS * FB_MAX_ROWS)
    rendered_cells[cell_index] = cell;

  uint row = (uint)cell_index / columns;
  uint col = (uint)cell_index % columns;
  ushort single_cell = cell;
  framebuffer_draw_row_span(row, &single_cell - col, col, col + 1);
}

void framebuffer_redraw_cells(const ushort *cells, int count) {
  if (!pixels || !cells)
    return;

  uint total_rows = (count + columns - 1) / columns;
  if (total_rows > rows)
    total_rows = rows;

  for (uint r = 0; r < total_rows; r++) {
    const ushort *cur_row = cells + r * columns;
    framebuffer_draw_row(r, cur_row);
    if (r * columns < FB_MAX_COLUMNS * FB_MAX_ROWS) {
      uint copy_count = columns;
      if ((r + 1) * columns > FB_MAX_COLUMNS * FB_MAX_ROWS)
        copy_count = (FB_MAX_COLUMNS * FB_MAX_ROWS) - (r * columns);
      memmove(rendered_cells + r * columns, cur_row, copy_count * sizeof(ushort));
    }
  }
  if (virtio_gpu_available()) {
    virtio_gpu_flush_all();
  }
}

void framebuffer_flush_cells(const ushort *cells, int count) {
  if (!pixels || !cells)
    return;

  uint total_rows = (count + columns - 1) / columns;
  if (total_rows > rows)
    total_rows = rows;

  int min_row = -1, max_row = -1;
  for (uint r = 0; r < total_rows; r++) {
    const ushort *cur_row = cells + r * columns;
    ushort *rend_row = rendered_cells + r * columns;
    if (memcmp(cur_row, rend_row, columns * sizeof(ushort)) != 0) {
      uint min_col = 0;
      while (min_col < columns && cur_row[min_col] == rend_row[min_col])
        min_col++;
      uint max_col = columns - 1;
      while (max_col > min_col && cur_row[max_col] == rend_row[max_col])
        max_col--;

      framebuffer_draw_row_span(r, cur_row, min_col, max_col + 1);
      memmove(rend_row + min_col, cur_row + min_col, (max_col - min_col + 1) * sizeof(ushort));
      if (min_row == -1) min_row = (int)r;
      max_row = (int)r;
    }
  }

  if (virtio_gpu_available() && min_row != -1) {
    virtio_gpu_flush_rect(0, (uint)min_row * cell_height, width, (uint)(max_row - min_row + 1) * cell_height);
  }
}

void framebuffer_clear(void) {
  if (!pixels)
    return;

  if (dispi_detected && fb_y_offset != 0) {
    fb_y_offset = 0;
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
  }
  uint total_lines = (dispi_detected && fb_max_y_offset > 0) ? (fb_max_y_offset + height) : height;
  memset((void *)pixels, 0, pitch * total_lines);
  memset(rendered_cells, 0xff, sizeof(rendered_cells));
  if (virtio_gpu_available()) {
    virtio_gpu_flush_all();
  }
}

void framebuffer_scroll_lines(int n, ushort fill_cell) {
  if (!pixels || n <= 0)
    return;

  if (n >= (int)rows) {
    framebuffer_clear();
    return;
  }

  uint scroll_pixels = (uint)n * cell_height;
  uchar attr = fill_cell >> 8;
  int blank_is_zero = (((attr >> 4) & 0x0f) == 0);
  ushort empty_cell = blank_is_zero ? fill_cell : 0xffff;

  if (dispi_detected && fb_max_y_offset >= cell_height) {
    if (fb_y_offset + scroll_pixels > fb_max_y_offset) {
      // Reached the end of VRAM buffer: copy current visible screen back to offset 0
      memmove((void *)pixels, (const void *)(pixels + fb_y_offset * pitch), height * pitch);
      fb_y_offset = 0;
      dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
      // Zero out the remaining headroom
      memset((void *)(pixels + height * pitch), 0, fb_max_y_offset * pitch);
    }

    // Fast hardware panning
    fb_y_offset += scroll_pixels;
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)fb_y_offset);

    // If blank is not zero (colored background), clear the new rows in VRAM
    if (!blank_is_zero) {
      volatile uchar *bottom_ptr = pixels + (fb_y_offset + height - scroll_pixels) * pitch;
      memset((void *)bottom_ptr, 0, scroll_pixels * pitch);
    }

    // Update rendered_cells shadow in RAM
    memmove(rendered_cells, rendered_cells + n * columns,
            (rows - n) * columns * sizeof(ushort));
    for (uint i = (rows - n) * columns; i < rows * columns; i++)
      rendered_cells[i] = empty_cell;
    return;
  }

  // Software scroll: copy scanlines up in memory
  uint move_bytes = (height - scroll_pixels) * pitch;
  volatile uchar *dst = pixels + fb_y_offset * pitch;
  volatile uchar *src = dst + scroll_pixels * pitch;
  memmove((void *)dst, (const void *)src, move_bytes);

  // Clear bottom scanlines
  memset((void *)(dst + move_bytes), 0, scroll_pixels * pitch);

  // Update rendered_cells in RAM
  memmove(rendered_cells, rendered_cells + n * columns,
          (rows - n) * columns * sizeof(ushort));
  for (uint i = (rows - n) * columns; i < rows * columns; i++)
    rendered_cells[i] = empty_cell;

  if (virtio_gpu_available()) {
    virtio_gpu_flush_all();
  }
}

void framebuffer_scroll_up(void) {
  framebuffer_scroll_lines(1, ' ' | 0x0700);
}

int display_set_resolution(uint w, uint h) {
  if (virtio_gpu_available()) {
    return virtio_gpu_set_resolution(w, h);
  }
  if (dispi_detected && pixels) {
    if (w < 320 || w > FB_MAX_WIDTH || h < 200 || h > 2160)
      return -1;
    uint32_t new_pitch = w * 4;
    uint32_t new_size = new_pitch * h;
    if (new_size > framebuffer_size())
      return -1;
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_BPP, 32);
    dispi_write(VBE_DISPI_INDEX_ENABLE, 0x01 | 0x40);
    framebuffer_init_gpu((void *)pixels, w, h, new_pitch);
    console_switch_to_gpu();
    cprintf("VBE: resolution changed to %dx%d\n", w, h);
    return 0;
  }
  return -1;
}

void display_get_resolution(uint *w, uint *h, uint *bpp) {
  if (virtio_gpu_available()) {
    if (w) *w = virtio_gpu_width();
    if (h) *h = virtio_gpu_height();
    if (bpp) *bpp = 32;
  } else if (framebuffer_available()) {
    if (w) *w = framebuffer_width();
    if (h) *h = framebuffer_height();
    if (bpp) *bpp = 32;
  } else {
    if (w) *w = 80;
    if (h) *h = 25;
    if (bpp) *bpp = 4;
  }
}
