// fnusvc: brings the services up in their order, keeps them up, takes them down
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fnu/sys.h>
#include "inc/product.h"
#include <err.h>
#include <util.h>

#define RUNDIR "/var/run"
#define CONTROL RUNDIR "/fnusvc.ctl"
#define STATUS RUNDIR "/fnusvc.status"
#define PIDFILE RUNDIR "/fnusvc.pid"
#define LOGDIR "/var/log/svc"
#define INIT_CONF "/etc/init.conf"
#define RC_CONF "/etc/rc.conf"
#define RC_DIR "/etc/rc.d"
#define SHELL "/usr/bin/sh"
#define STDPATH "/usr/bin:/bin:/usr/sbin:/sbin"

// respawning too fast
#define SPACING 5
#define NSPACE 3
#define SPACE_SLEEP 30
// five failures within ten minutes
#define LIMIT 5
#define WINDOW 600
#define MAXLIMIT 32
#define STEP_TIMEOUT 300
#define STOP_TIMEOUT 10
#define LOG_ROLL (256 * 1024)
#define PENDING_MAX 4096

enum { BOOT, MULTI };
enum { R_ALWAYS, R_FAILURE, R_NEVER };
enum {
  S_WAITING,
  S_STARTING,
  S_RUNNING,
  S_DONE,
  S_FAILED,
  S_MAINT,
  S_STOPPING,
  S_STOPPED,
  S_ABSENT,
  S_BLOCKED
};

static const char *const state_names[] = {
    "waiting", "starting", "running", "done",   "failed",
    "maintenance", "stopping", "stopped", "absent", "blocked"};
static const char *const stage_names[] = {"boot", "multi"};

struct service {
  char *name, *text;
  char *desc;
  int stage;
  char *run;
  char *step, *stop;
  char **requires, **before, **provides;
  int restart;
  int limit, window;
  char *user, *dir, *log, *tty;
  int insecure;
  int timeout;
  char **env;
  int order;

  int state;
  pid_t pid, stoppid;
  time_t since, started, again, deadline;
  int restarts, lastexit, quick, killed, gone;
  unsigned changes;
  time_t fails[MAXLIMIT];
  int nfails;
  char reason[128];
  int *deps, ndeps;
  const char *lack;
  char *pending;
  size_t npending;
};

static struct service **svc;
static int nsvc;
static int target = MULTI, trace;
static int ctl = -1;
static volatile sig_atomic_t got_chld, got_term, got_hup;
static int shutting;
static int notify = -1;
static struct pidfh *pfh;

// FILES

static char *slurp(const char *path) {
  char *buf = 0, *grown;
  size_t len = 0, cap = 0;
  ssize_t n = 0;
  int fd;

  if ((fd = open(path, O_RDONLY)) < 0)
    return 0;
  for (;;) {
    if (len + 1 >= cap) {
      cap = cap ? cap * 2 : 512;
      if ((grown = realloc(buf, cap)) == 0) {
        free(buf);
        close(fd);
        return 0;
      }
      buf = grown;
    }
    if ((n = read(fd, buf + len, cap - len - 1)) <= 0)
      break;
    len += (size_t)n;
  }
  close(fd);
  if (n < 0) {
    free(buf);
    return 0;
  }
  buf[len] = 0;
  return buf;
}

static char **split(char *line) {
  char **out, *p;
  int n = 0, i;

  for (p = line; *p != 0;) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == 0)
      break;
    n++;
    while (*p != 0 && *p != ' ' && *p != '\t')
      p++;
  }
  if ((out = calloc((size_t)n + 1, sizeof(*out))) == 0)
    err(1, 0);
  for (i = 0, p = line; i < n; i++) {
    while (*p == ' ' || *p == '\t')
      p++;
    out[i] = p;
    while (*p != 0 && *p != ' ' && *p != '\t')
      p++;
    if (*p != 0)
      *p++ = 0;
  }
  return out;
}

// name=value; the value runs to the end of the line
static int keyval(char *line, char **key, char **value) {
  char *eq, *end;

  while (*line == ' ' || *line == '\t')
    line++;
  if (*line == '#' || *line == 0 || (eq = strchr(line, '=')) == 0)
    return 0;
  *eq = 0;
  for (end = eq; end > line && (end[-1] == ' ' || end[-1] == '\t');)
    *--end = 0;
  for (eq++; *eq == ' ' || *eq == '\t'; eq++)
    ;
  for (end = eq + strlen(eq); end > eq && (end[-1] == ' ' || end[-1] == '\t' ||
                                           end[-1] == '\r');)
    *--end = 0;
  *key = line;
  *value = eq;
  return 1;
}

static void read_init_conf(void) {
  char *text, *line, *next, *key, *value;

  if ((text = slurp(INIT_CONF)) == 0)
    return;
  for (line = text; line != 0 && *line != 0; line = next) {
    if ((next = strchr(line, '\n')) != 0)
      *next++ = 0;
    if (!keyval(line, &key, &value))
      continue;
    if (strcmp(key, "start") == 0)
      target = strcmp(value, "boot") == 0 ? BOOT : MULTI;
    else if (strcmp(key, "trace") == 0)
      trace = strcmp(value, "yes") == 0;
  }
  free(text);
}

