#include <err.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <fnu/sys.h>

enum { F_DEFAULT, F_USER, F_LONG, F_JOBS };

static int format = F_DEFAULT, all_users, no_tty_too, command_only, wide;
static long *pids;
static int npids;
static char *want_tty;
static uid_t want_uid = (uid_t)-1;
static uint64_t ram;
static int64_t boottime;

static void usage(void) {
  fprintf(stderr, "usage: ps [-acjluwx] [-p pid] [-t tty] [-U user]\n");
  exit(1);
}

static const char *user_name(uint32_t uid) {
  static char num[16];
  struct passwd *pw = getpwuid(uid);

  if (pw != 0)
    return pw->pw_name;
  snprintf(num, sizeof(num), "%u", uid);
  return num;
}

static void stat_of(const struct fnu_procinfo *p, char *out) {
  char *o = out;

  switch (p->state) {
  case FNU_PS_RUNNING:
  case FNU_PS_RUNNABLE: *o++ = 'R'; break;
  case FNU_PS_SLEEPING: *o++ = 'S'; break;
  case FNU_PS_STOPPED: *o++ = 'T'; break;
  case FNU_PS_ZOMBIE: *o++ = 'Z'; break;
  default: *o++ = 'I'; break;
  }
  if (p->nice > 0)
    *o++ = 'N';
  else if (p->nice < 0)
    *o++ = '<';
  if (p->flags & FNU_PROC_TRACED)
    *o++ = 'X';
  if (p->flags & FNU_PROC_SYSTEM)
    *o++ = 'K';
  if (p->flags & FNU_PROC_LEADER)
    *o++ = 's';
  if (p->flags & FNU_PROC_FOREGROUND)
    *o++ = '+';
  *o = 0;
}

static void cputime(const struct fnu_procinfo *p, char *out, size_t n) {
  int64_t us = p->utime + p->stime;

  snprintf(out, n, "%lld:%02lld.%02lld", (long long)(us / 60000000),
           (long long)(us / 1000000 % 60), (long long)(us / 10000 % 100));
}

static void started(const struct fnu_procinfo *p, char *out, size_t n) {
  time_t when = (time_t)(boottime + p->start / 1000000000), now = time(0);
  struct tm tm;

  localtime_r(&when, &tm);
  if (now - when < 24 * 3600)
    strftime(out, n, "%H:%M", &tm);
  else if (now - when < 7 * 24 * 3600)
    strftime(out, n, "%a%H", &tm);
  else
    strftime(out, n, "%e%b%y", &tm);
}

static const char *tty_of(const struct fnu_procinfo *p) {
  if (p->tty[0] == 0)
    return "??";
  return strncmp(p->tty, "tty", 3) == 0 ? p->tty + 3 : p->tty;
}

static int wanted(const struct fnu_procinfo *p, uid_t me) {
  int i;

  if (npids > 0) {
    for (i = 0; i < npids; i++)
      if (pids[i] == (long)p->pid)
        return 1;
    return 0;
  }
  if (want_tty != 0)
    return strcmp(tty_of(p), want_tty) == 0 || strcmp(p->tty, want_tty) == 0;
  if (want_uid != (uid_t)-1)
    return p->uid == want_uid;
  if (!all_users && p->uid != me)
    return 0;
  return no_tty_too || p->tty[0] != 0;
}

static void header(void) {
  switch (format) {
  case F_USER:
    printf("USER       PID %%CPU %%MEM   VSZ   RSS TT  STAT STARTED       TIME "
           "COMMAND\n");
    break;
  case F_LONG:
    printf("  UID   PID  PPID CPU PRI  NI   VSZ   RSS WCHAN   STAT  TT       TIME "
           "COMMAND\n");
    break;
  case F_JOBS:
    printf("USER       PID  PPID  PGID   SID TT  STAT       TIME COMMAND\n");
    break;
  default:
    printf("  PID TT  STAT        TIME COMMAND\n");
  }
}

