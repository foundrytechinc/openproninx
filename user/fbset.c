#include "user.h"

static int parse_number(const char *s) {
  int n = 0;
  while (*s >= '0' && *s <= '9') {
    n = n * 10 + (*s - '0');
    s++;
  }
  return n;
}

int main(int argc, char *argv[]) {
  struct fb_var_screeninfo vinfo;
  int fd = 0;
  int w = 0, h = 0;

  // Ensure fd is a valid console descriptor
  if (ioctl(fd, FBIOGET_VSCREENINFO, (uint64_t)&vinfo) < 0) {
    fd = open("/dev/console", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, (uint64_t)&vinfo) < 0) {
      printf("fbset: display device / framebuffer not available\n");
      if (fd >= 0) close(fd);
      exit();
    }
  }

  if (argc < 2) {
    printf("Current resolution: %dx%d (%d columns x %d rows, %d bpp)\n\n",
           vinfo.xres, vinfo.yres, vinfo.xres / 8, vinfo.yres / 16,
           vinfo.bits_per_pixel);
    printf("Usage: fbset <width> <height>   OR   fbset <width>x<height>\n\n");
    printf("Popular standard resolutions:\n");
    printf("  640 480    (640x480,   80x30 text grid)\n");
    printf("  800 600    (800x600,  100x37 text grid)\n");
    printf("  1024 768   (1024x768, 128x48 text grid) [Default]\n");
    printf("  1280 720   (1280x720, 160x45 text grid, 720p HD)\n");
    printf("  1280 800   (1280x800, 160x50 text grid)\n");
    printf("  1280 1024  (1280x1024,160x64 text grid)\n");
    printf("  1600 900   (1600x900, 200x56 text grid)\n");
    printf("  1920 1080  (1920x1080,240x67 text grid, 1080p FHD)\n");
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
  } else if (argc >= 3) {
    w = parse_number(argv[1]);
    h = parse_number(argv[2]);
  }

  if (w <= 0 || h <= 0) {
    printf("fbset: invalid resolution. Example: fbset 1024 768 or fbset 1280x720\n");
    if (fd > 0) close(fd);
    exit();
  }

  vinfo.xres = w;
  vinfo.yres = h;
  vinfo.bits_per_pixel = 32;

  if (ioctl(fd, FBIOPUT_VSCREENINFO, (uint64_t)&vinfo) < 0) {
    printf("fbset: failed to set resolution %dx%d (unsupported mode or memory limit)\n", w, h);
    if (fd > 0) close(fd);
    exit();
  }

  printf("Display resolution changed to %dx%d (%d columns x %d rows)\n",
         w, h, w / 8, h / 16);

  if (fd > 0) close(fd);
  exit();
}