static int number(const char *v, int dflt) {
  char *end;
  long n = strtol(v, &end, 10);

  return *v == 0 || *end != 0 || n < 0 || n > 1000000 ? dflt : (int)n;
}

static char **append(char **list, char *item) {
  int n = 0;

  while (list != 0 && list[n] != 0)
    n++;
  if ((list = reallocarray(list, (size_t)n + 2, sizeof(*list))) == 0)
    err(1, 0);
  list[n] = item;
  list[n + 1] = 0;
  return list;
}

static struct service *read_service(const char *name, int order) {
  char path[PATH_MAX], *line, *next, *key, *value, *slash;
  struct service *s;

  snprintf(path, sizeof(path), "%s/%s", RC_DIR, name);
  if ((s = calloc(1, sizeof(*s))) == 0)
    err(1, 0);
  if ((s->text = slurp(path)) == 0) {
    warn("%s", path);
    free(s);
    return 0;
  }
  if ((s->name = strdup(name)) == 0)
    err(1, 0);
  s->stage = MULTI;
  s->restart = -1;
  s->limit = LIMIT;
  s->window = WINDOW;
  s->timeout = -1;
  s->order = order;
  s->pid = s->stoppid = -1;
  s->lastexit = -1;
  for (line = s->text; line != 0 && *line != 0; line = next) {
    if ((next = strchr(line, '\n')) != 0)
      *next++ = 0;
    if (!keyval(line, &key, &value))
      continue;
    if (strcmp(key, "desc") == 0)
      s->desc = value;
    else if (strcmp(key, "stage") == 0) {
      if (strcmp(value, "boot") == 0)
        s->stage = BOOT;
      else if (strcmp(value, "multi") != 0)
        warnx("%s: no stage called %s", path, value);
    } else if (strcmp(key, "run") == 0)
      s->run = value;
    else if (strcmp(key, "step") == 0)
      s->step = value;
    else if (strcmp(key, "stop") == 0)
      s->stop = value;
    else if (strcmp(key, "requires") == 0)
      s->requires = split(value);
    else if (strcmp(key, "before") == 0)
      s->before = split(value);
    else if (strcmp(key, "provides") == 0)
      s->provides = split(value);
    else if (strcmp(key, "restart") == 0) {
      s->restart = strcmp(value, "always") == 0    ? R_ALWAYS
                   : strcmp(value, "failure") == 0 ? R_FAILURE
                   : strcmp(value, "never") == 0   ? R_NEVER
                                                   : -1;
      if (s->restart < 0)
        warnx("%s: restart is always, failure or never", path);
    } else if (strcmp(key, "limit") == 0) {
      if ((slash = strchr(value, '/')) != 0) {
        *slash = 0;
        s->window = number(slash + 1, WINDOW);
      }
      s->limit = number(value, LIMIT);
      if (s->limit > MAXLIMIT)
        s->limit = MAXLIMIT;
    } else if (strcmp(key, "user") == 0)
      s->user = value;
    else if (strcmp(key, "dir") == 0)
      s->dir = value;
    else if (strcmp(key, "log") == 0)
      s->log = value;
    else if (strcmp(key, "on") == 0)
      s->tty = value;
    else if (strcmp(key, "secure") == 0)
      s->insecure = strcmp(value, "no") == 0;
    else if (strcmp(key, "timeout") == 0)
      s->timeout = number(value, -1);
    else if (strcmp(key, "env") == 0)
      s->env = append(s->env, value);
    else
      warnx("%s: %s is not a service's field", path, key);
  }
  if ((s->run == 0 || *s->run == 0) && s->step == 0) {
    warnx("%s names nothing to run and no step", path);
    free(s->text);
    free(s->name);
    free(s);
    return 0;
  }
  if (s->run != 0 && *s->run != 0 && s->step != 0)
    warnx("%s: both run and step; the step is ignored", path);
  if (s->run != 0 && *s->run != 0)
    s->step = 0;
  if (s->restart < 0)
    s->restart = s->step != 0 ? R_NEVER : R_ALWAYS;
  if (s->timeout < 0)
    s->timeout = s->step != 0 ? STEP_TIMEOUT : STOP_TIMEOUT;
  return s;
}

static void free_service(struct service *s) {
  free(s->requires);
  free(s->before);
  free(s->provides);
  free(s->env);
  free(s->deps);
  free(s->pending);
  free(s->text);
  free(s->name);
  free(s);
}

// the enabled ones, in rc.conf's order
static struct service **read_services(int *count) {
  char *conf, *line, *next, *key, *value, **words;
  struct service **list = 0, *s;
  int n = 0, i, k;