static void command(const struct fnu_procinfo *p, int used) {
  const char *c = command_only || p->args[0] == 0 ? p->name : p->args;
  int room = wide ? 1 << 20 : 79 - used;

  printf("%.*s\n", room > 0 ? room : 0, c);
}

static void line(const struct fnu_procinfo *p) {
  char st[8], t[16], when[16];
  int used = 0;

  stat_of(p, st);
  cputime(p, t, sizeof(t));
  switch (format) {
  case F_USER:
    started(p, when, sizeof(when));
    used = printf("%-8.8s %5d %4.1f %4.1f %5llu %5llu %-3s %-4s %-7s %10s ",
                  user_name(p->uid), (int)p->pid,
                  (double)p->pctcpu * 100 / FNU_FSCALE,
                  ram ? (double)p->rss * 1024 * 100 / (double)ram : 0.0,
                  (unsigned long long)(p->memory_size / 1024),
                  (unsigned long long)p->rss, tty_of(p), st, when, t);
    break;
  case F_LONG:
    used = printf("%5u %5d %5d %3u %3u %3d %5llu %5llu %-7.7s %-4s %-3s %10s ",
                  p->uid, (int)p->pid, p->ppid,
                  (unsigned)(p->pctcpu * 100 / FNU_FSCALE), p->priority, p->nice,
                  (unsigned long long)(p->memory_size / 1024),
                  (unsigned long long)p->rss, p->wchan[0] ? p->wchan : "-", st,
                  tty_of(p), t);
    break;
  case F_JOBS:
    used = printf("%-8.8s %5d %5d %5d %5d %-3s %-4s %10s ", user_name(p->uid),
                  (int)p->pid, p->ppid, p->pgid, p->sid, tty_of(p), st, t);
    break;
  default:
    used = printf("%5d %-3s %-4s %10s ", (int)p->pid, tty_of(p), st, t);
  }
  command(p, used);
}

static int by_pid(const void *a, const void *b) {
  const struct fnu_procinfo *x = a, *y = b;

  return (x->pid > y->pid) - (x->pid < y->pid);
}

int main(int argc, char **argv) {
  struct fnu_procinfo *table;
  struct fnu_info sys;
  struct passwd *pw;
  int ch, n, i;
  size_t len = sizeof(boottime);
  char *end;

  while ((ch = getopt(argc, argv, "acjlp:t:U:uwx")) != -1)
    switch (ch) {
    case 'a': all_users = 1; break;
    case 'c': command_only = 1; break;
    case 'j': format = F_JOBS; break;
    case 'l': format = F_LONG; break;
    case 'p':
      if ((pids = reallocarray(pids, (size_t)npids + 1, sizeof(*pids))) == 0)
        err(1, 0);
      pids[npids] = strtol(optarg, &end, 10);
      if (*optarg == 0 || *end != 0)
        errx(1, "%s: not a process number", optarg);
      npids++;
      break;
    case 't': want_tty = optarg; break;
    case 'U':
      if ((pw = getpwnam(optarg)) == 0)
        errx(1, "%s: no such user", optarg);
      want_uid = pw->pw_uid;
      break;
    case 'u': format = F_USER; break;
    case 'w': wide = 1; break;
    case 'x': no_tty_too = 1; break;
    default: usage();
    }
  if (optind != argc)
    usage();

  if (fnu_info(&sys) == 0)
    ram = sys.total_ram;
  if (sysctlbyname("kern.boottime", &boottime, &len, 0, 0) < 0)
    boottime = 0;
  if ((n = fnu_processes(&table)) < 0)
    errx(1, "the processes: %s", fnu_errstr(n));
  qsort(table, (size_t)n, sizeof(*table), by_pid);
  header();
  for (i = 0; i < n; i++)
    if (table[i].pid != 0 && wanted(&table[i], geteuid()))
      line(&table[i]);
  fnu_table_free(table);
  return 0;
}
