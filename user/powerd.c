// POWERD
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/sysctl.h>
#include <err.h>
#include <util.h>

#define MAXLEVELS 32
#define DEFAULT_ACTIVE 75 // percent: busy
#define DEFAULT_IDLE 50   // percent: idle
#define DEFAULT_POLL 250  // ms

enum { MODE_MIN, MODE_ADAPTIVE, MODE_HIADAPTIVE, MODE_MAX };
enum { SRC_AC, SRC_BATTERY, SRC_UNKNOWN };

static const char *const source_name[] = {"AC", "battery", "unknown"};

static int levels[MAXLEVELS], nlevels;
static int nominal;
static volatile sig_atomic_t done;
static int verbose;

static void on_signal(int sig) {
  (void)sig;
  done = 1;
}

static int mode_of(const char *s) {
  if (strcmp(s, "min") == 0 || strcmp(s, "minimum") == 0)
    return MODE_MIN;
  if (strcmp(s, "max") == 0 || strcmp(s, "maximum") == 0)
    return MODE_MAX;
  if (strcmp(s, "adp") == 0 || strcmp(s, "adaptive") == 0)
    return MODE_ADAPTIVE;
  if (strcmp(s, "hadp") == 0 || strcmp(s, "hiadaptive") == 0)
    return MODE_HIADAPTIVE;
  errx(1, "bad mode -- %s", s);
}

static int int_sysctl(const char *name, int *v) {
  size_t len = sizeof(*v);

  return sysctlbyname(name, v, &len, 0, 0);
}

// MHz, or nominal: the fastest that is not turbo
static int freq_of(const char *s) {
  char *end;
  long v;

  if (strcmp(s, "nominal") == 0)
    return nominal;
  v = strtol(s, &end, 10);
  if (*end != 0 || v <= 0)
    errx(1, "bad frequency -- %s", s);
  return (int)v;
}

static void read_levels(void) {
  char buf[256], *p, *end;
  size_t len = sizeof(buf) - 1;

  if (sysctlbyname("hw.cpu.levels", buf, &len, 0, 0) < 0) {
    printf("powerd: no processor performance states here\n");
    exit(0);
  }
  buf[len] = 0;
  for (p = buf; nlevels < MAXLEVELS;) {
    long v = strtol(p, &end, 10);

    if (end == p)
      break;
    levels[nlevels++] = (int)v;
    p = end;
  }
  if (nlevels < 2) {
    printf("powerd: one frequency only, nothing to do\n");
    exit(0);
  }
  if (int_sysctl("hw.cpu.nominal", &nominal) < 0)
    nominal = levels[0];
}

// the index of the fastest level at or under freq
static int level_of(int freq) {
  int i;

  for (i = 0; i < nlevels - 1 && levels[i] > freq; i++)
    ;
  return i;
}

static int source(void) {
  int ac;

  if (int_sysctl("hw.acpi.acline", &ac) < 0 || ac < 0)
    return SRC_UNKNOWN;
  return ac ? SRC_AC : SRC_BATTERY;
}

// the busiest cpus' load summed, in percent of one cpu
static int read_load(void) {
  static uint64_t *now, *was;
  static size_t len;
  size_t got;
  int load = 0, c;

  if (now == 0) {
    if (sysctlbyname("kern.cp_times", 0, &len, 0, 0) < 0 || len == 0)
      err(1, "kern.cp_times");
    if ((now = calloc(1, len)) == 0 || (was = calloc(1, len)) == 0)
      err(1, "kern.cp_times");
  }
  got = len;
  if (sysctlbyname("kern.cp_times", now, &got, 0, 0) < 0)
    return -1;
  for (c = 0; c < (int)(got / (2 * sizeof(uint64_t))); c++) {
    uint64_t busy = now[2 * c] - was[2 * c], idle = now[2 * c + 1] - was[2 * c + 1];

    if (busy + idle != 0)
      load += (int)(busy * 100 / (busy + idle));
  }
  memcpy(was, now, got);
  return load;
}

static int set_freq(int freq) {
  if (sysctlbyname("hw.cpu.freq", 0, 0, &freq, sizeof(freq)) < 0) {
    warn("hw.cpu.freq %d", freq);
    return -1;
  }
  return 0;
}

static void usage(void) {
  fprintf(stderr, "usage: powerd [-v] [-a mode] [-b mode] [-n mode] [-i %%] "
                  "[-r %%] [-m freq] [-M freq] [-A freq] [-B freq] [-p ival] "
                  "[-P pidfile]\n");
  exit(1);
}