  *count = -1;
  if ((conf = slurp(RC_CONF)) == 0) {
    warn("%s", RC_CONF);
    return 0;
  }
  for (line = conf; line != 0 && *line != 0; line = next) {
    if ((next = strchr(line, '\n')) != 0)
      *next++ = 0;
    if (!keyval(line, &key, &value))
      continue;
    if (strcmp(key, "services") != 0) {
      warnx("%s: %s is not a setting here", RC_CONF, key);
      continue;
    }
    words = split(value);
    for (i = 0; words[i] != 0; i++) {
      for (k = 0; k < n && strcmp(list[k]->name, words[i]) != 0; k++)
        ;
      if (k < n || (s = read_service(words[i], n)) == 0)
        continue;
      if ((list = reallocarray(list, (size_t)n + 1, sizeof(*list))) == 0)
        err(1, 0);
      list[n++] = s;
    }
    free(words);
  }
  free(conf);
  *count = n;
  return list;
}

// ORDER

static int find(struct service **list, int n, const char *name) {
  int i, k;

  for (i = 0; i < n; i++) {
    if (strcmp(list[i]->name, name) == 0)
      return i;
    for (k = 0; list[i]->provides != 0 && list[i]->provides[k] != 0; k++)
      if (strcmp(list[i]->provides[k], name) == 0)
        return i;
  }
  return -1;
}

static void add_dep(struct service *s, int on) {
  int i;

  for (i = 0; i < s->ndeps; i++)
    if (s->deps[i] == on)
      return;
  if ((s->deps = reallocarray(s->deps, (size_t)s->ndeps + 1, sizeof(int))) == 0)
    err(1, 0);
  s->deps[s->ndeps++] = on;
}

// requires and before are edges
static void resolve(void) {
  int i, k, j, *indeg, *order, done = 0, progress;

  for (i = 0; i < nsvc; i++) {
    struct service *s = svc[i];

    free(s->deps);
    s->deps = 0;
    s->ndeps = 0;
    s->lack = 0;
    for (k = 0; s->requires != 0 && s->requires[k] != 0; k++) {
      if ((j = find(svc, nsvc, s->requires[k])) < 0) {
        if (s->lack == 0)
          s->lack = s->requires[k];
        continue;
      }
      if (j != i)
        add_dep(s, j);
    }
  }
  for (i = 0; i < nsvc; i++)
    for (k = 0; svc[i]->before != 0 && svc[i]->before[k] != 0; k++)
      if ((j = find(svc, nsvc, svc[i]->before[k])) >= 0 && j != i)
        add_dep(svc[j], i);
  for (i = 0; i < nsvc; i++)
    for (k = 0; k < svc[i]->ndeps; k++)
      if (svc[svc[i]->deps[k]]->stage > svc[i]->stage) {
        warnx("%s is a boot step but needs %s, which is not", svc[i]->name,
              svc[svc[i]->deps[k]]->name);
        svc[i]->deps[k--] = svc[i]->deps[--svc[i]->ndeps];
      }
  // a cycle gets named, its edges dropped
  indeg = calloc((size_t)nsvc + 1, sizeof(int));
  order = calloc((size_t)nsvc + 1, sizeof(int));
  if (indeg == 0 || order == 0)
    err(1, 0);
  for (i = 0; i < nsvc; i++)
    indeg[i] = svc[i]->ndeps;
  for (;;) {
    progress = 0;
    for (i = 0; i < nsvc; i++) {
      if (indeg[i] != 0 || order[i])
        continue;
      order[i] = 1;
      done++;
      progress = 1;
      for (j = 0; j < nsvc; j++)
        for (k = 0; k < svc[j]->ndeps; k++)
          if (svc[j]->deps[k] == i)
            indeg[j]--;
    }
    if (done == nsvc)
      break;
    if (progress)
      continue;
    dprintf(2, "fnusvc: a circle of requirements among:");
    for (i = 0; i < nsvc; i++)
      if (!order[i])
        dprintf(2, " %s", svc[i]->name);
    dprintf(2, "\n");
    for (i = 0; i < nsvc; i++)
      if (!order[i]) {
        for (k = 0; k < svc[i]->ndeps; k++)
          if (!order[svc[i]->deps[k]])
            svc[i]->deps[k--] = svc[i]->deps[--svc[i]->ndeps];
        indeg[i] = 0;
      }
  }
  free(indeg);
  free(order);
}

// LOGS AND STATUS

static void stamp(char *buf, size_t size) {
  time_t now = time(0);
  struct tm *tm = localtime(&now);

  if (tm == 0 || strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm) == 0)
    snprintf(buf, size, "%lld", (long long)now);
}

static void log_path(struct service *s, char *path, size_t size) {
  if (s->log != 0 && s->log[0] == '/')
    strlcpy(path, s->log, size);
  else
    snprintf(path, size, "%s/%s.log", LOGDIR, s->name);
}

