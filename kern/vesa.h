#ifndef PRONINX_X86_64_VESA_H
#define PRONINX_X86_64_VESA_H

#include "inc/types.h"

struct vesa_modeinfo {
  uint16_t mode;
  uint16_t width;
  uint16_t height;
  uint16_t bpp;
  uint32_t pitch;
  uint32_t phys_base;
  uint8_t red_pos, red_size;
  uint8_t green_pos, green_size;
  uint8_t blue_pos, blue_size;
};

int vesa_available(void);
struct realregs;
int video_bios_int(struct realregs *r);
int vesa_mode_lookup(uint16_t mode, uint *w, uint *h, uint *bpp);
uint64_t vesa_video_memory(void);
uint64_t vesa_video_memory_known(void);
int vesa_set_mode(uint w, uint h, struct vesa_modeinfo *out);
int vesa_each_mode(int (*fn)(uint16_t, uint, uint, uint, void *), void *arg);
int vesa_state_save(void);
int vesa_state_restore(void);

#endif /* ifndef PRONINX_X86_64_VESA_H */
