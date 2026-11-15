// services: what the supervisor runs, and telling it what to do
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnu/sys.h>
#include <err.h>

#define CONTROL "/var/run/fnusvc.ctl"
#define STATUS "/var/run/fnusvc.status"
#define RC_CONF "/etc/rc.conf"
#define RC_DIR "/etc/rc.d"
#define LOGDIR "/var/log/svc"
#define NFIELD 10
#define TAIL 10

extern char *__progname;

struct row {
  char *f[NFIELD];
};

enum { NAME, STATE, PID, SINCE, RESTARTS, LAST, STAGE, REASON, CHANGES, DESC };

static void usage(void) {
  if (strcmp(__progname, "health") == 0)
    dprintf(2, "usage: health\n");
  else
    dprintf(2, "usage: services [-a]\n"
               "       services status | log name\n"
               "       services start | stop | restart | clear | run | reload name\n"
               "       services enable | disable name\n");
  exit(1);
}

static void health(void) {
  struct fnu_info sys;
  int e;

  if ((e = fnu_info(&sys)) < 0)
    errx(1, "the machine's state: %s", fnu_errstr(e));
  printf("uptime=%lu ticks\nprocesses=%u\nfree_memory=%lu KB\n",
         (unsigned long)sys.uptime, sys.nprocs, (unsigned long)(sys.free_ram / 1024));
}

static char *slurp(const char *path) {
  char *buf = 0, *grown;
  size_t len = 0, cap = 0;
  ssize_t n = 0;
  int fd;

  if ((fd = open(path, O_RDONLY)) < 0)
    return 0;
  for (;;) {
    if (len + 1 >= cap) {
      cap = cap ? cap * 2 : 1024;
      if ((grown = realloc(buf, cap)) == 0)
        err(1, 0);
      buf = grown;
    }
    if ((n = read(fd, buf + len, cap - len - 1)) <= 0)
      break;
    len += (size_t)n;
  }
  close(fd);
  buf[len] = 0;
  return buf;
}

static int rows(char *text, struct row **out) {
  struct row *r = 0;
  char *line, *next, *p;
  int n = 0, k;

  for (line = text; line != 0 && *line != 0; line = next) {
    if ((next = strchr(line, '\n')) != 0)
      *next++ = 0;
    if (*line == '#' || *line == 0)
      continue;
    if ((r = reallocarray(r, (size_t)n + 1, sizeof(*r))) == 0)
      err(1, 0);
    for (k = 0, p = line; k < NFIELD; k++) {
      r[n].f[k] = p != 0 ? p : "";
      if (p != 0 && (p = strchr(p, '\t')) != 0)
        *p++ = 0;
    }
    n++;
  }
  *out = r;
  return n;
}

static void age(long long since, char *buf, size_t size) {
  long long d = (long long)time(0) - since;

  if (since == 0)
    strlcpy(buf, "-", size);
  else if (d < 120)
    snprintf(buf, size, "%llds", d);
  else if (d < 7200)
    snprintf(buf, size, "%lldm", d / 60);
  else if (d < 172800)
    snprintf(buf, size, "%lldh", d / 3600);
  else
    snprintf(buf, size, "%lldd", d / 86400);
}

static int enabled(const char *name) {
  char *conf = slurp(RC_CONF), *line, *next, *word;
  int on = 0;

  if (conf == 0)
    return 0;
  for (line = conf; line != 0 && *line != 0 && !on; line = next) {
    if ((next = strchr(line, '\n')) != 0)
      *next++ = 0;
    if (strncmp(line, "services=", 9) != 0)
      continue;
    for (word = strtok(line + 9, " \t"); word != 0; word = strtok(0, " \t"))
      if (strcmp(word, name) == 0)
        on = 1;
  }
  free(conf);
  return on;
}

static void list(int all) {
  struct row *r;
  char *text = slurp(STATUS), when[16];
  struct dirent *de;
  DIR *d;
  int n, i, k;

  if (text == 0)
    errx(1, "no status: the supervisor is not running");
  n = rows(text, &r);
  printf("%-12s %-12s %6s %6s %4s %-6s %s\n", "NAME", "STATE", "PID", "SINCE", "RUNS",
         "STAGE", "WHAT");
  for (i = 0; i < n; i++) {
    age(atoll(r[i].f[SINCE]), when, sizeof(when));
    printf("%-12s %-12s %6s %6s %4s %-6s %s\n", r[i].f[NAME], r[i].f[STATE],
           strcmp(r[i].f[PID], "0") == 0 ? "-" : r[i].f[PID], when, r[i].f[RESTARTS],
           r[i].f[STAGE], strcmp(r[i].f[DESC], "-") == 0 ? "" : r[i].f[DESC]);
  }
  if (all && (d = opendir(RC_DIR)) != 0) {
    while ((de = readdir(d)) != 0) {
      if (de->d_name[0] == '.')
        continue;
      for (k = 0; k < n && strcmp(r[k].f[NAME], de->d_name) != 0; k++)
        ;
      if (k == n)
        printf("%-12s %-12s %6s %6s %4s %-6s\n", de->d_name, "disabled", "-", "-", "-", "-");
    }
    closedir(d);
  }
  free(r);
  free(text);
}

