// the name decides
#include <sys/types.h>
#include <sys/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <fnu/sys.h>

extern char *__progname;

static void usage(void) {
  fprintf(stderr, "usage: %s [-q]\n", __progname);
  exit(1);
}

int main(int argc, char **argv) {
  int ch, qflag = 0, howto, r;

  if (strcmp(__progname, "halt") == 0)
    howto = RB_HALT;
  else if (strcmp(__progname, "poweroff") == 0)
    howto = RB_POWEROFF;
  else
    howto = RB_AUTOBOOT;
  while ((ch = getopt(argc, argv, "q")) != -1) {
    switch (ch) {
    case 'q':
      qflag = 1;
      break;
    default:
      usage();
    }
  }
  if (argc != optind)
    usage();
  if (geteuid() != 0)
    errx(1, "%s", "permission denied");
  if (qflag) {
    reboot(howto);
    err(1, "reboot");
  }
  r = howto == RB_AUTOBOOT ? fnu_reboot() : fnu_poweroff();
  if (r < 0)
    errx(1, "%s: %s", __progname, fnu_errstr(r));
  return 0;
}
