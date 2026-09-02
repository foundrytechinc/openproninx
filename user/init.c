#include "user.h"

// PID 1 deliberately has one responsibility: keep exactly one service
// supervisor alive. It does not start individual services or an interactive
// shell, so a failed supervisor cannot leave a second service tree behind.
static char *supervisor_argv[] = {"/bin/fnusvc", 0};

int main(void) {
  pid_t pid;

  if (open("console", O_RDWR) < 0) {
    if (mknod("console", 1, 1) < 0 || open("console", O_RDWR) < 0) {
      // PID 1 cannot safely continue without its only recovery path.
      for (;;)
        ;
    }
  }
  if (dup(0) < 0 || dup(0) < 0) {
    for (;;)
      ;
  }

  for (;;) {
    pid = fork();
    if (pid < 0) {
      printf("fnu-init: cannot start supervisor; retrying\n");
      sleep(1);
      continue;
    }
    if (pid == 0) {
      exec("/bin/fnusvc", supervisor_argv);
      // Development images still place programs in /. Release UFS2 images
      // use /bin and are always attempted first.
      supervisor_argv[0] = "fnusvc";
      exec("fnusvc", supervisor_argv);
      printf("fnu-init: cannot exec fnusvc\n");
      exit();
    }

    printf("fnu-init: started supervisor (pid %d)\n", pid);
    wait();

    // Children of a failed supervisor are reparented to PID 1. Reboot instead
    // of spawning another tree beside them; this preserves a single owner.
    printf("fnu-init: supervisor exited; rebooting\n");
    sleep(1);
    reboot();
    for (;;)
      ;
  }
}
