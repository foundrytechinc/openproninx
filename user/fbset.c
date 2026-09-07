#include "user.h"
#include "inc/bootinfo.h"

static const char *source_name(uint code) {
  static const char *name[] = {"built-in default", "monitor EDID",
                               "firmware mode", "coreboot", "build option"};
  return code < sizeof(name) / sizeof(name[0]) ? name[code] : "unknown";
}

static int parse_number(const char *s) {
  int n = 0;
  while (*s >= '0' && *s <= '9') {
    n = n * 10 + (*s - '0');
    s++;
  }
  return n;
}

static struct fb_modelist list;

static int fetch_modes(int fd) {
  if (ioctl(fd, FBIOGET_MODELIST, (uint64_t)&list) < 0)
    list.count = 0;
  return (int)list.count;
}

static void list_modes(void) {
  uint i;

  if (list.count == 0)
    return;
  printf("modes the adapter offers:\n");
  for (i = 0; i < list.count; i++) {
    printf("  %dx%d@%d", list.mode[i].width, list.mode[i].height,
           list.mode[i].bpp);
    printf("%s", (i % 4) == 3 || i + 1 == list.count ? "\n" : "");
  }
}

static void show(const struct fb_var_screeninfo *vinfo,
                 const struct hwinfo *hw, int have_hw) {
  int cols = vinfo->xres / 8, rows = vinfo->yres / 16;

  if (have_hw) {
    cols = (int)hw->fb_columns;
    rows = (int)hw->fb_rows;
  }
  printf("mode:       %dx%d, %d bpp\n", vinfo->xres, vinfo->yres,
         vinfo->bits_per_pixel);
  printf("console:    %d columns x %d rows\n", cols, rows);
  if (!have_hw)
    return;
  printf("chosen by:  %s\n", source_name(hw->fb_source));
  if (hw->fb_vram)
    printf("video ram:  %d KiB\n", (int)(hw->fb_vram / 1024));
  if (hw->fb_modeset)
    printf("modesetting: yes; the adapter takes any mode it lists\n");
  else
    printf("modesetting: no; this framebuffer was programmed by the firmware "
           "and cannot be retimed without a native display driver\n");
}

int main(int argc, char *argv[]) {
  struct fb_var_screeninfo vinfo;
  struct hwinfo hw;
  int fd = 0, have_hw;
  int w = 0, h = 0;

  if (ioctl(fd, FBIOGET_VSCREENINFO, (uint64_t)&vinfo) < 0) {
    fd = open("/dev/console", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, (uint64_t)&vinfo) < 0) {
      printf("fbset: no framebuffer on this console\n");
      if (fd >= 0) close(fd);
      exit();
    }
  }
  have_hw = hwinfo(&hw) == 0;

  if (argc == 2 && argv[1][0] == '-' && argv[1][1] == 'v') {
    ioctl(fd, FBIODUMPMODES, 0);   /* the raw table goes to the console */
    if (fd > 0) close(fd);
    exit();
  }

  if (argc < 2) {
    fetch_modes(fd);      /* asks the adapter, so hwinfo below knows more */
    have_hw = hwinfo(&hw) == 0;
    list_modes();
    printf("\n");
    show(&vinfo, &hw, have_hw);
    printf("\nusage: fbset <width> <height>   or   fbset <width>x<height>\n");
    printf("       fbset -v   dumps the adapter's raw mode table\n");
    if (fd > 0) close(fd);
    exit();
  }

  if (argc == 2) {
    char *xpos = strchr(argv[1], 'x');
    if (!xpos)
      xpos = strchr(argv[1], 'X');
    if (xpos) {
      *xpos = 0;
      w = parse_number(argv[1]);
      h = parse_number(xpos + 1);
    }
  } else {
    w = parse_number(argv[1]);
    h = parse_number(argv[2]);
  }

  if (w <= 0 || h <= 0) {
    printf("fbset: bad size. example: fbset 1600 900\n");
    if (fd > 0) close(fd);
    exit();
  }

  if (have_hw && !hw.fb_modeset) {
    printf("fbset: this display cannot change mode. the %s programmed "
           "%dx%d and there is no mode-setting adapter behind it.\n",
           source_name(hw.fb_source), (int)hw.fb_width, (int)hw.fb_height);
    printf("       build with VBE_WIDTH=%d VBE_HEIGHT=%d to ask the firmware "
           "for that size at boot instead.\n", w, h);
    if (fd > 0) close(fd);
    exit();
  }
  /* With a mode list the adapter is the authority; only guess when there is
     none, as with a dispi or virtio adapter that takes any size. */
  if (fetch_modes(fd) == 0 && have_hw && hw.fb_vram != 0 &&
      (uint64)w * h * 4 > hw.fb_vram) {
    printf("fbset: %dx%d needs %d KiB, the adapter has %d KiB\n", w, h,
           (int)((uint64)w * h * 4 / 1024), (int)(hw.fb_vram / 1024));
    if (fd > 0) close(fd);
    exit();
  }
  if (w % 8) {
    printf("fbset: width must be a multiple of 8\n");
    if (fd > 0) close(fd);
    exit();
  }

  vinfo.xres = w;
  vinfo.yres = h;
  vinfo.bits_per_pixel = 32;

  if (ioctl(fd, FBIOPUT_VSCREENINFO, (uint64_t)&vinfo) < 0) {
    printf("fbset: the adapter does not offer %dx%d\n", w, h);
    list_modes();
    if (fd > 0) close(fd);
    exit();
  }

  if (hwinfo(&hw) == 0)
    printf("mode set to %dx%d at %d bpp (%d columns x %d rows)\n", w, h,
           (int)hw.fb_bpp, (int)hw.fb_columns, (int)hw.fb_rows);
  else
    printf("mode set to %dx%d\n", w, h);

  if (fd > 0) close(fd);
  exit();
}
