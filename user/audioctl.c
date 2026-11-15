#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fnu/sys.h>
#include <err.h>

#define DEVICE "/dev/audio0"

struct var {
  const char *name;
  uint32 value;
  int present;
  uint32 *set;
};

static struct var vars[24];
static struct fnu_audio_swpar par;
static int nvars, nameless, quiet, fd = -1;

static void add(const char *name, uint32 value, int present, uint32 *set) {
  if (nvars < (int)(sizeof(vars) / sizeof(vars[0]))) {
    vars[nvars].name = name;
    vars[nvars].value = value;
    vars[nvars].present = present;
    vars[nvars].set = set;
    nvars++;
  }
}

static void show(const struct var *v) {
  if (nameless)
    printf("%u\n", v->value);
  else
    printf("%s=%u\n", v->name, v->value);
}

static void usage(void) {
  dprintf(2, "usage: audioctl [-anq] [-f device] [-w wait] [name[=value] ...]\n");
  exit(1);
}

static void load(void) {
  struct fnu_audioinfo ai;

  nvars = 0;
  if (ioctl(fd, FNU_AUDIOGETINFO, (uint64_t)(uintptr_t)&ai) < 0 || !ai.present) {
    dprintf(2, "audioctl: no audio device\n");
    exit(1);
  }
  add("play.rate", ai.rate, 1, &par.rate);
  add("play.channels", ai.channels, 1, 0);
  add("play.bits", ai.width * 8, 1, 0);
  add("play.interval", ai.interval, 1, 0);
  add("play.queued", ai.queued, 1, 0);
  add("play.capacity", ai.capacity, 1, 0);
  add("play.starved", ai.starved, 1, 0);
  add("play.intervals", ai.intervals, 1, 0);
  add("record.rate", ai.rec_rate, ai.rec_present, &par.rate);
  add("record.channels", ai.rec_channels, ai.rec_present, 0);
  add("record.bits", ai.rec_width * 8, ai.rec_present, 0);
  add("record.queued", ai.rec_queued, ai.rec_present, 0);
  add("record.overruns", ai.rec_overruns, ai.rec_present, 0);
  add("record.intervals", ai.rec_intervals, ai.rec_present, 0);
}

static struct var *lookup(const char *name) {
  int k;

  for (k = 0; k < nvars; k++)
    if (vars[k].present && strcmp(vars[k].name, name) == 0)
      return &vars[k];
  return 0;
}

int main(int argc, char **argv) {
  const char *device = DEVICE;
  int i = 1, all = 0, k, wait = 0, set = 0, status = 0;

  for (; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
    const char *p = argv[i] + 1;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (; *p != 0; p++) {
      if (*p == 'a')
        all = 1;
      else if (*p == 'n')
        nameless = 1;
      else if (*p == 'q')
        quiet = 1;
      else if (*p == 'f' || *p == 'w') {
        const char *v = p[1] != 0 ? p + 1 : (i + 1 < argc ? argv[++i] : 0);

        if (v == 0)
          usage();
        if (*p == 'f')
          device = v;
        else if ((wait = atoi(v)) < 1) {
          dprintf(2, "audioctl: wait is %s: %s\n",
                  wait < 0 ? "too small" : "invalid", v);
          exit(1);
        }
        p += strlen(p) - 1;
      } else
        usage();
    }
  }
  FNU_AUDIO_INITPAR(&par);
  if ((fd = open(device, O_RDONLY)) < 0)
    err(1, "%s", device);
  load();

  for (k = i; k < argc; k++) {
    char *eq = strchr(argv[k], '=');
    struct var *v;

    if (eq != 0)
      *eq = 0;
    if ((v = lookup(argv[k])) == 0) {
      dprintf(2, "audioctl: %s: no such variable\n", argv[k]);
      exit(1);
    }
    if (eq == 0)
      continue;
    if (v->set == 0) {
      dprintf(2, "audioctl: %s: can't be set\n", argv[k]);
      exit(1);
    }
    if (atoi(eq + 1) <= 0) {
      dprintf(2, "audioctl: %s: bad value: %s\n", argv[k], eq + 1);
      exit(1);
    }
    *v->set = (uint32)atoi(eq + 1);
    *eq = '=';
    set++;
  }

  for (;;) {
    if (i >= argc || all) {
      for (k = 0; k < nvars; k++)
        if (vars[k].present)
          show(&vars[k]);
    } else
      for (k = i; k < argc; k++)
        if (strchr(argv[k], '=') == 0)
          show(lookup(argv[k]));
    if (wait == 0)
      break;
    sleep(wait);
    load();
  }
  if (set == 0)
    exit(0);

  if (ioctl(fd, FNU_AUDIOSETPAR, (uint64_t)(uintptr_t)&par) < 0) {
    dprintf(2, "audioctl: the device refused the rate; it is not one it "
               "lists, or the stream is running\n");
    status = 1;
  }
  close(fd);
  if (status == 0 && !quiet) {
    for (k = 0; k < nvars; k++) {
      if (vars[k].set == 0 || *vars[k].set == ~0u)
        continue;
      if (nameless)
        printf("%u\n", *vars[k].set);
      else
        printf("%s: %u -> %u\n", vars[k].name, vars[k].value, *vars[k].set);
    }
  }
  exit(status);
}
