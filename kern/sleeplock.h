#ifndef PRONINX_X86_64_SLEEPLOCK_H
#define PRONINX_X86_64_SLEEPLOCK_H

#include "inc/types.h"
#include "mutex.h"

struct sleeplock {
  uint locked; // held
  uint waiters;
  struct mutex lk;

  char *name;
  pid_t pid; // holder
  struct proc *owner;
};

#endif /* ifndef PRONINX_X86_64_SLEEPLOCK_H */
