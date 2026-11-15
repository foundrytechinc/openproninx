
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static volatile int fired;

static void ok(const char *what, int good) {
  printf("alarmtest: %-40s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static void on_alarm(int sig __unused) { fired++; }

int main(void) {
  struct sigaction act;
  sigset_t set, pending;
  unsigned int left;
  int i, sig;

  memset(&act, 0, sizeof(act));
  act.sa_handler = on_alarm;
  sigaction(SIGALRM, &act, 0);
  alarm(1);
  for (i = 0; i < 30 && !fired; i++)
    usleep(100000);
  ok("a handler runs when it fires", fired == 1);

  alarm(5);
  left = alarm(0);
  ok("alarm(0) says what was left", left >= 4 && left <= 5);
  sleep(1);
  ok("and nothing fires after it", fired == 1);

  sigemptyset(&set);
  sigaddset(&set, SIGALRM);
  sigprocmask(SIG_BLOCK, &set, 0);
  alarm(1);
  sleep(2);
  sigpending(&pending);
  ok("a blocked one waits pending", sigismember(&pending, SIGALRM) == 1);
  ok("and its handler does not run", fired == 1);
  ok("sigwait takes it", sigwait(&set, &sig) == 0 && sig == SIGALRM);
  sigpending(&pending);
  ok("and it is pending no more", sigismember(&pending, SIGALRM) == 0);

  printf("alarmtest: %s\n", failures == 0 ? "all ok" : "FAILURES");
  exit(failures == 0 ? 0 : 1);
}