int main(int argc, char **argv) {
  int mode_src[3] = {MODE_HIADAPTIVE, MODE_ADAPTIVE, MODE_HIADAPTIVE};
  const char *cap_arg[3] = {0, 0, 0}, *min_arg = 0, *max_arg = 0;
  int cap[3], active = DEFAULT_ACTIVE, idle_mark = DEFAULT_IDLE, ival = DEFAULT_POLL;
  int lo, hi, src, last_src = -1, mode, load, freq, cur, want, quiet = 0, ch;
  struct sigaction sa;
  struct timespec ts;
  struct pidfh *pfh;
  const char *pidfile = 0;
  pid_t other;

  while ((ch = getopt(argc, argv, "a:b:n:i:r:m:M:A:B:p:P:v")) != -1) {
    switch (ch) {
    case 'a':
      mode_src[SRC_AC] = mode_of(optarg);
      break;
    case 'b':
      mode_src[SRC_BATTERY] = mode_of(optarg);
      break;
    case 'n':
      mode_src[SRC_UNKNOWN] = mode_of(optarg);
      break;
    case 'i':
      idle_mark = atoi(optarg);
      break;
    case 'r':
      active = atoi(optarg);
      break;
    case 'm':
      min_arg = optarg;
      break;
    case 'M':
      max_arg = optarg;
      break;
    case 'A':
      cap_arg[SRC_AC] = optarg;
      break;
    case 'B':
      cap_arg[SRC_BATTERY] = optarg;
      break;
    case 'p':
      ival = atoi(optarg);
      break;
    case 'P':
      pidfile = optarg;
      break;
    case 'v':
      verbose = 1;
      break;
    default:
      usage();
    }
  }
  if (optind != argc || idle_mark < 0 || idle_mark > 100 || active < 0 ||
      active > 100 || ival < 10)
    usage();

  read_levels();
  lo = min_arg ? freq_of(min_arg) : levels[nlevels - 1];
  hi = max_arg ? freq_of(max_arg) : levels[0];
  for (src = 0; src < 3; src++)
    cap[src] = cap_arg[src] ? freq_of(cap_arg[src]) : hi;
  if ((pfh = pidfile_open(pidfile, 0600, &other)) == 0) {
    if (errno == EEXIST)
      errx(1, "already running, pid %d", (int)other);
    warn("cannot open pid file");
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGINT, &sa, 0);

  pidfile_write(pfh);
  freq = cur = levels[0];
  read_load();
  while (!done) {
    int top, bottom;

    ts.tv_sec = ival / 1000;
    ts.tv_nsec = (long)(ival % 1000) * 1000000;
    nanosleep(&ts, 0);
    if (done)
      break;

    src = source();
    mode = mode_src[src];
    top = levels[level_of(cap[src] < hi ? cap[src] : hi)];
    bottom = lo < top ? lo : top;
    if (src != last_src && verbose)
      printf("on %s power, %s, %d..%d MHz\n", source_name[src],
             mode == MODE_MIN ? "minimum" : mode == MODE_MAX ? "maximum"
             : mode == MODE_ADAPTIVE ? "adaptive" : "hiadaptive",
             bottom, top);
    if ((load = read_load()) < 0)
      continue;

    // up fast when busy, down slowly when idle
    if (mode == MODE_MIN)
      freq = bottom;
    else if (mode == MODE_MAX)
      freq = top;
    else if (mode == MODE_ADAPTIVE) {
      if (load > active)
        freq = load > 95 || load > active * 2 ? freq * 2 : freq * load / active;
      else if (load < idle_mark &&
               cur * load < levels[level_of(freq * 7 / 8)] * active)
        freq = freq * 7 / 8;
    } else {
      if (load > active / 2)
        freq = load > 95 || load > active ? freq * 4 : freq * load * 2 / active;
      else if (load < idle_mark / 2 &&
               cur * load < levels[level_of(freq * 31 / 32)] * active / 2)
        freq = freq * 31 / 32;
    }
    if (freq > top)
      freq = top;
    if (freq < bottom)
      freq = bottom;

    want = levels[level_of(freq)];
    if (want != cur || src != last_src) {
      if (verbose)
        printf("load %3d%%, %d MHz -> %d MHz\n", load, cur, want);
      if (set_freq(want) == 0)
        cur = want;
      else if (++quiet > 10)
        errx(1, "the kernel will not take a frequency");
    }
    last_src = src;
  }
  pidfile_remove(pfh);
  return 0;
}
