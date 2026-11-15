#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <fnu/sys.h>

static volatile double in_handler;

static void on_alarm(int sig, const struct fnu_siginfo *si, struct fnu_sigcontext *sc) {
  double x = 0;
  int i;

  (void)sig;
  (void)si;
  (void)sc;
  for (i = 1; i < 1000; i++)
    x += 1.0 / i;
  in_handler = x;
}

static int worker(int id) {
  double sum = 0, expect;
  int i, round;

  for (round = 0; round < 200; round++) {
    sum = 0;
    for (i = 1; i <= 1000; i++)
      sum += (double)id * i;
    expect = (double)id * 500500.0;
    if (sum != expect) {
      printf("fputest: worker %d got %ld, wanted %ld\n", id, (long)sum,
             (long)expect);
      return 1;
    }
  }
  return 0;
}

int main(void) {
  struct sigaction act;
  int e;
  double held, half;
  int i, pid, bad = 0, n = 0;

  for (i = 1; i <= 4; i++) {
    if ((pid = fork()) == 0)
      exit(worker(i));
    if (pid > 0)
      n++;
  }
  while (n-- > 0) {
    wait(&e);
    if (!WIFEXITED(e) || WEXITSTATUS(e) != 0)
      bad++;
  }

  memset(&act, 0, sizeof(act));
  act.sa_sigaction = (void (*)(int, siginfo_t *, void *))on_alarm;
  act.sa_flags = SA_SIGINFO;
  sigaction(SIGALRM, &act, 0);
  held = 3.0;
  for (i = 0; i < 20; i++)
    held = held * 1.5 - 0.25;
  half = held / 2;
  alarm(1);
  for (i = 0; i < 300; i++)
    usleep(10000);
  if (in_handler == 0)
    printf("fputest: the handler never ran\n"), bad++;
  if (half * 2 != held) {
    printf("fputest: value changed under the handler\n");
    bad++;
  }
  printf("fputest: %s (harmonic %ld/1000, held %ld)\n", bad ? "FAILED" : "ok",
         (long)(in_handler * 1000), (long)held);
  exit(bad != 0);
}
