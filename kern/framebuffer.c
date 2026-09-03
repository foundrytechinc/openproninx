#include "defs.h"
#include "inc/framebuffer.h"
#include "memlayout.h"

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
static int initialization_error;

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
  if (!framebuffer_phys())
    return 0;
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

  /*
   * VBE linear framebuffers are normally above 2 GiB (QEMU stdvga uses
   * 0xfd000000).  P2V is only valid below that boundary; use the device
   * window so every possible 32-bit physical framebuffer has a canonical
   * kernel virtual address.
   */
  return physical_base ? DEVSPACE_P2V(physical_base) : 0;
}

static void putpixel(uint x, uint y, uint32_t color) {
  volatile uchar *pixel;

  if (x >= width || y >= height)
    return;
  pixel = pixels + y * pitch + x * bytes_per_pixel;
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
  initialization_error = 0;

  for (uint y = 0; y < height; y++)
    for (uint x = 0; x < width; x++)
      putpixel(x, y, 0);
}

void framebuffer_draw_cell(int cell_index, ushort cell) {
  uint row, col, x0, x1, y0, y1, glyph_x, glyph_y;
  uchar ch = cell & 0xff;
  uchar attr = cell >> 8;
  uint32_t foreground = vga_palette[attr & 0x0f];
  uint32_t background = vga_palette[(attr >> 4) & 0x0f];

  if (!pixels || cell_index < 0 || cell_index >= (int)(columns * rows))
    return;
  row = cell_index / columns;
  col = cell_index % columns;
  /*
   * Spread cell bounds across the entire framebuffer.  The glyph itself is
   * still drawn with one integer scale in both axes, so it never stretches;
   * any remainder pixels become part of the cell background.
   */
  x0 = col * width / columns;
  x1 = (col + 1) * width / columns;
  y0 = row * height / rows;
  y1 = (row + 1) * height / rows;
  glyph_x = x0 + (x1 - x0 - FONT_WIDTH * scale) / 2;
  glyph_y = y0 + (y1 - y0 - FONT_HEIGHT * scale) / 2;

  for (uint y = y0; y < y1; y++)
    for (uint x = x0; x < x1; x++)
      putpixel(x, y, background);

  for (uint gy = 0; gy < FONT_HEIGHT; gy++) {
    uchar bits = font[(uint)ch * FONT_HEIGHT + gy];
    for (uint gx = 0; gx < FONT_WIDTH; gx++) {
      if (bits & (0x80 >> gx))
        for (uint dy = 0; dy < scale; dy++)
          for (uint dx = 0; dx < scale; dx++)
            putpixel(glyph_x + gx * scale + dx, glyph_y + gy * scale + dy,
                     foreground);
    }
  }
}

void framebuffer_redraw_cells(const ushort *cells, int count) {
  if (!pixels)
    return;
  for (int i = 0; i < count && i < (int)(columns * rows); i++)
    framebuffer_draw_cell(i, cells[i]);
}