// one older generation kept
static void log_roll(const char *path) {
  char old[PATH_MAX];
  struct stat st;

  if (stat(path, &st) == 0 && st.st_size > LOG_ROLL) {
    snprintf(old, sizeof(old), "%s.0", path);
    rename(path, old);
  }
}

static int log_open(struct service *s) {
  char path[PATH_MAX];

  if (s->log != 0 && strcmp(s->log, "console") == 0)
    return -1;
  mkdir("/var/log", 0755);
  mkdir(LOGDIR, 0755);
  log_path(s, path, sizeof(path));
  log_roll(path);
  return open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
}

// held while the volume is read-only, written when not
static void note(struct service *s, const char *line) {
  size_t n = strlen(line);
  char *grown;
  int fd;

  if ((fd = log_open(s)) >= 0) {
    if (s->npending > 0)
      write(fd, s->pending, s->npending);
    write(fd, line, n);
    close(fd);
    s->npending = 0;
    return;
  }
  if (s->log != 0 && strcmp(s->log, "console") == 0)
    return;
  if (s->npending + n > PENDING_MAX)
    return;
  if ((grown = realloc(s->pending, s->npending + n)) == 0)
    return;
  s->pending = grown;
  memcpy(s->pending + s->npending, line, n);
  s->npending += n;
}

static void event(struct service *s, int loud, const char *fmt, ...) {
  char what[160], when[32], line[224];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(what, sizeof(what), fmt, ap);
  va_end(ap);
  stamp(when, sizeof(when));
  snprintf(line, sizeof(line), "[%s] %s\n", when, what);
  note(s, line);
  if (loud || trace) {
    printf("fnusvc: %s: %s\n", s->name, what);
    fflush(stdout);
  }
}

static void set_state(struct service *s, int state, const char *fmt, ...) {
  va_list ap;

  s->state = state;
  s->since = time(0);
  s->changes++;
  s->reason[0] = 0;
  if (fmt != 0) {
    va_start(ap, fmt);
    vsnprintf(s->reason, sizeof(s->reason), fmt, ap);
    va_end(ap);
  }
  if (state == S_FAILED || state == S_MAINT || state == S_BLOCKED)
    event(s, 1, "%s: %s", state_names[state], s->reason);
  else if (s->reason[0] != 0)
    event(s, 0, "%s: %s", state_names[state], s->reason);
}

static void how_ended(int status, char *buf, size_t size) {
  if (status < 0)
    strlcpy(buf, "-", size);
  else if (WIFSIGNALED(status))
    snprintf(buf, size, "signal %d", WTERMSIG(status));
  else
    snprintf(buf, size, "exit %d", WEXITSTATUS(status));
}

static void publish(void) {
  char last[32];
  FILE *f;
  int i;

  if ((f = fopen(STATUS ".new", "w")) == 0)
    return;
  fprintf(f, "# name\tstate\tpid\tsince\trestarts\tlast\tstage\treason\tchanges\tdesc\n");
  for (i = 0; i < nsvc; i++) {
    struct service *s = svc[i];

    how_ended(s->lastexit, last, sizeof(last));
    fprintf(f, "%s\t%s\t%d\t%lld\t%d\t%s\t%s\t%s\t%u\t%s\n", s->name,
            state_names[s->state], s->pid > 0 ? (int)s->pid : 0,
            (long long)s->since, s->restarts, last, stage_names[s->stage],
            s->reason[0] != 0 ? s->reason : "-", s->changes, s->desc != 0 ? s->desc : "-");
  }
  if (fclose(f) == 0)
    rename(STATUS ".new", STATUS);
}

// STARTING

static void child_env(struct service *s, const struct passwd *pw) {
  static char *none[] = {0};
  int i;

  environ = none;
  setenv("PATH", STDPATH, 1);
  setenv("HOME", pw != 0 && pw->pw_dir[0] != 0 ? pw->pw_dir : "/", 1);
  setenv("SHELL", pw != 0 && pw->pw_shell[0] != 0 ? pw->pw_shell : SHELL, 1);
  if (pw != 0) {
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
  }
  setenv("SERVICE", s->name, 1);
  if (s->insecure)
    setenv("LOGIN_SECURE", "no", 1);
  for (i = 0; s->env != 0 && s->env[i] != 0; i++) {
    char *eq = strchr(s->env[i], '=');

    if (eq != 0) {
      *eq = 0;
      setenv(s->env[i], eq + 1, 1);
      *eq = '=';
    }
  }
}

// /var/run/<name>
static void run_dir(struct service *s, const struct passwd *pw) {
  char path[PATH_MAX];

  snprintf(path, sizeof(path), "%s/%s", RUNDIR, s->name);
  if (mkdir(path, 0755) < 0 && errno != EEXIST)
    return;
  if (pw != 0)
    chown(path, pw->pw_uid, pw->pw_gid);
}

