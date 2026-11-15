#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <fnu/sys.h>
#include <err.h>

#define AMPLITUDE 8000

static int number(const char *text, int fallback) {
  int value = 0;

  if (text == 0 || *text == 0)
    return fallback;
  while (*text) {
    if (*text < '0' || *text > '9')
      return fallback;
    value = value * 10 + *text++ - '0';
  }
  return value;
}

int main(int argc, char **argv) {
  struct fnu_audioinfo ai;
  const char *device = "/dev/audio0";
  int hz, seconds, period, half, step, frames, i, f, ch, phase = 0, fd;
  uint before = 0;
  short block[1024];

  if (argc > 2 && strcmp(argv[1], "-f") == 0) {
    device = argv[2];
    argc -= 2;
    argv += 2;
  }
  hz = number(argc > 1 ? argv[1] : 0, 440);
  seconds = number(argc > 2 ? argv[2] : 0, 2);
  if ((fd = open(device, O_WRONLY)) < 0)
    err(1, "%s", device);
  if (ioctl(fd, FNU_AUDIOGETINFO, (uint64_t)(uintptr_t)&ai) < 0 || !ai.present) {
    dprintf(2, "tone: no audio device\n");
    exit(1);
  }
  if (ai.width != 2) {
    dprintf(2, "tone: %d bit samples are not what this writes\n", ai.width * 8);
    exit(1);
  }
  if (hz <= 0 || seconds <= 0 || (int)ai.rate / hz < 4) {
    dprintf(2, "usage: tone [-f device] [HZ [SECONDS]]\n");
    exit(1);
  }
  period = (int)ai.rate / hz;
  half = period / 2;
  step = AMPLITUDE * 2 / half;

  printf("tone: %d Hz for %d seconds into %d Hz, %d channels, %d bit\n", hz,
         seconds, ai.rate, ai.channels, ai.width * 8);
  before = ai.starved;
  frames = (int)(sizeof(block) / sizeof(block[0])) / (int)ai.channels;
  for (i = 0; i < (int)ai.rate * seconds; i += frames) {
    int n = 0;

    for (f = 0; f < frames; f++) {
      int v = phase < half ? -AMPLITUDE + step * phase
                           : AMPLITUDE - step * (phase - half);
      for (ch = 0; ch < (int)ai.channels; ch++)
        block[n++] = (short)v;
      if (++phase >= period)
        phase = 0;
    }
    if (write(fd, block, n * 2) != n * 2) {
      warn("write");
      break;
    }
  }
  ioctl(fd, FNU_AUDIOGETINFO, (uint64_t)(uintptr_t)&ai);
  if (ai.rate >= 1000)
    printf("tone: %d of %d intervals went out as silence (starved %u before, "
           "%u after)\n",
           (int)(ai.starved - before),
           (int)ai.rate * seconds / (int)(ai.rate / 1000), before, ai.starved);
  else
    printf("tone: %d intervals went out as silence (starved %u before, "
           "%u after)\n",
           (int)(ai.starved - before), before, ai.starved);
  close(fd);
  exit(0);
}
