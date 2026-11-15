#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <fnu/sys.h>

static int64_t boottime;

static int is_login(const struct fnu_procinfo *p) {
  return (p->flags & (FNU_PROC_LEADER | FNU_PROC_CTTY)) ==
             (FNU_PROC_LEADER | FNU_PROC_CTTY) &&
         !(p->flags & FNU_PROC_SYSTEM) && p->login[0] != 0;
}

static void header(int users) {
  struct fnu_timespec up;
  struct fnu_info sys;
  time_t now = time(0);
  int64_t s, days, hours, mins;
  char clock[16];
  int i;

  strftime(clock, sizeof(clock), "%l:%M%p", localtime(&now));
  fnu_gettime(&up, FNU_CLOCK_UPTIME);
  s = up.sec;
  days = s / 86400;
  hours = s / 3600 % 24;
  mins = s / 60 % 60;
  printf("%s  up", clock);
  if (days > 0)
    printf(" %lld day%s,", (long long)days, days > 1 ? "s" : "");
  if (hours > 0)
    printf(" %2lld:%02lld,", (long long)hours, (long long)mins);
  else
    printf(" %lld min%s,", (long long)mins, mins != 1 ? "s" : "");
  printf(" %d user%s", users, users != 1 ? "s" : "");
  if (fnu_info(&sys) == 0) {
    printf(", load averages:");
    for (i = 0; i < 3; i++)
      printf("%s %.2f", i ? "," : "", (double)sys.loadavg[i] / FNU_FSCALE);
  }
  printf("\n");
}

static void idle(int64_t ns, char *out, size_t n) {
  struct fnu_timespec up;
  int64_t s;

  if (ns == 0) {
    snprintf(out, n, "-");
    return;
  }
  fnu_gettime(&up, FNU_CLOCK_UPTIME);
  s = up.sec - ns / 1000000000;
  if (s < 60)
    snprintf(out, n, "0");
  else if (s < 3600)
    snprintf(out, n, "%lld", (long long)(s / 60));
  else if (s < 86400)
    snprintf(out, n, "%lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60));
  else
    snprintf(out, n, "%lldday%s", (long long)(s / 86400), s >= 2 * 86400 ? "s" : "");
}

static void login_at(const struct fnu_procinfo *p, char *out, size_t n) {
  time_t when = (time_t)(boottime + p->start / 1000000000), now = time(0);

  strftime(out, n, now - when < 24 * 3600 ? "%l:%M%p" : "%a%I%p", localtime(&when));
}

static const char *what(const struct fnu_procinfo *t, int n, const struct fnu_procinfo *p) {
  int i;

  for (i = 0; i < n; i++)
    if ((int)t[i].pid == p->tpgid)
      return t[i].args[0] ? t[i].args : t[i].name;
  return p->args[0] ? p->args : p->name;
}

int main(int argc, char **argv) {
  struct fnu_procinfo *t;
  size_t len = sizeof(boottime);
  int n, i, users = 0, only_header, no_header = 0, ch;
  char when[16], quiet[16];

  only_header = strcmp(getprogname(), "uptime") == 0;
  while ((ch = getopt(argc, argv, only_header ? "" : "h")) != -1)
    switch (ch) {
    case 'h': no_header = 1; break;
    default:
      fprintf(stderr, only_header ? "usage: uptime\n" : "usage: w [-h] [user]\n");
      return 1;
    }
  argc -= optind;
  argv += optind;
  if (sysctlbyname("kern.boottime", &boottime, &len, 0, 0) < 0)
    boottime = 0;
  if ((n = fnu_processes(&t)) < 0)
    errx(1, "the processes: %s", fnu_errstr(n));
  for (i = 0; i < n; i++)
    users += is_login(&t[i]);
  if (!no_header)
    header(users);
  if (!only_header) {
    printf("%-8s %-6s %-7s %5s %s\n", "USER", "TTY", "LOGIN@", "IDLE", "WHAT");
    for (i = 0; i < n; i++) {
      if (!is_login(&t[i]) || (argc > 0 && strcmp(argv[0], t[i].login) != 0))
        continue;
      login_at(&t[i], when, sizeof(when));
      idle(t[i].ttyinput, quiet, sizeof(quiet));
      printf("%-8.8s %-6s %-7s %5s %.50s\n", t[i].login,
             strncmp(t[i].tty, "tty", 3) == 0 ? t[i].tty + 3 : t[i].tty, when,
             quiet, what(t, n, &t[i]));
    }
  }
  fnu_table_free(t);
  return 0;
}