static pid_t spawn(struct service *s, const char *line) {
  struct passwd *pw = 0, copy;
  sigset_t none;
  pid_t pid;
  int fd, logfd = -1;

  if (s->user != 0 && (pw = getpwnam(s->user)) == 0) {
    set_state(s, S_FAILED, "no account %s", s->user);
    return -1;
  }
  if (pw == 0)
    pw = getpwuid(0);
  if (pw != 0)
    copy = *pw;
  if (s->tty == 0) {
    run_dir(s, pw != 0 ? &copy : 0);
    logfd = log_open(s);
  }
  if ((pid = fork()) < 0) {
    if (logfd >= 0)
      close(logfd);
    set_state(s, S_FAILED, "fork: %s", strerror(errno));
    return -1;
  }
  if (pid > 0) {
    if (logfd >= 0)
      close(logfd);
    return pid;
  }
  sigemptyset(&none);
  signal(SIGCHLD, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  signal(SIGHUP, SIG_DFL);
  sigprocmask(SIG_SETMASK, &none, 0);
  if (s->tty != 0) {
    if ((fd = open(s->tty, O_RDWR)) < 0 || login_tty(fd) < 0)
      err(1, "%s", s->tty);
  } else {
    setsid();
    if ((fd = open("/dev/null", O_RDWR)) >= 0 && fd != 0) {
      dup2(fd, 0);
      close(fd);
    }
    if (logfd < 0)
      logfd = open("/dev/console", O_WRONLY);
    if (logfd >= 0) {
      fcntl(logfd, F_SETFD, 0);
      dup2(logfd, 1);
      dup2(logfd, 2);
      if (logfd > 2)
        close(logfd);
    }
  }
  if (pw != 0) {
    pw = &copy;
    if (pw->pw_uid != 0 &&
        (setgid(pw->pw_gid) < 0 || initgroups(pw->pw_name, pw->pw_gid) < 0 ||
         setuid(pw->pw_uid) < 0))
      err(1, "%s: become %s", s->name, pw->pw_name);
  }
  child_env(s, pw);
  if (chdir(s->dir != 0 ? s->dir : "/") < 0)
    err(1, "%s: %s", s->name, s->dir);
  if (line == 0) {
    // sh exec: one process
    char *run;

    if (asprintf(&run, "exec %s", s->run) < 0)
      err(1, 0);
    line = run;
  }
  execl(SHELL, "sh", "-c", line, (char *)0);
  err(1, "%s", SHELL);
}

static void start(struct service *s) {
  pid_t pid;
  int fd;

  if (s->tty != 0) {
    if ((fd = open(s->tty, O_RDWR)) < 0) {
      set_state(s, S_ABSENT, "no %s on this machine", s->tty);
      return;
    }
    close(fd);
  }
  s->killed = 0;
  if ((pid = spawn(s, s->step)) < 0)
    return;
  s->pid = pid;
  s->started = time(0);
  if (s->step != 0) {
    s->deadline = s->started + s->timeout;
    set_state(s, S_STARTING, 0);
    event(s, 0, "step started, pid %d", (int)pid);
  } else {
    set_state(s, S_RUNNING, 0);
    event(s, 0, "started, pid %d", (int)pid);
  }
}

// a step done or a service running meets a requirement
static int ready(struct service *s, char *why, size_t size) {
  int i;

  if (s->stage > target)
    return 0;
  if (s->lack != 0) {
    snprintf(why, size, "needs %s, which is not enabled", s->lack);
    return -1;
  }
  if (s->stage == MULTI)
    for (i = 0; i < nsvc; i++)
      if (svc[i]->stage == BOOT &&
          (svc[i]->state == S_WAITING || svc[i]->state == S_STARTING))
        return 0;
  for (i = 0; i < s->ndeps; i++) {
    struct service *d = svc[s->deps[i]];

    switch (d->state) {
    case S_DONE:
    case S_RUNNING:
      continue;
    case S_WAITING:
    case S_STARTING:
    case S_STOPPING:
      return 0;
    default:
      snprintf(why, size, "needs %s, which is %s", d->name, state_names[d->state]);
      return -1;
    }
  }
  return 1;
}

// STOPPING

static int has_dependents_up(int i) {
  int j, k;

  for (j = 0; j < nsvc; j++) {
    if (svc[j]->state != S_RUNNING && svc[j]->state != S_STARTING &&
        svc[j]->state != S_STOPPING)
      continue;
    for (k = 0; k < svc[j]->ndeps; k++)
      if (svc[j]->deps[k] == i)
        return 1;
  }
  return 0;
}

static void stop(struct service *s) {
  time_t now = time(0);

  switch (s->state) {
  case S_RUNNING:
  case S_STARTING:
    s->deadline = now + s->timeout;
    set_state(s, S_STOPPING, 0);
    if (s->stop != 0 && s->step == 0 && (s->stoppid = spawn(s, s->stop)) > 0) {
      event(s, 0, "stop command, pid %d", (int)s->stoppid);
      return;
    }
    if (s->pid > 0) {
      event(s, 0, "SIGTERM to %d", (int)s->pid);
      if (kill(-s->pid, SIGTERM) < 0)
        kill(s->pid, SIGTERM);
    }
    return;
  case S_DONE:
    if (s->stop != 0) {
      s->deadline = now + s->timeout;
      set_state(s, S_STOPPING, 0);
      if ((s->stoppid = spawn(s, s->stop)) > 0) {
        event(s, 0, "stop command, pid %d", (int)s->stoppid);
        return;
      }
    }
    set_state(s, S_STOPPED, 0);
    return;
  case S_STOPPING:
    return;
  default:
    set_state(s, S_STOPPED, 0);
  }
}

// EXITS

static int failures_in_window(struct service *s, time_t now) {
  int i, n = 0;

  for (i = 0; i < s->nfails; i++)
    if (now - s->fails[i] < s->window)
      n++;
  return n;
}

static void record_failure(struct service *s, time_t now) {
  int i;

  if (s->nfails < MAXLIMIT) {
    s->fails[s->nfails++] = now;
    return;
  }
  for (i = 1; i < MAXLIMIT; i++)
    s->fails[i - 1] = s->fails[i];
  s->fails[MAXLIMIT - 1] = now;
}

static void ended(struct service *s, int status) {
  char how[32];
  time_t now = time(0);
  int failed = WIFSIGNALED(status) || WEXITSTATUS(status) != 0;

  s->pid = -1;
  s->lastexit = status;
  how_ended(status, how, sizeof(how));
  if (s->tty != 0)
    logwtmp(strncmp(s->tty, "/dev/", 5) == 0 ? s->tty + 5 : s->tty, "", "");
  if (s->state == S_STOPPING) {
    if (s->stoppid <= 0)
      set_state(s, S_STOPPED, 0);
    event(s, 0, "ended (%s)", how);
    return;
  }
  if (s->step != 0) {
    if (s->killed)
      set_state(s, S_FAILED, "ran past %d s", s->timeout);
    else if (failed)
      set_state(s, S_FAILED, "step ended with %s", how);
    else {
      set_state(s, S_DONE, 0);
      event(s, 0, "step done in %lld s", (long long)(now - s->started));
    }
    return;
  }
  event(s, failed, "ended (%s) after %lld s", how, (long long)(now - s->started));
  if (s->restart == R_NEVER || (s->restart == R_FAILURE && !failed)) {
    if (failed)
      set_state(s, S_FAILED, "ended with %s", how);
    else
      set_state(s, S_STOPPED, "ended with %s", how);
    return;
  }
  if (failed) {
    record_failure(s, now);
    if (failures_in_window(s, now) >= s->limit) {
      set_state(s, S_MAINT, "restarting too quickly: %d failures in %d s", s->limit,
                s->window);
      return;
    }
  }
  if (now - s->started < SPACING) {
    if (++s->quick >= NSPACE) {
      s->quick = 0;
      s->again = now + SPACE_SLEEP;
      s->restarts++;
      set_state(s, S_WAITING, "repeating too quickly, again in %d s", SPACE_SLEEP);
      return;
    }
  } else
    s->quick = 0;
  s->again = 0;
  s->restarts++;
  set_state(s, S_WAITING, 0);
}

static void stop_ended(struct service *s, int status) {
  char how[32];

  s->stoppid = -1;
  how_ended(status, how, sizeof(how));
  event(s, 0, "stop command ended (%s)", how);
  if (s->pid > 0) {
    if (kill(-s->pid, SIGTERM) < 0)
      kill(s->pid, SIGTERM);
    return;
  }
  set_state(s, S_STOPPED, 0);
}

static void reap(void) {
  pid_t pid;
  int status, i;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    for (i = 0; i < nsvc; i++) {
      if (svc[i]->pid == pid) {
        ended(svc[i], status);
        break;
      }
      if (svc[i]->stoppid == pid) {
        stop_ended(svc[i], status);
        break;
      }
    }
}

// THE LOOP

static time_t advance(void) {
  char why[128];
  time_t now = time(0), next = 0;
  int i, r, changed;

#define SOONER(t) (next = next == 0 || (t) < next ? (t) : next)
  do {
    changed = 0;
    for (i = 0; i < nsvc; i++) {
      struct service *s = svc[i];

      if ((s->state == S_STARTING || s->state == S_STOPPING) && s->deadline != 0) {
        if (now >= s->deadline && !s->killed) {
          s->killed = 1;
          s->deadline = now + STOP_TIMEOUT;
          event(s, 1, "%s past %d s: SIGKILL",
                s->state == S_STARTING ? "step ran" : "stop ran", s->timeout);
          if (s->pid > 0 && kill(-s->pid, SIGKILL) < 0)
            kill(s->pid, SIGKILL);
          if (s->stoppid > 0)
            kill(s->stoppid, SIGKILL);
        } else if (now < s->deadline)
          SOONER(s->deadline);
      }
      if (shutting) {
        if (s->state != S_STOPPED && s->state != S_STOPPING && !has_dependents_up(i)) {
          stop(s);
          changed = 1;
        }
        continue;
      }
      // blocked: waits on its requirements
      if (s->state == S_BLOCKED) {
        if (ready(s, why, sizeof(why)) > 0) {
          start(s);
          changed = 1;
        }
        continue;
      }
      if (s->state != S_WAITING)
        continue;
      if (s->again > now) {
        SOONER(s->again);
        continue;
      }
      if ((r = ready(s, why, sizeof(why))) < 0) {
        set_state(s, S_BLOCKED, "%s", why);
        changed = 1;
      } else if (r > 0) {
        start(s);
        changed = 1;
      }
    }
  } while (changed);
  return next;
}

static int all_stopped(void) {
  int i;

  for (i = 0; i < nsvc; i++)
    if (svc[i]->state != S_STOPPED || svc[i]->pid > 0 || svc[i]->stoppid > 0)
      return 0;
  return 1;
}

// COMMANDS

static void wake(struct service *s) {
  s->again = 0;
  s->quick = 0;
  s->killed = 0;
  set_state(s, S_WAITING, 0);
}

// re-read rc.conf and rc.d; what is still enabled keeps running
static void reread(void) {
  struct service **fresh, *old, *s;
  int n, i, k;

  fresh = read_services(&n);
  if (n < 0)
    return;
  for (i = 0; i < n; i++) {
    s = fresh[i];
    for (k = 0; k < nsvc && strcmp(svc[k]->name, s->name) != 0; k++)
      ;
    if (k == nsvc) {
      set_state(s, S_WAITING, 0);
      continue;
    }
    old = svc[k];
    s->state = old->state;
    s->pid = old->pid;
    s->stoppid = old->stoppid;
    s->since = old->since;
    s->started = old->started;
    s->again = old->again;
    s->deadline = old->deadline;
    s->restarts = old->restarts;
    s->changes = old->changes;
    s->lastexit = old->lastexit;
    s->quick = old->quick;
    s->killed = old->killed;
    memcpy(s->fails, old->fails, sizeof(s->fails));
    s->nfails = old->nfails;
    strlcpy(s->reason, old->reason, sizeof(s->reason));
    old->gone = 2;
  }
  for (i = 0; i < nsvc; i++) {
    if (svc[i]->gone == 2) {
      free_service(svc[i]);
      continue;
    }
    // no longer enabled: stopped, then forgotten
    svc[i]->gone = 1;
    stop(svc[i]);
    fresh = reallocarray(fresh, (size_t)n + 1, sizeof(*fresh));
    if (fresh == 0)
      err(1, 0);
    fresh[n++] = svc[i];
  }
  free(svc);
  svc = fresh;
  nsvc = n;
  resolve();
}

static void forget_gone(void) {
  int i, k;

  for (i = k = 0; i < nsvc; i++) {
    if (svc[i]->gone == 1 && svc[i]->state == S_STOPPED && svc[i]->pid <= 0 &&
        svc[i]->stoppid <= 0) {
      free_service(svc[i]);
      continue;
    }
    svc[k++] = svc[i];
  }
  if (k != nsvc) {
    nsvc = k;
    resolve();
  }
}

static void command(char *line) {
  static const char *const verbs[] = {"start",  "stop",   "restart", "clear", "run",
                                      "reload", "reread", "multi",   0};
  struct service *s;
  char *arg;
  int v, i;

  if ((arg = strchr(line, ' ')) != 0)
    *arg++ = 0;
  for (v = 0; verbs[v] != 0 && strcmp(verbs[v], line) != 0; v++)
    ;
  if (verbs[v] == 0) {
    warnx("not a command: %s", line);
    return;
  }
  if (v == 6) {
    reread();
    return;
  }
  if (v == 7) {
    target = MULTI;
    return;
  }
  if (arg == 0 || (i = find(svc, nsvc, arg)) < 0) {
    warnx("%s: no service %s", line, arg != 0 ? arg : "named");
    return;
  }
  s = svc[i];
  switch (v) {
  case 0:
    if (s->state == S_MAINT) {
      event(s, 1, "in maintenance: clear it first");
      break;
    }
    if (s->state == S_STOPPED || s->state == S_FAILED || s->state == S_BLOCKED ||
        s->state == S_ABSENT)
      wake(s);
    break;
  case 1:
    stop(s);
    break;
  case 2:
    if (s->state == S_RUNNING || s->state == S_STARTING) {
      stop(s);
      s->again = -1;
    } else if (s->state != S_STOPPING && s->state != S_MAINT)
      wake(s);
    break;
  case 3:
    if (s->state == S_MAINT || s->state == S_FAILED || s->state == S_BLOCKED) {
      s->nfails = 0;
      event(s, 0, "cleared");
      wake(s);
    }
    break;
  case 4:
    if (s->state != S_RUNNING && s->state != S_STARTING && s->state != S_STOPPING)
      wake(s);
    break;
  case 5:
    if (s->pid > 0 && s->state == S_RUNNING)
      kill(s->pid, SIGHUP);
    break;
  }
}

static void read_commands(void) {
  static char *pending;
  static size_t len, cap;
  char buf[512], *nl, *grown;
  ssize_t n;

  while ((n = read(ctl, buf, sizeof(buf))) > 0) {
    if (len + (size_t)n + 1 > cap) {
      cap = len + (size_t)n + 1;
      if ((grown = realloc(pending, cap)) == 0)
        err(1, 0);
      pending = grown;
    }
    memcpy(pending + len, buf, (size_t)n);
    len += (size_t)n;
    pending[len] = 0;
    while ((nl = memchr(pending, '\n', len)) != 0) {
      *nl = 0;
      if (nl > pending && nl[-1] == '\r')
        nl[-1] = 0;
      command(pending);
      len -= (size_t)(nl + 1 - pending);
      memmove(pending, nl + 1, len + 1);
    }
  }
}

// a restart waits for its stop
static void restarts(void) {
  int i;

  for (i = 0; i < nsvc; i++)
    if (svc[i]->again == -1 && svc[i]->state == S_STOPPED)
      wake(svc[i]);
}

// the pipe belongs to the pidfile holder; root read-only, later
static void open_control(void) {
  pid_t other;

  if (ctl >= 0)
    return;
  if (mkdir(RUNDIR, 0755) < 0 && errno != EEXIST)
    return;
  if (pfh == 0) {
    if ((pfh = pidfile_open(PIDFILE, 0644, &other)) == 0) {
      if (errno == EEXIST)
        errx(1, "already running, pid %d", (int)other);
      return;
    }
    pidfile_write(pfh);
  }
  unlink(CONTROL);
  if (mkfifo(CONTROL, 0600) < 0)
    return;
  ctl = open(CONTROL, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

static void on_signal(int sig) {
  if (sig == SIGCHLD)
    got_chld = 1;
  else if (sig == SIGHUP)
    got_hup = 1;
  else
    got_term = 1;
}

static void stage_report(void) {
  static int said;
  int i, failed = 0, n = 0;

  if (said)
    return;
  for (i = 0; i < nsvc; i++) {
    if (svc[i]->stage != BOOT)
      continue;
    if (svc[i]->state == S_WAITING || svc[i]->state == S_STARTING)
      return;
    n++;
    if (svc[i]->state == S_FAILED || svc[i]->state == S_BLOCKED)
      failed++;
  }
  said = 1;
  printf("fnusvc: boot stage: %d steps and services, %d failed%s\n", n, failed,
         target == BOOT ? "; multi waits for init" : "");
  fflush(stdout);
  if (notify >= 0) {
    write(notify, "boot\n", 5);
    close(notify);
    notify = -1;
  }
}

int main(int argc, char **argv __unused) {
  struct pollfd pfd;
  struct timespec wait;
  sigset_t blocked, none;
  time_t next, now;
  int i;

  if (argc > 1) {
    fprintf(stderr, "usage: fnusvc\n"
                    "init(8) starts it; services(8) asks it\n");
    exit(1);
  }
  if (getppid() != 1)
    errx(1, "only init(8) starts the supervisor; services(8) asks it");
  open_control();
  printf("%s service supervisor\n", FNU_PRODUCT_NAME);
  fflush(stdout);
  signal(SIGCHLD, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGCHLD);
  sigaddset(&blocked, SIGTERM);
  sigaddset(&blocked, SIGHUP);
  sigprocmask(SIG_BLOCK, &blocked, 0);
  sigemptyset(&none);

  read_init_conf();
  if (getenv("FNUSVC_NOTIFY") != 0) {
    notify = atoi(getenv("FNUSVC_NOTIFY"));
    fcntl(notify, F_SETFD, FD_CLOEXEC);
    unsetenv("FNUSVC_NOTIFY");
  }
  svc = read_services(&nsvc);
  if (nsvc < 0)
    nsvc = 0;
  if (nsvc == 0)
    warnx("%s named no service to start", RC_CONF);
  for (i = 0; i < nsvc; i++)
    set_state(svc[i], S_WAITING, 0);
  resolve();

  for (;;) {
    if (got_chld) {
      got_chld = 0;
      reap();
    }
    if (got_hup) {
      got_hup = 0;
      reread();
    }
    if (got_term && !shutting) {
      shutting = 1;
      printf("fnusvc: stopping the services\n");
      fflush(stdout);
    }
    open_control();
    if (ctl >= 0 && !shutting)
      read_commands();
    restarts();
    next = advance();
    forget_gone();
    stage_report();
    publish();
    if (shutting && all_stopped()) {
      printf("fnusvc: every service stopped\n");
      fflush(stdout);
      pidfile_remove(pfh);
      exit(0);
    }
    now = time(0);
    wait.tv_sec = next != 0 ? (next > now ? next - now : 0) : 3600;
    wait.tv_nsec = 0;
    pfd.fd = ctl;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ppoll(&pfd, ctl >= 0 ? 1 : 0, &wait, &none);
  }
}
