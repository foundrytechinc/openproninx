// VIDCONTROL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <fnu/sys.h>
#include <err.h>

static void usage(void) {
  dprintf(2, "usage: vidcontrol [-i active | adapter | mode] [-m on | off] [mode]\n");
  exit(1);
}

static void set_mouse(const char *arg) {
  struct fnu_mouse_info mouse;

  memset(&mouse, 0, sizeof(mouse));
  if (strcmp(arg, "on") == 0)
    mouse.operation = FNU_MOUSE_SHOW;
  else if (strcmp(arg, "off") == 0)
    mouse.operation = FNU_MOUSE_HIDE;
  else {
    dprintf(2, "vidcontrol: argument to -m must be either on or off\n");
    exit(1);
  }
  if (ioctl(0, FNU_CONSMOUSE, (uint64_t)(uintptr_t)&mouse) < 0) {
    dprintf(2, "vidcontrol: %sing the mouse: not a console\n",
            mouse.operation == FNU_MOUSE_SHOW ? "show" : "hid");
    exit(1);
  }
}

static const char *adapter_name(int type) {
  switch (type) {
  case FNU_FBTYPE_VGA: return "VGA";
  case FNU_FBTYPE_EFIFB: return "EFI framebuffer";
  case FNU_FBTYPE_PCIMISC: return "PCI display";
  }
  return "unknown";
}

static void show_adapter_info(void) {
  struct fnu_fbtype ft;
  int mode;

  if (ioctl(0, FNU_FBTYPE, (uint64_t)(uintptr_t)&ft) < 0) {
    dprintf(2, "vidcontrol: obtaining adapter information: not a console\n");
    exit(1);
  }
  printf("fb0:\n");
  printf("    type:%s (%d)\n", adapter_name(ft.fb_type), ft.fb_type);
  if (ioctl(0, FNU_FBGETMODE, (uint64_t)(uintptr_t)&mode) == 0)
    printf("    current mode:%d\n", mode);
  printf("    frame buffer size:0x%x\n", ft.fb_size);
  printf("    %dx%d, depth:%d, colormap:%d\n", ft.fb_width, ft.fb_height,
         ft.fb_depth, ft.fb_cmsize);
}

static void print_mode(const struct fnu_video_info *info) {
  char buf[80];
  int c, mm;

  printf("%3d (0x%03x)", info->vi_mode, info->vi_mode);
  printf(" 0x%08x", info->vi_flags);
  if (info->vi_flags & FNU_V_INFO_GRAPHICS) {
    c = 'G';
    if (info->vi_mem_model == FNU_V_INFO_MM_PLANAR)
      snprintf(buf, sizeof(buf), "%dx%dx%d %d", info->vi_width,
               info->vi_height, info->vi_depth, info->vi_planes);
    else {
      switch (info->vi_mem_model) {
      case FNU_V_INFO_MM_PACKED: mm = 'P'; break;
      case FNU_V_INFO_MM_DIRECT: mm = 'D'; break;
      default: mm = ' '; break;
      }
      snprintf(buf, sizeof(buf), "%dx%dx%d %c", info->vi_width,
               info->vi_height, info->vi_depth, mm);
    }
  } else {
    c = 'T';
    snprintf(buf, sizeof(buf), "%dx%d", info->vi_width, info->vi_height);
  }
  printf(" %c %-15s", c, buf);
  snprintf(buf, sizeof(buf), "%dx%d", info->vi_cwidth, info->vi_cheight);
  printf(" %-5s", buf);
  printf(" 0x%05lx %2dk %2dk", (unsigned long)info->vi_window,
         (int)info->vi_window_size / 1024, (int)info->vi_window_gran / 1024);
  printf(" 0x%08lx %dk\n", (unsigned long)info->vi_buffer,
         (int)info->vi_buffer_size / 1024);
}

