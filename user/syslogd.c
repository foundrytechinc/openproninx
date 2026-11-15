// syslogd: the log stream into files, terminals and other hosts
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fnu/sys.h>
#include <err.h>
#include <util.h>

#define KLOG "/dev/klog"
#define CONF "/etc/syslog.conf"
#define NFAC 24
#define LINE 1024
#define ROLL (1024 * 1024)
#define SYSLOG_PORT 514
#define KERN_DEFAULT ((0 << 3) | 5)

enum { A_FILE, A_USERS, A_FORWARD };

struct rule {
  unsigned char mask[NFAC];
  int kind;
  char path[256];
  int fd;
  struct sockaddr_in to;
  struct rule *next;
};

static const struct name {
  const char *name;
  int value;
} facilities[] = {{"kern", 0},     {"user", 1},     {"mail", 2},     {"daemon", 3},
                  {"auth", 4},     {"syslog", 5},   {"lpr", 6},      {"news", 7},
                  {"uucp", 8},     {"cron", 9},     {"authpriv", 10}, {"ftp", 11},
                  {"local0", 16},  {"local1", 17},  {"local2", 18},  {"local3", 19},
                  {"local4", 20},  {"local5", 21},  {"local6", 22},  {"local7", 23},
                  {0, 0}},
  severities[] = {{"emerg", 0},   {"alert", 1}, {"crit", 2},  {"err", 3},
                  {"warning", 4}, {"notice", 5}, {"info", 6}, {"debug", 7},
                  {"panic", 0},   {"error", 3}, {"warn", 4},  {0, 0}};

static struct rule *rules;
static char host[256];
static int sock = -1;
static volatile sig_atomic_t got_hup, got_term;

static int lookup(const struct name *t, const char *s) {
  int i;

  for (i = 0; t[i].name != 0; i++)
    if (strcmp(t[i].name, s) == 0)
      return t[i].value;
  return -1;
}

// fac[,fac].pri; =pri exactly; none; *
static int selector(char *sel, struct rule *r) {
  char *dot = strrchr(sel, '.'), *fac, *next;
  unsigned char bits;
  int exact = 0, sev, f;

  if (dot == 0)
    return -1;
  *dot++ = 0;
  if (*dot == '=') {
    exact = 1;
    dot++;
  }
  if (strcmp(dot, "*") == 0)
    bits = 0xff;
  else if (strcmp(dot, "none") == 0)
    bits = 0;
  else if ((sev = lookup(severities, dot)) < 0)
    return -1;
  else
    bits = exact ? (unsigned char)(1u << sev) : (unsigned char)((2u << sev) - 1);
  for (fac = sel; fac != 0 && *fac != 0; fac = next) {
    if ((next = strchr(fac, ',')) != 0)
      *next++ = 0;
    if (strcmp(fac, "*") == 0) {
      for (f = 0; f < NFAC; f++)
        r->mask[f] = bits;
      continue;
    }
    if ((f = lookup(facilities, fac)) < 0)
      return -1;
    r->mask[f] = bits;
  }
  return 0;
}

