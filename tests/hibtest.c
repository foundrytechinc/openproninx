#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

#define PAGE 4096

// hibtest [-r] [pages] &, sysctl hw.acpi.sleep=4, kill -USR1 after the wake
// -r: noise that does not compress
// not in run: needs a hibernation in the middle

static volatile sig_atomic_t told;

static void tell(int sig) {
  (void)sig;
  told = 1;
}

static long long quad(const char *name) {
  long long v = 0;
  size_t len = sizeof(v);

  if (sysctlbyname(name, &v, &len, NULL, 0) < 0)
    return -1;
  return v;
}

static unsigned mark(long i) { return 0x48494200u ^ (unsigned)(i * 1000003); }

// xorshift32, seeded by the page
static void noise(unsigned *w, long i) {
  unsigned x = mark(i) | 1;
  int k;

  for (k = 0; k < PAGE / 4; k++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w[k] = x;
  }
}

static int intact(const unsigned *w, long i, int full) {
  unsigned want[PAGE / 4];

  if (!full)
    return w[0] == mark(i) && w[PAGE / 4 - 1] == ~mark(i);
  noise(want, i);
  return memcmp(w, want, PAGE) == 0;
}

int main(int argc, char **argv) {
  long long freemem = quad("vm.freemem");
  long pages, i, bad = 0;
  int full = argc > 1 && strcmp(argv[1], "-r") == 0;
  sigset_t mask, old;
  char *mem;

  if (freemem <= 0) {
    printf("hibtest: the vm counters cannot be read\n");
    exit(1);
  }
  argc -= full;
  argv += full;
  pages = argc > 1 ? atol(argv[1]) : (long)(freemem / PAGE * 3 / 4);
  if ((mem = malloc((size_t)pages * PAGE)) == 0) {
    printf("hibtest: no memory for %ld pages\n", pages);
    exit(1);
  }
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigprocmask(SIG_BLOCK, &mask, &old);
  signal(SIGUSR1, tell);
  for (i = 0; i < pages; i++) {
    unsigned *w = (unsigned *)(mem + (size_t)i * PAGE);
    if (full)
      noise(w, i);
    else {
      w[0] = mark(i);
      w[PAGE / 4 - 1] = ~mark(i);
    }
  }
  printf("hibtest: %ld pages held, pid %d\n", pages, (int)getpid());
  fflush(stdout);
  while (!told)
    sigsuspend(&old);
  for (i = 0; i < pages; i++)
    if (!intact((unsigned *)(mem + (size_t)i * PAGE), i, full) && bad++ < 5)
      printf("hibtest: page %ld came back changed\n", i);
  printf("hibtest: %ld pages checked: %s\n", pages, bad ? "FAILED" : "ok");
  exit(bad != 0);
}
