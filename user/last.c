#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>

struct ended {
  char line[UT_LINESIZE + 1];
  time_t at;
  struct ended *next;
};

static struct ended *ends;
static time_t down_at;
static const char *down_what;
static char **names;
static int nnames;

static void usage(void) {
  fprintf(stderr, "usage: last [-n count] [name | tty ...]\n");
  exit(1);
}

static void field(char *out, const char *in, size_t n) {
  memcpy(out, in, n);
  out[n] = 0;
}

static int wanted(const char *name, const char *line) {
  int i;

  if (nnames == 0)
    return 1;
  for (i = 0; i < nnames; i++)
    if (strcmp(names[i], name) == 0 || strcmp(names[i], line) == 0 ||
        (strncmp(line, "tty", 3) == 0 && strcmp(names[i], line + 3) == 0))
      return 1;
  return 0;
}

static time_t ended_at(const char *line) {
  struct ended **pp, *e;
  time_t at;

  for (pp = &ends; (e = *pp) != 0; pp = &e->next)
    if (strcmp(e->line, line) == 0) {
      *pp = e->next;
      at = e->at;
      free(e);
      return at;
    }
  return 0;
}

static void ended_add(const char *line, time_t at) {
  struct ended *e;

  ended_at(line);
  if ((e = calloc(1, sizeof(*e))) == 0)
    err(1, 0);
  strlcpy(e->line, line, sizeof(e->line));
  e->at = at;
  e->next = ends;
  ends = e;
}

static void show(const struct utmp *u, const char *name, const char *line) {
  char host[UT_HOSTSIZE + 1], when[32], till[16];
  time_t t = u->ut_time, end;
  int64_t d;

  field(host, u->ut_host, UT_HOSTSIZE);
  strftime(when, sizeof(when), "%a %b %e %H:%M", localtime(&t));
  printf("%-8.8s %-8.8s %-16.16s %s", name, line, host, when);
  if (strcmp(line, "~") == 0) {
    printf("\n");
    return;
  }
  end = ended_at(line);
  if (end == 0 && down_at != 0) {
    end = down_at;
    printf(" - %-5s", down_what);
  } else if (end == 0) {
    printf("   still logged in\n");
    return;
  } else {
    strftime(till, sizeof(till), "%H:%M", localtime(&end));
    printf(" - %s", till);
  }
  d = (int64_t)(end - t);
  if (d < 0)
    d = 0;
  if (d >= 86400)
    printf("  (%lld+%02lld:%02lld)\n", (long long)(d / 86400),
           (long long)(d / 3600 % 24), (long long)(d / 60 % 60));
  else
    printf("  (%02lld:%02lld)\n", (long long)(d / 3600), (long long)(d / 60 % 60));
}

int main(int argc, char **argv) {
  struct utmp *all;
  struct stat st;
  long count = -1, shown = 0;
  size_t n, i;
  int fd, ch;
  char *end;

  while ((ch = getopt(argc, argv, "n:")) != -1)
    switch (ch) {
    case 'n':
      count = strtol(optarg, &end, 10);
      if (*optarg == 0 || *end != 0 || count < 0)
        errx(1, "%s: not a count", optarg);
      break;
    default:
      usage();
    }
  names = argv + optind;
  nnames = argc - optind;

  if ((fd = open(_PATH_WTMP, O_RDONLY)) < 0 || fstat(fd, &st) < 0)
    err(1, "%s", _PATH_WTMP);
  n = (size_t)st.st_size / sizeof(struct utmp);
  if ((all = calloc(n + 1, sizeof(*all))) == 0)
    err(1, 0);
  if (n > 0 && read(fd, all, n * sizeof(*all)) != (ssize_t)(n * sizeof(*all)))
    err(1, "%s", _PATH_WTMP);
  close(fd);

  for (i = n; i-- > 0 && (count < 0 || shown < count);) {
    char name[UT_NAMESIZE + 1], line[UT_LINESIZE + 1];

    field(name, all[i].ut_name, UT_NAMESIZE);
    field(line, all[i].ut_line, UT_LINESIZE);
    if (strcmp(line, "~") == 0) {
      while (ends != 0)
        ended_at(ends->line);
      down_at = all[i].ut_time;
      down_what = strcmp(name, "shutdown") == 0 ? "down" : "crash";
      if (wanted(name, line)) {
        show(&all[i], name, line);
        shown++;
      }
      continue;
    }
    if (name[0] == 0) {
      ended_add(line, all[i].ut_time);
      continue;
    }
    if (wanted(name, line)) {
      show(&all[i], name, line);
      shown++;
    } else
      ended_at(line);
  }
  if (n > 0) {
    time_t first = all[0].ut_time;

    printf("\nwtmp begins %s", ctime(&first));
  }
  free(all);
  return 0;
}