static int action(char *what, struct rule *r) {
  uint8 addr[4];
  int e;

  if (strcmp(what, "*") == 0) {
    r->kind = A_USERS;
    return 0;
  }
  if (what[0] == '@') {
    if ((e = fnu_resolve(what + 1, addr, 5000)) < 0) {
      warnx("%s: %s", what + 1, fnu_errstr(e));
      return -1;
    }
    r->kind = A_FORWARD;
    memset(&r->to, 0, sizeof(r->to));
    r->to.sin_family = AF_INET;
    r->to.sin_port = htons(SYSLOG_PORT);
    memcpy(&r->to.sin_addr, addr, 4);
    if (sock < 0)
      sock = socket(AF_INET, SOCK_DGRAM, 0);
    return 0;
  }
  if (what[0] != '/')
    return -1;
  r->kind = A_FILE;
  strlcpy(r->path, what, sizeof(r->path));
  r->fd = open(r->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
  if (r->fd < 0)
    warn("%s", r->path);
  return 0;
}

static void load(void) {
  char line[LINE], *p, *sel, *next, *what;
  struct rule *r, **tail = &rules;
  int n = 0;
  FILE *fp;

  while ((r = rules) != 0) {
    rules = r->next;
    if (r->kind == A_FILE && r->fd >= 0)
      close(r->fd);
    free(r);
  }
  if ((fp = fopen(CONF, "r")) == 0) {
    warn("%s", CONF);
    return;
  }
  while (fgets(line, sizeof(line), fp) != 0) {
    n++;
    line[strcspn(line, "\r\n")] = 0;
    for (p = line; *p == ' ' || *p == '\t'; p++)
      ;
    if (*p == '#' || *p == 0)
      continue;
    sel = p;
    while (*p != 0 && *p != ' ' && *p != '\t')
      p++;
    if (*p == 0) {
      warnx("%s:%d: no action", CONF, n);
      continue;
    }
    *p++ = 0;
    while (*p == ' ' || *p == '\t')
      p++;
    what = p;
    if ((r = calloc(1, sizeof(*r))) == 0)
      err(1, 0);
    r->fd = -1;
    for (; sel != 0 && *sel != 0; sel = next) {
      if ((next = strchr(sel, ';')) != 0)
        *next++ = 0;
      if (selector(sel, r) < 0) {
        warnx("%s:%d: %s is not a selector", CONF, n, sel);
        free(r);
        r = 0;
        break;
      }
    }
    if (r == 0)
      continue;
    if (action(what, r) < 0) {
      warnx("%s:%d: %s is not an action", CONF, n, what);
      free(r);
      continue;
    }
    *tail = r;
    tail = &r->next;
  }
  fclose(fp);
  gethostname(host, sizeof(host));
  if (host[0] == 0)
    strlcpy(host, "localhost", sizeof(host));
}

// one older generation kept
static void roll(struct rule *r) {
  char old[300];
  struct stat st;

  if (fstat(r->fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < ROLL)
    return;
  snprintf(old, sizeof(old), "%s.0", r->path);
  rename(r->path, old);
  close(r->fd);
  r->fd = open(r->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
}

static void to_users(const char *line) {
  char path[300], buf[LINE + 8];
  struct dirent *de;
  DIR *d;
  int fd, n;

  n = snprintf(buf, sizeof(buf), "\r\n%s\r\n", line);
  if ((d = opendir("/dev")) == 0)
    return;
  while ((de = readdir(d)) != 0) {
    if (strncmp(de->d_name, "tty", 3) != 0 || strcmp(de->d_name, "tty") == 0)
      continue;
    snprintf(path, sizeof(path), "/dev/%s", de->d_name);
    if ((fd = open(path, O_WRONLY | O_NONBLOCK)) < 0)
      continue;
    write(fd, buf, (size_t)n);
    close(fd);
  }
  closedir(d);
}

static void dispatch(int pri, const char *msg) {
  char stamp[32], line[LINE + 300], wire[LINE + 16];
  time_t now = time(0);
  int fac = (pri >> 3) & 0x1f, sev = pri & 7, n;
  struct rule *r;

  if (fac >= NFAC)
    fac = 1;
  strftime(stamp, sizeof(stamp), "%b %e %H:%M:%S", localtime(&now));
  n = snprintf(line, sizeof(line), "%s %s %s", stamp, host, msg);
  if (n >= (int)sizeof(line))
    n = sizeof(line) - 1;
  for (r = rules; r != 0; r = r->next) {
    if (!(r->mask[fac] & (1u << sev)))
      continue;
    switch (r->kind) {
    case A_FILE:
      if (r->fd < 0)
        r->fd = open(r->path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
      if (r->fd >= 0) {
        line[n] = '\n';
        write(r->fd, line, (size_t)n + 1);
        line[n] = 0;
        roll(r);
      }
      break;
    case A_USERS:
      to_users(line);
      break;
    case A_FORWARD:
      if (sock >= 0) {
        int w = snprintf(wire, sizeof(wire), "<%d>%s", pri, msg);

        sendto(sock, wire, (size_t)(w < (int)sizeof(wire) ? w : (int)sizeof(wire) - 1), 0,
               (struct sockaddr *)&r->to, sizeof(r->to));
      }
      break;
    }
  }
}

// same line as before: counted, not written. спам, нахуй
#define REPEAT_MS 30000
static char prev[LINE + 16];
static int prevpri = -1, repeats;

static void flush_repeats(void) {
  char m[64];

  if (repeats == 0)
    return;
  snprintf(m, sizeof(m), "last message repeated %d time%s", repeats,
           repeats == 1 ? "" : "s");
  repeats = 0;
  dispatch(prevpri, m);
}

static void deliver(int pri, const char *msg) {
  if (pri == prevpri && strcmp(msg, prev) == 0) {
    repeats++;
    return;
  }
  flush_repeats();
  strlcpy(prev, msg, sizeof(prev));
  prevpri = pri;
  dispatch(pri, msg);
}

// <pri>text; the kernel's own lines carry none
static void one_line(char *s) {
  char msg[LINE + 16];
  int pri = -1;
  char *end;

  if (*s == 0)
    return;
  if (s[0] == '<') {
    long p = strtol(s + 1, &end, 10);

    if (*end == '>' && end > s + 1 && p >= 0 && p < 256) {
      pri = (int)p;
      s = end + 1;
    }
  }
  if (pri < 0) {
    snprintf(msg, sizeof(msg), "kernel: %s", s);
    deliver(KERN_DEFAULT, msg);
  } else
    deliver(pri, s);
}

static void on_signal(int sig) {
  if (sig == SIGHUP)
    got_hup = 1;
  else
    got_term = 1;
}

int main(int argc, char **argv __unused) {
  struct sigaction sa;
  struct pollfd pf;
  struct timespec repeat = {REPEAT_MS / 1000, 0};
  sigset_t blocked, open_mask;
  char buf[4096], pending[LINE + 1], *p, *nl;
  size_t have = 0;
  ssize_t n;
  struct pidfh *pfh;
  pid_t other;
  int fd;

  if (argc != 1) {
    dprintf(2, "usage: syslogd\n");
    return 1;
  }
  if ((pfh = pidfile_open(0, 0600, &other)) == 0) {
    if (errno == EEXIST)
      errx(1, "already running, pid %d", (int)other);
    warn("cannot open or create pidfile");
  }
  if ((fd = open(KLOG, O_RDONLY | O_CLOEXEC)) < 0)
    err(1, "%s", KLOG);
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGHUP, &sa, 0);
  sigaction(SIGTERM, &sa, 0);
  // signals land only inside the wait
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGHUP);
  sigaddset(&blocked, SIGTERM);
  sigprocmask(SIG_BLOCK, &blocked, &open_mask);
  load();
  pidfile_write(pfh);
  for (;;) {
    if (got_term)
      break;
    if (got_hup) {
      got_hup = 0;
      load();
    }
    pf.fd = fd;
    pf.events = POLLIN;
    if ((n = ppoll(&pf, 1, repeats != 0 ? &repeat : 0, &open_mask)) <= 0) {
      if (n == 0)
        flush_repeats();
      else if (errno != EINTR)
        err(1, "%s", KLOG);
      continue;
    }
    if ((n = read(fd, buf, sizeof(buf))) < 0) {
      if (errno == EINTR)
        continue;
      err(1, "%s", KLOG);
    }
    for (p = buf; p < buf + n;) {
      if ((nl = memchr(p, '\n', (size_t)(buf + n - p))) == 0) {
        size_t k = (size_t)(buf + n - p);

        if (have + k > LINE)
          k = LINE - have;
        memcpy(pending + have, p, k);
        have += k;
        break;
      }
      if (have + (size_t)(nl - p) > LINE)
        nl = p + (LINE - have);
      memcpy(pending + have, p, (size_t)(nl - p));
      have += (size_t)(nl - p);
      pending[have] = 0;
      one_line(pending);
      have = 0;
      p = nl + 1;
    }
  }
  pidfile_remove(pfh);
  return 0;
}
