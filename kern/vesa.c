#include "defs.h"
#include "memlayout.h"
#include "realmode.h"
#include "vesa.h"

// layout and call sequence from FreeBSD sys/dev/fb/vesa.c: 4f00 with a
// "VBE2" signature, nothing below VBE 1.2, then 4f01 per mode.
struct vbe_info {
  char sig[4];
  uint16_t version;
  uint32_t oemstr;
  uint32_t caps;
  uint32_t modetable;
  uint16_t memsize; /* in 64K blocks */
  uint8_t rest[492];
} __attribute__((packed));

struct vbe_mode {
  uint16_t attr;
  uint8_t win_a, win_b;
  uint16_t granularity, winsize, seg_a, seg_b;
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
} __attribute__((packed));

_Static_assert(sizeof(struct vbe_info) == 512, "vbe_info");
_Static_assert(sizeof(struct vbe_mode) == 256, "vbe_mode");

#define VBE_OK 0x004f
#define INFO_OFF 0
#define MODE_OFF 512

static int vesa_ready;
static uint16_t vesa_version;
static uint64_t vesa_memory;
static uint32_t vesa_modetable;

static void *buf_at(uint off) { return (uchar *)realmode_buffer() + off; }

static uint16_t buf_seg(uint off) { return (uint16_t)((RM_BUF + off) >> 4); }

static uint16_t buf_off(uint off) { return (uint16_t)((RM_BUF + off) & 0xf); }

static int vbe_mode_info(uint16_t mode, struct vbe_mode *out) {
  struct realregs r;

  memset(&r, 0, sizeof(r));
  memset(buf_at(MODE_OFF), 0, sizeof(*out));
  r.ax = 0x4f01;
  r.cx = mode;
  r.es = buf_seg(MODE_OFF);
  r.di = buf_off(MODE_OFF);
  if (realmode_int(0x10, &r) < 0 || r.ax != VBE_OK)
    return -1;
  memmove(out, buf_at(MODE_OFF), sizeof(*out));
  return 0;
}

// probing calls the video bios, so it waits for the first mode change
static int vesa_probed;

static void vesa_probe(void) {
  struct vbe_info *info = buf_at(INFO_OFF);
  struct realregs r;

  if (vesa_probed)
    return;
  vesa_probed = 1;
  if (!realmode_available())
    return;

  memset(info, 0, sizeof(*info));
  memmove(info->sig, "VBE2", 4);
  memset(&r, 0, sizeof(r));
  r.ax = 0x4f00;
  r.es = buf_seg(INFO_OFF);
  r.di = buf_off(INFO_OFF);
  if (realmode_int(0x10, &r) < 0 || r.ax != VBE_OK)
    return;
  if (info->sig[0] != 'V' || info->sig[1] != 'E' || info->sig[2] != 'S' ||
      info->sig[3] != 'A')
    return;
  if (info->version < 0x0102) {
    cprintf("VESA: VBE %d.%d is too old\n", info->version >> 8,
            info->version & 0xff);
    return;
  }
  vesa_version = info->version;
  vesa_memory = (uint64_t)info->memsize * 64 * 1024;
  vesa_modetable = info->modetable;
  vesa_ready = vesa_modetable != 0;
  cprintf("VESA: VBE %d.%d, %d KB video memory, mode setting through the "
          "video bios\n",
          vesa_version >> 8, vesa_version & 0xff, (uint)(vesa_memory / 1024));
}

int vesa_available(void) {
  vesa_probe();
  return vesa_ready;
}

uint64_t vesa_video_memory(void) {
  vesa_probe();
  return vesa_memory;
}

// what we already know, without going near the bios
uint64_t vesa_video_memory_known(void) { return vesa_memory; }

// A real mode far pointer into the option rom.
static const uint16_t *mode_list(void) {
  uintptr_t linear = ((uintptr_t)(vesa_modetable >> 16) << 4) +
                     (vesa_modetable & 0xffff);
  return (const uint16_t *)P2V(linear);
}

static int usable(const struct vbe_mode *m) {
  if ((m->attr & 0x01) == 0 || (m->attr & 0x10) == 0 || (m->attr & 0x80) == 0)
    return 0;
  if (m->planes != 1 || m->model != 6)
    return 0;
  // widescreen sizes are often offered at 16 bits only; the console can draw
  // into those now, so do not throw them away
  return m->bpp == 15 || m->bpp == 16 || m->bpp == 24 || m->bpp == 32;
}

// everything the rom lists, filter off: never offered, or thrown away
void vesa_dump_modes(void) {
  struct vbe_mode m;
  const uint16_t *list;
  int i;

  vesa_probe();
  if (!vesa_ready) {
    cprintf("VESA: no mode table\n");
    return;
  }
  cprintf("VESA: VBE %d.%d, %d KB, mode table at %x:%x\n", vesa_version >> 8,
          vesa_version & 0xff, (uint)(vesa_memory / 1024), vesa_modetable >> 16,
          vesa_modetable & 0xffff);
  list = mode_list();
  for (i = 0; i < 512 && list[i] != 0xffff; i++) {
    if (vbe_mode_info(list[i], &m) < 0) {
      cprintf("  %x  no info\n", list[i]);
      continue;
    }
    cprintf("  %x attr %x %dx%dx%d model %d pitch %d lfb %x %s\n", list[i],
            m.attr, m.width, m.height, m.bpp, m.model,
            (vesa_version >= 0x0300 && m.lin_pitch) ? m.lin_pitch : m.pitch,
            m.phys_base, usable(&m) ? "usable" : "dropped");
  }
  cprintf("VESA: %d entries\n", i);
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

// bit 14 asks for the linear framebuffer, bit 15 keeps the contents.
// straight out of FreeBSD vesa_set_mode().
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
  if (realmode_int(0x10, &r) < 0 || r.ax != VBE_OK)
    return -1;

  // the adapter answers with the pitch and aperture it actually programmed
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
