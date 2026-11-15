#ifndef PRONINX_MUTEX_H
#define PRONINX_MUTEX_H

#include "inc/types.h"

// MUTEX

struct mutex {
  volatile ulong owner;
  const char *name;
  ulong pc;             // where it was taken
};

#define MUTEX_INITIALIZER(n) {0, (n), 0}

void mtx_init(struct mutex *m, const char *name);
void mtx_enter(struct mutex *m);
int mtx_enter_try(struct mutex *m);
void mtx_leave(struct mutex *m);
int mtx_owned(struct mutex *m);  // this cpu has it
int mtx_locked(struct mutex *m); // somebody has it

#endif