static int find(const char *name, struct row *out, char **keep) {
  struct row *r;
  char *text = slurp(STATUS);
  int n, i;

  if (text == 0)
    return -1;
  n = rows(text, &r);
  for (i = 0; i < n; i++)
    if (strcmp(r[i].f[NAME], name) == 0) {
      *out = r[i];
      free(r);
      *keep = text;
      return 0;
    }
  free(r);
  free(text);
  return -1;
}

static void tail(const char *path, int lines) {
  char *text = slurp(path), *p;
  int seen = 0;

  if (text == 0) {
    printf("  (no log yet)\n");
    return;
  }
  for (p = text + strlen(text); p > text; p--)
    if (p[-1] == '\n' && p[0] != 0 && ++seen > lines)
      break;
  fputs(p, stdout);
  free(text);
}

static void log_of(const char *name, char *path, size_t size) {
  char rc[PATH_MAX], *text, *p;

  snprintf(path, size, "%s/%s.log", LOGDIR, name);
  snprintf(rc, sizeof(rc), "%s/%s", RC_DIR, name);
  if ((text = slurp(rc)) == 0)
    return;
  for (p = strstr(text, "log="); p != 0; p = strstr(p + 1, "log="))
    if ((p == text || p[-1] == '\n') && p[4] == '/') {
      p[4 + strcspn(p + 4, " \t\n")] = 0;
      strlcpy(path, p + 4, size);
      break;
    }
  free(text);
}

static void status(const char *name) {
  struct row r;
  char *keep, when[16], path[PATH_MAX];

  if (find(name, &r, &keep) < 0) {
    if (access(RC_DIR, F_OK) == 0 && !enabled(name))
      errx(1, "%s is not enabled (services enable %s)", name, name);
    errx(1, "%s: the supervisor knows no such service", name);
  }
  age(atoll(r.f[SINCE]), when, sizeof(when));
  printf("%s: %s\n", r.f[NAME], strcmp(r.f[DESC], "-") == 0 ? "" : r.f[DESC]);
  printf("  state    %s for %s%s%s\n", r.f[STATE], when,
         strcmp(r.f[REASON], "-") == 0 ? "" : ": ",
         strcmp(r.f[REASON], "-") == 0 ? "" : r.f[REASON]);
  printf("  stage    %s\n", r.f[STAGE]);
  if (strcmp(r.f[PID], "0") != 0)
    printf("  pid      %s\n", r.f[PID]);
  printf("  restarts %s\n  last end %s\n", r.f[RESTARTS], r.f[LAST]);
  log_of(name, path, sizeof(path));
  printf("  file     %s/%s\n  log      %s\n", RC_DIR, name, path);
  tail(path, TAIL);
  free(keep);
}

static void tell(const char *line) {
  size_t n = strlen(line);
  int fd;

  if (n > PIPE_BUF)
    errx(1, "that name is too long");
  if ((fd = open(CONTROL, O_WRONLY | O_NONBLOCK)) < 0) {
    if (errno == ENXIO || errno == ENOENT)
      errx(1, "the supervisor is not running");
    err(1, "%s", CONTROL);
  }
  if (write(fd, line, n) != (ssize_t)n)
    err(1, "%s", CONTROL);
  close(fd);
}

// status changed after the one seen
static void watch(const char *name, const char *seen) {
  struct timespec pause = {0, 100000000};
  struct row r;
  char *keep;
  int i;

  for (i = 0; i < 50; i++) {
    nanosleep(&pause, 0);
    if (find(name, &r, &keep) < 0)
      continue;
    if (strcmp(r.f[CHANGES], seen) != 0 && strcmp(r.f[STATE], "stopping") != 0 &&
        strcmp(r.f[STATE], "waiting") != 0) {
      printf("%s: %s%s%s\n", name, r.f[STATE], strcmp(r.f[REASON], "-") == 0 ? "" : ": ",
             strcmp(r.f[REASON], "-") == 0 ? "" : r.f[REASON]);
      free(keep);
      return;
    }
    free(keep);
  }
  printf("%s: asked; see services status %s\n", name, name);
}

