#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#define PAGE 4096

static long long quad(const char *name) {
  long long v = 0;
  size_t len = sizeof(v);

  if (sysctlbyname(name, &v, &len, NULL, 0) < 0)
    return -1;
  return v;
}

static unsigned mark(int id, long i) { return 0x51000000u ^ (unsigned)(id * 1000003 + i); }

static int filled[2], go[2];

// every page carries its mark at both ends; all hold theirs before any checks
static int fill_and_check(int id, long pages, int rounds) {
  char *mem = malloc((size_t)pages * PAGE);
  long i;
  int r;

  if (mem == 0) {
    printf("swaptest %d: no memory for %ld pages\n", id, pages);
    return 1;
  }
  for (i = 0; i < pages; i++) {
    unsigned *w = (unsigned *)(mem + (size_t)i * PAGE);
    w[0] = mark(id, i);
    w[PAGE / 4 - 1] = ~mark(id, i);
  }
  close(filled[0]);
  close(go[1]);
  if (write(filled[1], "x", 1) != 1 || read(go[0], &r, 1) != 0)
    return 1;
  for (r = 0; r < rounds; r++) {
    for (i = 0; i < pages; i++) {
      unsigned *w = (unsigned *)(mem + (size_t)i * PAGE);
      if (w[0] != mark(id, i) || w[PAGE / 4 - 1] != ~mark(id, i)) {
        printf("swaptest %d: page %ld came back as 0x%x/0x%x\n", id, i, w[0],
               w[PAGE / 4 - 1]);
        return 1;
      }
      w[1] = (unsigned)r;
    }
  }
  return 0;
}

// more than free memory: pages must go out and come back
int main(int argc, char **argv) {
  long long freemem = quad("vm.freemem"), total = quad("vm.swap.total");
  long long used = quad("vm.swap.used"), outs = quad("vm.swap.outs");
  long long ins = quad("vm.swap.ins");
  long long want, excess, after;
  int ncpu = (int)quad("hw.ncpu");
  int workers, rounds = argc > 3 ? atoi(argv[3]) : 2;
  long pages;
  int e, i, pid, bad = 0;

  if (freemem <= 0 || total < 0 || used < 0 || outs < 0 || ins < 0) {
    printf("swaptest: the vm counters cannot be read\n");
    exit(1);
  }
  if (total == 0) {
    printf("swaptest: no swap configured, nothing to test\n");
    exit(0);
  }
  workers = argc > 1 ? atoi(argv[1]) : ncpu < 1 ? 1 : ncpu > 8 ? 8 : ncpu;
  // past the free memory by half the free swap
  excess = (total - used) / 2;
  want = freemem + excess;
  pages = argc > 2 ? atol(argv[2]) : (long)(want / PAGE / workers);

  if (pipe(filled) < 0 || pipe(go) < 0) {
    printf("swaptest: no pipe\n");
    exit(1);
  }
  for (i = 0; i < workers; i++)
    if ((pid = fork()) == 0)
      exit(fill_and_check(i + 1, pages, rounds));
  close(filled[1]);
  close(go[0]);
  for (i = 0; i < workers && read(filled[0], &e, 1) == 1; i++)
    ;
  close(go[1]);
  for (i = 0; i < workers; i++) {
    wait(&e);
    if (!WIFEXITED(e) || WEXITSTATUS(e) != 0)
      bad++;
  }
  after = quad("vm.swap.outs");
  printf("swaptest: %d workers x %ld pages x %d rounds, %lld pages out, %lld "
         "in: ",
         workers, pages, rounds, after - outs, quad("vm.swap.ins") - ins);
  // the excess, at least half of it, has to have gone out
  if (!bad && argc <= 2 && (after - outs) * PAGE < excess / 2) {
    printf("FAILED, too little was paged out\n");
    exit(1);
  }
  printf("%s\n", bad ? "FAILED" : "ok");
  exit(bad != 0);
}