static void show_mode_info(void) {
  struct fnu_video_info info;
  int mode;

  printf("    mode#     flags   type    size       "
         "font      window      linear buffer\n");
  printf("---------------------------------------"
         "---------------------------------------\n");
  memset(&info, 0, sizeof(info));
  for (mode = 0; mode <= FNU_M_VESA_MODE_MAX; ++mode) {
    info.vi_mode = mode;
    if (ioctl(0, FNU_FBMODEINFO, (uint64_t)(uintptr_t)&info) < 0)
      continue;
    if (info.vi_mode != mode)
      continue;
    if (info.vi_width == 0 && info.vi_height == 0 && info.vi_cwidth == 0 &&
        info.vi_cheight == 0)
      continue;
    print_mode(&info);
  }
}

static void show_active_info(void) {
  struct fnu_video_info info;
  struct winsize ws;
  int mode;

  if (ioctl(0, FNU_FBGETMODE, (uint64_t)(uintptr_t)&mode) < 0) {
    dprintf(2, "vidcontrol: obtaining the current mode: not a console\n");
    exit(1);
  }
  memset(&info, 0, sizeof(info));
  info.vi_mode = mode;
  if (ioctl(0, FNU_FBMODEINFO, (uint64_t)(uintptr_t)&info) < 0) {
    dprintf(2, "vidcontrol: obtaining the current mode parameters\n");
    exit(1);
  }
  printf("mode:                       %d\n", mode);
  printf("screen size:                %dx%d, %d bits\n", info.vi_width,
         info.vi_height, info.vi_depth);
  if (ioctl(0, TIOCGWINSZ, (uint64_t)(uintptr_t)&ws) == 0)
    printf("console:                    %d columns x %d rows\n", ws.ws_col,
           ws.ws_row);
  printf("font:                       %dx%d\n", info.vi_cwidth,
         info.vi_cheight);
}

static void show_info(const char *arg) {
  if (strcmp(arg, "active") == 0)
    show_active_info();
  else if (strcmp(arg, "adapter") == 0)
    show_adapter_info();
  else if (strcmp(arg, "mode") == 0)
    show_mode_info();
  else {
    dprintf(2, "vidcontrol: argument to -i must be active, adapter, or mode\n");
    exit(1);
  }
}

static void video_mode(const char *arg) {
  struct fnu_video_info info;
  int mode;

  memset(&info, 0, sizeof(info));
  if (strncmp(arg, "MODE_", 5) == 0) {
    if (!isdigit(arg[5])) {
      dprintf(2, "vidcontrol: invalid video mode number\n");
      exit(1);
    }
    mode = atoi(arg + 5);
  } else {
    const char *x = strchr(arg, 'x');

    if (x == 0 || !isdigit(arg[0]) || !isdigit(x[1]))
      usage();
    info.vi_width = atoi(arg);
    info.vi_height = atoi(x + 1);
    if (ioctl(0, FNU_FBFINDMODE, (uint64_t)(uintptr_t)&info) < 0) {
      dprintf(2, "vidcontrol: the adapter does not offer %s\n", arg);
      exit(1);
    }
    mode = info.vi_mode;
  }
  if (ioctl(0, FNU_FBSETMODE, (uint64_t)(uintptr_t)&mode) < 0) {
    warn("mode %d", mode);
    exit(1);
  }
}

int main(int argc, char **argv) {
  int i, did = 0;

  for (i = 1; i < argc; i++) {
    const char *p = argv[i];

    if (p[0] == '-' && p[1] != 0 && p[2] == 0 && (p[1] == 'i' || p[1] == 'm')) {
      if (i + 1 >= argc)
        usage();
      if (p[1] == 'i')
        show_info(argv[++i]);
      else
        set_mouse(argv[++i]);
    } else if (strncmp(p, "-i", 2) == 0 && p[2] != 0)
      show_info(p + 2);
    else if (strncmp(p, "-m", 2) == 0 && p[2] != 0)
      set_mouse(p + 2);
    else if (p[0] == '-')
      usage();
    else
      video_mode(p);
    did = 1;
  }
  if (!did)
    usage();
  exit(0);
}
