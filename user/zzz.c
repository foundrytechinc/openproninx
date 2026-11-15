// the name decides
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <sys/sysctl.h>

extern char *__progname;

static void usage(void) {
  fprintf(stderr, "usage: %s\n", __progname);
  exit(1);
}

int main(int argc, char **argv) {
  int state = strcmp(__progname, "hibernate") == 0 ? 4 : 3;

  (void)argv;
  if (argc != 1)
    usage();
  if (geteuid() != 0)
    errx(1, "permission denied");
  if (sysctlbyname("hw.acpi.sleep", 0, 0, &state, sizeof(state)) < 0)
    err(1, "S%d", state);
  return 0;
}
