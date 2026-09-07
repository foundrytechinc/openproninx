#ifndef PRONINX_FONT_H
#define PRONINX_FONT_H

#include "inc/types.h"

// One console face. data holds 256 glyphs of height rows, stride bytes each,
// most significant bit leftmost, the layout wsfont calls L2R.
struct console_font {
  const char *name;
  const uchar *data;
  uint16_t width;
  uint16_t height;
  uint16_t stride;
  uint32_t min_screen_width; // the narrowest display this face is meant for
};

extern const struct console_font console_fonts[];
extern const uint console_font_count;

#endif
