#include "inc/signal.h"
#include "user.h"

// pid 1 keeps exactly one supervisor alive. it starts no services and no
// shell, so a failed supervisor cannot leave a second tree behind.
static char *supervisor_argv[] = {"/usr/bin/fnusvc", 0};

// every terminal hears it, whichever one the user happens to be watching
static void wall(const char *what) {
  static const char *tty[] = {"/dev/tty1", "/dev/tty2", "/dev/tty3",
                              "/dev/tty4"};
  char line[128];
  int i, fd;

  safestrcpy(line, "\n\n*** FINAL system shutdown message ***\n"
                   "THE SYSTEM IS GOING DOWN FOR ", sizeof(line));
  safestrcpy(line + strlen(line), what, sizeof(line) - strlen(line));
  safestrcpy(line + strlen(line), " NOW\n\n", sizeof(line) - strlen(line));
  for (i = 0; i < 4; i++) {
    fd = open(tty[i], O_RDWR);
    if (fd < 0)
      continue;
    write(fd, line, strlen(line));
    close(fd);
  }
}

// FreeBSD init death() waits ten seconds for a daemon flushing a disk.
// root here is a ramdisk, so the watch is short - only that it is bounded.
#define DEATH_WATCH 200 /* ticks */

static uint64 now(void) {
  struct info sys;

  return info(&sys) == 0 ? sys.uptime : 0;
}

// reap what has died, give the rest until the deadline, then move on
static int reap(uint64 watch) {
  uint64 deadline = now() + watch;
  int n = 0;
  pid_t pid;

  for (;;) {
    pid = waitpid(-1, WNOHANG);
    if (pid > 0) {
      n++;
      continue;
    }
    if (pid < 0 || now() >= deadline)
      break;
    sleep(1);
  }
  return n;
}

// SIGINT reboots, SIGUSR2 powers off. sysvinit's convention.
static void bring_down(int sig) {
  int howto = (sig == SIGINT) ? FNU_RB_REBOOT : FNU_RB_POWEROFF;
  int stopped, killed;

  wall(howto == FNU_RB_REBOOT ? "REBOOT" : "POWEROFF");

  printf("fnu-init: asking every process to stop\n");
  signal(-1, SIGTERM);
  stopped = reap(DEATH_WATCH);
  printf("fnu-init: %d stopped on SIGTERM\n", stopped);

  signal(-1, SIGKILL);
  killed = reap(DEATH_WATCH / 2);
  if (killed)
    printf("fnu-init: %d killed for ignoring it\n", killed);

  printf("fnu-init: handing the machine to the kernel\n");
  halt(howto);
  for (;;)
    ;
}

int main(void) {
  struct siginfo si;
  pid_t pid;
  int reaped;

  if (open("/dev/console", O_RDWR) < 0) {
    if (mknod("console", 1, -1) < 0 || open("console", O_RDWR) < 0) {
      // PID 1 cannot safely continue without its only recovery path.
      for (;;)
        ;
    }
  }
  if (dup(0) < 0 || dup(0) < 0) {
    for (;;)
      ;
  }

  sigcatch(SIGMASK(SIGINT) | SIGMASK(SIGTERM) | SIGMASK(SIGUSR1) |
           SIGMASK(SIGUSR2));

  for (;;) {
    pid = fork();
    if (pid < 0) {
      printf("fnu-init: cannot start supervisor; retrying\n");
      sleep(1);
      continue;
    }
    if (pid == 0) {
      exec("/usr/bin/fnusvc", supervisor_argv);
      // Keep the short-name fallback for development images.
      supervisor_argv[0] = "fnusvc";
      exec("fnusvc", supervisor_argv);
      printf("fnu-init: cannot exec fnusvc\n");
      exit();
    }

    printf("fnu-init: started supervisor (pid %d)\n", pid);
    for (;;) {
      reaped = wait();
      if (sigwait(&si, 0) == 0)
        bring_down(si.signo);
      if (reaped == pid)
        break;
      if (reaped < 0)
        sleep(1);
    }

    // a failed supervisor leaves its children reparented here. reboot rather
    // than spawn a second tree beside them.
    printf("fnu-init: supervisor exited\n");
    sleep(1);
    bring_down(SIGINT);
  }
}
