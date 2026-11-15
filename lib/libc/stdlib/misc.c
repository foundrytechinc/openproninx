#include <stdlib.h>
#include <paths.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }
long labs(long j) { return j < 0 ? -j : j; }

static unsigned int seed = 1;

int rand(void) {
  seed = seed * 1103515245 + 12345;
  return (int)((seed >> 16) & RAND_MAX);
}

void srand(unsigned int s) { seed = s; }

int system(const char *command) {
  pid_t pid;
  int status;
  sigset_t mask, omask;
  struct sigaction ign, intact, quitact;

  if (command == 0)
    return 1;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = (sig_t)SIG_IGN;
  sigaction(SIGINT, &ign, &intact);
  sigaction(SIGQUIT, &ign, &quitact);
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &omask);
  switch (pid = fork()) {
  case -1:
    status = -1;
    break;
  case 0:
    sigaction(SIGINT, &intact, 0);
    sigaction(SIGQUIT, &quitact, 0);
    sigprocmask(SIG_SETMASK, &omask, 0);
    execl(_PATH_BSHELL, "sh", "-c", command, (char *)0);
    _exit(127);
  default:
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      ;
    break;
  }
  sigaction(SIGINT, &intact, 0);
  sigaction(SIGQUIT, &quitact, 0);
  sigprocmask(SIG_SETMASK, &omask, 0);
  return status;
}
