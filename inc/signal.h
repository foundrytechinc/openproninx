#ifndef PRONINX_SIGNAL_H
#define PRONINX_SIGNAL_H

#include "inc/types.h"

// an unclaimed signal is fatal. never to pid 1.
#define SIGHUP 1
#define SIGINT 2
#define SIGKILL 9
#define SIGTERM 15
#define SIGUSR1 30
#define SIGUSR2 31
#define NSIG 32

#define SIGMASK(s) (1u << (s))

struct siginfo {
  int32_t signo;
  int32_t reserved;
  pid_t sender;
};

// halt(2) howto
#define FNU_RB_HALT 0
#define FNU_RB_POWEROFF 1
#define FNU_RB_REBOOT 2
#define FNU_RB_FORCE 0x100 // bypass init, root only

#endif /* ifndef PRONINX_SIGNAL_H */
