#include "defs.h"
#include "malloc.h"
#include "memlayout.h"
#include "realmode.h"
#include "vesa.h"

struct vbe_info {
  char sig[4];
  uint16_t version;
  uint32_t oemstr;
  uint32_t caps;
  uint32_t modetable;
  uint16_t memsize; /* in 64K blocks */
  uint8_t rest[492];
} __packed;

struct vbe_mode {
  uint16_t attr;
  uint8_t win_a, win_b;
  uint16_t granularity, win_size, seg_a, seg_b;
  uint32_t winfunc;
  uint16_t pitch;
  uint16_t width, height;
  uint8_t xchar, ychar, planes, bpp;
  uint8_t banks, model, banksize, pages;
  uint8_t rsvd1;
  uint8_t red_size, red_pos;
  uint8_t green_size, green_pos;
  uint8_t blue_size, blue_pos;
  uint8_t alpha_size, alpha_pos;
  uint8_t direct_color;
  uint32_t phys_base;
  uint32_t offscreen;
  uint16_t offscreen_size;
  uint16_t lin_pitch;
  uint8_t bnk_pages, lin_pages;
  uint8_t lin_red_size, lin_red_pos;
  uint8_t lin_green_size, lin_green_pos;
  uint8_t lin_blue_size, lin_blue_pos;
  uint8_t lin_alpha_size, lin_alpha_pos;
  uint32_t max_clock;
  uint8_t rsvd2[190];
} __packed;

_Static_assert(sizeof(struct vbe_info) == 512, "vbe_info");
_Static_assert(sizeof(struct vbe_mode) == 256, "vbe_mode");

#define VBE_OK 0x004f
#define INFO_OFF 0
#define MODE_OFF 512
#define STATE_OFF 1024
#define STATE_MAX (RM_STACK - RM_BUF - STATE_OFF - 1024) // the rest is stack

static int vesa_ready;
static uint16_t vesa_version;
static uint64_t vesa_memory;
static uint32_t vesa_modetable;
static int state_mask = -1; // VBE 4F04h's parts the bios keeps
static size_t state_size;
static uint8_t *state_saved;
static int state_valid;

// the video bios in x86emu where it can be, else in real mode
int video_bios_int(struct realregs *r) {
  return x86bios_ready() ? x86bios_int(0x10, r) : realmode_int(0x10, r);
}

static void *buf_at(uint off) {
  return (uchar *)(x86bios_ready() ? x86bios_buffer() : realmode_buffer()) + off;
}

static uint32_t buf_lin(uint off) {
  return (x86bios_ready() ? X86BIOS_BUF : realmode_buffer_lin()) + off;
}

static uint16_t buf_seg(uint off) { return (uint16_t)(buf_lin(off) >> 4); }

static uint16_t buf_off(uint off) { return (uint16_t)(buf_lin(off) & 0xf); }

static int vbe_mode_info(uint16_t mode, struct vbe_mode *out) {
  struct realregs r;

  memset(&r, 0, sizeof(r));
  memset(buf_at(MODE_OFF), 0, sizeof(*out));
  r.ax = 0x4f01;
  r.cx = mode;
  r.es = buf_seg(MODE_OFF);
  r.di = buf_off(MODE_OFF);
  if (video_bios_int(&r) < 0 || r.ax != VBE_OK)
    return -1;
  memmove(out, buf_at(MODE_OFF), sizeof(*out));
  return 0;
}

static int vesa_probed, said;

// again when the bios moves into x86emu: the mode list lives there
static void vesa_probe(void) {
  struct vbe_info *info = buf_at(INFO_OFF);
  struct realregs r;

  if (vesa_probed == 1 + x86bios_ready())
    return;
  vesa_probed = 1 + x86bios_ready();
  vesa_ready = 0;
  if (!realmode_available() && !x86bios_ready())
    return;

  memset(info, 0, sizeof(*info));
  memmove(info->sig, "VBE2", 4);
  memset(&r, 0, sizeof(r));
  r.ax = 0x4f00;
  r.es = buf_seg(INFO_OFF);
  r.di = buf_off(INFO_OFF);
  if (video_bios_int(&r) < 0 || r.ax != VBE_OK)
    return;
  if (info->sig[0] != 'V' || info->sig[1] != 'E' || info->sig[2] != 'S' ||
      info->sig[3] != 'A')
    return;
  if (info->version < 0x0102) {
    printf("VESA: VBE %d.%d is too old\n", info->version >> 8,
            info->version & 0xff);
    return;
  }
  vesa_version = info->version;
  vesa_memory = (uint64_t)info->memsize * 64 * 1024;
  vesa_modetable = info->modetable;
  vesa_ready = vesa_modetable != 0;
  if (!said) {
    said = 1;
    printf("VESA: VBE %d.%d, %d KB video memory, the video bios %s\n", vesa_version >> 8,
           vesa_version & 0xff, (uint)(vesa_memory / 1024),
           x86bios_ready() ? "in x86emu" : "in real mode");
  }
}

int vesa_available(void) {
  vesa_probe();
  return vesa_ready;
}

uint64_t vesa_video_memory(void) {
  vesa_probe();
  return vesa_memory;
}

uint64_t vesa_video_memory_known(void) { return vesa_memory; }

static const uint16_t *mode_list(void) {
  uintptr_t linear = ((uintptr_t)(vesa_modetable >> 16) << 4) +
                     (vesa_modetable & 0xffff);
  return (const uint16_t *)(x86bios_ready() ? x86bios_ptr((uint32_t)linear) : P2V(linear));
}

