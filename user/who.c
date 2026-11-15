#include <err.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <fnu/sys.h>

int main(int argc, char **argv) {
  struct fnu_procinfo *t;
  int64_t boottime;
  size_t len = sizeof(boottime);
  int n, i, me = 0;
  char *mytty = 0, when[32];

  if (argc == 3 && strcmp(argv[1], "am") == 0 &&
      (strcmp(argv[2], "i") == 0 || strcmp(argv[2], "I") == 0))
    me = 1;
  else if (argc != 1) {
    fprintf(stderr, "usage: who [am i]\n");
    return 1;
  }
  if (me && (mytty = ttyname(0)) == 0)
    return 1;
  if (mytty != 0 && strncmp(mytty, "/dev/", 5) == 0)
    mytty += 5;
  if (sysctlbyname("kern.boottime", &boottime, &len, 0, 0) < 0)
    boottime = 0;
  if ((n = fnu_processes(&t)) < 0)
    errx(1, "the processes: %s", fnu_errstr(n));
  for (i = 0; i < n; i++) {
    time_t at;

    if ((t[i].flags & (FNU_PROC_LEADER | FNU_PROC_CTTY | FNU_PROC_SYSTEM)) !=
            (FNU_PROC_LEADER | FNU_PROC_CTTY) ||
        t[i].login[0] == 0 || (mytty != 0 && strcmp(mytty, t[i].tty) != 0))
      continue;
    at = (time_t)(boottime + t[i].start / 1000000000);
    strftime(when, sizeof(when), "%b %e %H:%M", localtime(&at));
    printf("%-8.8s %-8s %s\n", t[i].login, t[i].tty, when);
  }
  fnu_table_free(t);
  return 0;
}
