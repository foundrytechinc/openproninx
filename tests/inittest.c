// inittest: an orphan is taken by init and reaped, alive or already a zombie
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static struct timespec nap = {0, 50000000};

// the middle child forks the orphan and bails
// the orphan dies after it, or before (zombie)
static int orphan_reaped(int dies_first) {
  pid_t middle, orphan = 0;
  int fd[2], i, gone = 0;

  if (pipe(fd) < 0)
    return 0;
  if ((middle = fork()) == 0) {
    if ((orphan = fork()) == 0) {
      if (!dies_first)
        nanosleep(&nap, 0);
      _exit(0);
    }
    if (dies_first)
      nanosleep(&nap, 0);
    write(fd[1], &orphan, sizeof(orphan));
    _exit(0);
  }
  close(fd[1]);
  if (read(fd[0], &orphan, sizeof(orphan)) != sizeof(orphan) || orphan <= 0) {
    close(fd[0]);
    return 0;
  }
  close(fd[0]);
  waitpid(middle, 0, 0);
  // a zombie answers kill 0; a reaped one doesn't
  for (i = 0; i < 60 && !gone; i++) {
    nanosleep(&nap, 0);
    gone = kill(orphan, 0) < 0 && errno == ESRCH;
  }
  return gone;
}

int main(void) {
  int alive = orphan_reaped(0), zombie = orphan_reaped(1);

  printf("inittest: init reaped the orphan %s\n", alive ? "ok" : "FAILED");
  printf("inittest: init reaped the zombie it was handed %s\n",
         zombie ? "ok" : "FAILED");
  return alive && zombie ? 0 : 1;
}