static int usable(const struct vbe_mode *m) {
  if ((m->attr & 0x01) == 0 || (m->attr & 0x10) == 0 || (m->attr & 0x80) == 0)
    return 0;
  if (m->planes != 1 || m->model != 6)
    return 0;
  return m->bpp == 15 || m->bpp == 16 || m->bpp == 24 || m->bpp == 32;
}

int vesa_mode_lookup(uint16_t mode, uint *w, uint *h, uint *bpp) {
  struct vbe_mode m;

  vesa_probe();
  if (!vesa_ready || vbe_mode_info(mode, &m) < 0 || !usable(&m))
    return -1;
  *w = m.width;
  *h = m.height;
  *bpp = m.bpp;
  return 0;
}

int vesa_each_mode(int (*fn)(uint16_t, uint, uint, uint, void *), void *arg) {
  struct vbe_mode m;
  const uint16_t *list;
  int i, n = 0;

  vesa_probe();
  if (!vesa_ready)
    return -1;
  list = mode_list();
  for (i = 0; i < 512 && list[i] != 0xffff; i++) {
    if (vbe_mode_info(list[i], &m) < 0 || !usable(&m))
      continue;
    n++;
    if (fn && fn(list[i], m.width, m.height, m.bpp, arg) != 0)
      break;
  }
  return n;
}

int vesa_set_mode(uint w, uint h, struct vesa_modeinfo *out) {
  struct vbe_mode m, best;
  struct realregs r;
  const uint16_t *list;
  int i, found = 0;
  uint16_t mode = 0;

  vesa_probe();
  if (!vesa_ready)
    return -1;
  list = mode_list();
  for (i = 0; i < 512 && list[i] != 0xffff; i++) {
    if (vbe_mode_info(list[i], &m) < 0 || !usable(&m))
      continue;
    if (m.width != w || m.height != h)
      continue;
    if (found && m.bpp <= best.bpp)
      continue;
    best = m;
    mode = list[i];
    found = 1;
  }
  if (!found)
    return -1;

  memset(&r, 0, sizeof(r));
  r.ax = 0x4f02;
  r.bx = (uint16_t)(mode | 0x4000 | 0x8000);
  if (video_bios_int(&r) < 0 || r.ax != VBE_OK)
    return -1;

  if (vbe_mode_info(mode, &best) < 0)
    return -1;
  out->mode = mode;
  out->width = best.width;
  out->height = best.height;
  out->bpp = best.bpp;
  {
    int v3 = vesa_version >= 0x0300 && best.lin_pitch != 0;
    out->pitch = v3 ? best.lin_pitch : best.pitch;
    out->red_pos = v3 ? best.lin_red_pos : best.red_pos;
    out->red_size = v3 ? best.lin_red_size : best.red_size;
    out->green_pos = v3 ? best.lin_green_pos : best.green_pos;
    out->green_size = v3 ? best.lin_green_size : best.green_size;
    out->blue_pos = v3 ? best.lin_blue_pos : best.blue_pos;
    out->blue_size = v3 ? best.lin_blue_size : best.blue_size;
  }
  out->phys_base = best.phys_base;
  return 0;
}

// VBE 4F04h: bytes for the mask, 0 if none
static size_t state_buf_size(int mask) {
  struct realregs r;

  memset(&r, 0, sizeof(r));
  r.ax = 0x4f04;
  r.cx = (uint16_t)mask;
  if (video_bios_int(&r) < 0 || r.ax != VBE_OK)
    return 0;
  return (size_t)r.bx * 64;
}

static void state_probe(void) {
  int i;

  if (state_mask >= 0)
    return;
  state_mask = 0;
  vesa_probe();
  if (!vesa_ready)
    return;
  for (i = 0; i < 4; i++)
    if (state_buf_size(1 << i) > 0)
      state_mask |= 1 << i;
  if (state_mask != 0)
    state_size = state_buf_size(state_mask);
  if (state_size == 0 || state_size > STATE_MAX) {
    state_size = 0;
    return;
  }
  state_saved = malloc(state_size, M_DEVBUF, M_WAITOK);
}

// adapter state kept by its bios, given back after sleep
int vesa_state_save(void) {
  struct realregs r;

  state_valid = 0;
  state_probe();
  if (state_size == 0)
    return -1;
  memset(&r, 0, sizeof(r));
  r.ax = 0x4f04;
  r.dx = 1;
  r.cx = (uint16_t)state_mask;
  r.es = buf_seg(STATE_OFF);
  r.bx = buf_off(STATE_OFF);
  if (video_bios_int(&r) < 0 || r.ax != VBE_OK)
    return -1;
  memmove(state_saved, buf_at(STATE_OFF), state_size);
  state_valid = 1;
  return 0;
}

int vesa_state_restore(void) {
  struct realregs r;

  if (!state_valid)
    return -1;
  memmove(buf_at(STATE_OFF), state_saved, state_size);
  memset(&r, 0, sizeof(r));
  r.ax = 0x4f04;
  r.dx = 2;
  r.cx = (uint16_t)state_mask;
  r.es = buf_seg(STATE_OFF);
  r.bx = buf_off(STATE_OFF);
  if (video_bios_int(&r) < 0 || r.ax != VBE_OK)
    return -1;
  return 0;
}
