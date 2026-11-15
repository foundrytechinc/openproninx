// psmtest: /dev/psm0 and /dev/bpsm0 at their three levels; -m waits for motion
#include <sys/ioctl.h>
#include <sys/mouse.h>
#include <fnu/sys.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void ok(const char *what, int good) {
  printf("psmtest: %-44s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static void motion(void) {
  struct pollfd pf;
  mousemode_t mode;
  unsigned char buf[64];
  int fd, level, n;

  if ((fd = open("/dev/bpsm0", O_RDONLY)) < 0)
    err(1, "/dev/bpsm0");
  for (level = 0; level <= 2; level++) {
    ok("SETLEVEL", ioctl(fd, MOUSE_SETLEVEL, &level) == 0);
    ioctl(fd, MOUSE_GETMODE, &mode);
    printf("psmtest: level %d: move the mouse\n", level);
    fflush(stdout);
    pf.fd = fd;
    pf.events = POLLIN;
    n = poll(&pf, 1, 20000);
    ok("poll sees the motion", n == 1 && (pf.revents & POLLIN));
    if (n != 1)
      continue;
    n = (int)read(fd, buf, sizeof(buf));
    ok("a whole packet, or several", n > 0 && (level == 2 || n % mode.packetsize == 0));
    if (level == 0)
      ok("level 0 has the PS/2 sync bit", (buf[0] & MOUSE_PS2_SYNC) != 0);
    if (level == 1)
      ok("level 1 is a sysmouse packet",
         (buf[0] & MOUSE_SYS_SYNCMASK) == MOUSE_SYS_SYNC);
    usleep(300000);
    for (n = 0; n < 5 && poll(&pf, 1, 100) == 1 && read(fd, buf, sizeof(buf)) > 0; n++)
      ;
  }
  close(fd);
}

int main(int argc, char **argv) {
  mousehw_t hw;
  mousemode_t mode;
  mousestatus_t st;
  mousedata_t data;
  struct pollfd pf;
  unsigned char buf[16];
  int fd, fd2, level, id;

  setvbuf(stdout, 0, _IONBF, 0);
  if ((fd = open("/dev/psm0", O_RDWR)) < 0) {
    if (errno == ENOENT) {
      printf("psmtest: no PS/2 mouse here, skipped\n");
      return 0;
    }
    err(1, "/dev/psm0");
  }
  ok("a second open is EBUSY", (fd2 = open("/dev/bpsm0", O_RDONLY)) < 0 && errno == EBUSY);
  ok("GETHWINFO: a PS/2 device", ioctl(fd, MOUSE_GETHWINFO, &hw) == 0 &&
                                     hw.iftype == MOUSE_IF_PS2);
  ok("level 0 reports the generic model", hw.model == MOUSE_MODEL_GENERIC);
  ok("GETMODE at level 0: PS/2, 3 bytes",
     ioctl(fd, MOUSE_GETMODE, &mode) == 0 && mode.level == 0 &&
         mode.protocol == MOUSE_PROTO_PS2 && mode.packetsize == 3);
  level = 1;
  ok("SETLEVEL 1", ioctl(fd, MOUSE_SETLEVEL, &level) == 0);
  ok("GETMODE at level 1: sysmouse, 8 bytes",
     ioctl(fd, MOUSE_GETMODE, &mode) == 0 && mode.protocol == MOUSE_PROTO_SYSMOUSE &&
         mode.packetsize == 8 && mode.syncmask[0] == MOUSE_SYS_SYNCMASK);
  level = 3;
  ok("SETLEVEL 3 is EINVAL", ioctl(fd, MOUSE_SETLEVEL, &level) < 0 && errno == EINVAL);
  ok("GETLEVEL", ioctl(fd, MOUSE_GETLEVEL, &level) == 0 && level == 1);
  ok("GETSTATUS", ioctl(fd, MOUSE_GETSTATUS, &st) == 0);
  id = -1;
  ok("GETHWID reads the device's id", ioctl(fd, MOUSE_GETHWID, &id) == 0 && id >= 0 &&
                                          ioctl(fd, MOUSE_GETHWINFO, &hw) == 0 &&
                                          (hw.hwid & 0xff) == id);
  memset(&mode, 0, sizeof(mode));
  mode.rate = 100;
  mode.resolution = MOUSE_RES_DEFAULT;
  mode.accelfactor = -1;
  mode.level = -1;
  ok("SETMODE rate 100", ioctl(fd, MOUSE_SETMODE, &mode) == 0);
  ok("the rate stuck", ioctl(fd, MOUSE_GETMODE, &mode) == 0 && mode.rate == 100);
  data.len = 3;
  ok("READSTATE: three status bytes", ioctl(fd, MOUSE_READSTATE, &data) == 0 && data.len == 3);
  ok("write below level 2 is ENODEV", write(fd, buf, 1) < 0 && errno == ENODEV);
  ok("an empty psm0 read is EAGAIN", read(fd, buf, sizeof(buf)) < 0 && errno == EAGAIN);
  pf.fd = fd;
  pf.events = POLLIN;
  ok("poll of an empty queue times out", poll(&pf, 1, 100) == 0);
  close(fd);
  ok("after close it opens again", (fd = open("/dev/psm0", O_RDONLY)) >= 0);
  ok("and starts at level 0", ioctl(fd, MOUSE_GETLEVEL, &level) == 0 && level == 0);
  close(fd);
  if ((fd = open("/dev/wsmouse0", O_RDONLY)) >= 0) {
    unsigned int type = 0;
    int native = FNU_WSMOUSE_NATIVE, compat = FNU_WSMOUSE_COMPAT, pad;
    struct fnu_wsmouse_calib calib;

    ok("wsmouse0 says what it is",
       ioctl(fd, FNU_WSMOUSEGTYPE, &type) == 0 &&
           (type == FNU_WSMOUSE_TYPE_PS2 || type == FNU_WSMOUSE_TYPE_USB ||
            type == FNU_WSMOUSE_TYPE_SYNAPTICS ||
            type == FNU_WSMOUSE_TYPE_ELANTECH ||
            type == FNU_WSMOUSE_TYPE_TOUCHPAD));
    pad = type == FNU_WSMOUSE_TYPE_SYNAPTICS ||
          type == FNU_WSMOUSE_TYPE_ELANTECH || type == FNU_WSMOUSE_TYPE_TOUCHPAD;
    if (pad) {
      ok("a touchpad has a NATIVE mode",
         ioctl(fd, FNU_WSMOUSESETMODE, &native) == 0 &&
             ioctl(fd, FNU_WSMOUSESETMODE, &compat) == 0);
      ok("and a touch range", ioctl(fd, FNU_WSMOUSEGCALIB, &calib) == 0 &&
                                  calib.maxx > calib.minx &&
                                  calib.maxy > calib.miny);
      printf("psmtest: touchpad type %u, x %d..%d, y %d..%d\n", type,
             (int)calib.minx, (int)calib.maxx, (int)calib.miny, (int)calib.maxy);
    } else {
      ok("a mouse has no NATIVE mode",
         ioctl(fd, FNU_WSMOUSESETMODE, &native) < 0 && errno == EINVAL);
      ok("nor a touch range", ioctl(fd, FNU_WSMOUSEGCALIB, &calib) < 0);
    }
    close(fd);
  }
  if (argc > 1 && strcmp(argv[1], "-m") == 0)
    motion();
  printf("psmtest: %s\n", failures ? "FAILED" : "ok");
  return failures != 0;
}
