#ifndef PRONINX_FONT_H
#define PRONINX_FONT_H

#include "inc/types.h"

typedef uint32_t vtcell_t;
#define ATTR(a) ((vtcell_t)(a) << 24)
#define CELL(cp, attr) (((vtcell_t)(cp) & 0xffffff) | (attr))
#define CELL_CP(c) ((c) & 0xffffff)
#define CELL_ATTR(c) ((uchar)((c) >> 24))

// a run of code points on consecutive glyphs
struct font_map {
  uint32_t src;
  uint16_t dst;
  uint16_t len;
};

// glyphs msb leftmost, in code point order
struct console_font {
  const char *name;
  const uchar *data;
  const struct font_map *map;
  uint nmap;
  uint nglyphs;
  uint16_t width;
  uint16_t height;
  uint16_t stride;
  uint32_t min_screen_width;
};

extern const struct console_font console_fonts[];
extern const uint console_font_count;

uint font_glyph(const struct console_font *f, uint32_t cp);
uchar cp437_of(uint32_t cp);

#endif