static void ask(const char *verb, const char *name) {
  struct row r;
  char *keep = 0, seen[16] = "", line[256];
  int up, idle, stuck;

  if (find(name, &r, &keep) < 0)
    errx(1, "%s: the supervisor knows no such service", name);
  strlcpy(seen, r.f[CHANGES], sizeof(seen));
  up = strcmp(r.f[STATE], "running") == 0 || strcmp(r.f[STATE], "starting") == 0;
  idle = strcmp(r.f[STATE], "stopped") == 0;
  stuck = strcmp(r.f[STATE], "maintenance") == 0 || strcmp(r.f[STATE], "failed") == 0 ||
          strcmp(r.f[STATE], "blocked") == 0;
  if (((strcmp(verb, "start") == 0 || strcmp(verb, "run") == 0) && up) ||
      (strcmp(verb, "stop") == 0 && idle) || (strcmp(verb, "clear") == 0 && !stuck) ||
      (strcmp(verb, "reload") == 0 && strcmp(r.f[STATE], "running") != 0)) {
    printf("%s: %s %s\n", name, strcmp(verb, "reload") == 0 ? "not reloaded," : "already",
           r.f[STATE]);
    free(keep);
    return;
  }
  free(keep);
  snprintf(line, sizeof(line), "%s %s\n", verb, name);
  tell(line);
  if (strcmp(verb, "reload") == 0)
    printf("%s: asked to reload\n", name);
  else
    watch(name, seen);
}

// rc.conf's services= list, with one name more or less
static void edit(const char *name, int on) {
  char *conf, *line, *next, rc[PATH_MAX], tmp[PATH_MAX];
  int found = 0, fd;
  FILE *f;

  snprintf(rc, sizeof(rc), "%s/%s", RC_DIR, name);
  if (on && access(rc, R_OK) < 0)
    err(1, "%s", rc);
  if ((conf = slurp(RC_CONF)) == 0)
    err(1, "%s", RC_CONF);
  snprintf(tmp, sizeof(tmp), "%s.new", RC_CONF);
  if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0 || (f = fdopen(fd, "w")) == 0)
    err(1, "%s", tmp);
  for (line = conf; line != 0 && *line != 0; line = next) {
    if ((next = strchr(line, '\n')) != 0)
      *next++ = 0;
    if (strncmp(line, "services=", 9) != 0 || found) {
      fprintf(f, "%s\n", line);
      continue;
    }
    found = 1;
    {
      char *copy = strdup(line + 9), *word;
      int first = 1, had = 0;

      if (copy == 0)
        err(1, 0);
      fputs("services=", f);
      for (word = strtok(copy, " \t"); word != 0; word = strtok(0, " \t")) {
        if (strcmp(word, name) == 0) {
          had = 1;
          if (!on)
            continue;
        }
        fprintf(f, "%s%s", first ? "" : " ", word);
        first = 0;
      }
      if (on && !had)
        fprintf(f, "%s%s", first ? "" : " ", name);
      fputc('\n', f);
      free(copy);
    }
  }
  if (!found && on)
    fprintf(f, "services=%s\n", name);
  if (fclose(f) != 0 || rename(tmp, RC_CONF) < 0)
    err(1, "%s", RC_CONF);
  free(conf);
  tell("reread\n");
  printf("%s %s\n", name, on ? "enabled" : "disabled");
}

int main(int argc, char **argv) {
  char path[PATH_MAX];
  int i;

  if (strcmp(__progname, "health") == 0) {
    if (argc > 1)
      usage();
    health();
    return 0;
  }
  if (argc == 1 || (argc == 2 && strcmp(argv[1], "-a") == 0)) {
    list(argc == 2);
    return 0;
  }
  if (argc != 3)
    usage();
  if (strcmp(argv[1], "status") == 0) {
    status(argv[2]);
    return 0;
  }
  if (strcmp(argv[1], "log") == 0) {
    log_of(argv[2], path, sizeof(path));
    tail(path, 1000);
    return 0;
  }
  if (strcmp(argv[1], "enable") == 0 || strcmp(argv[1], "disable") == 0) {
    edit(argv[2], argv[1][0] == 'e');
    return 0;
  }
  {
    static const char *const verbs[] = {"start", "stop", "restart", "clear", "run", "reload", 0};

    for (i = 0; verbs[i] != 0 && strcmp(verbs[i], argv[1]) != 0; i++)
      ;
    if (verbs[i] == 0)
      usage();
  }
  ask(argv[1], argv[2]);
  return 0;
}
