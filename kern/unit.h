#ifndef PRONINX_UNIT_H
#define PRONINX_UNIT_H

#include "inc/types.h"
#include "mutex.h"

struct unrhdr {
  struct mutex lock;
  uint8_t *used;
  int n;
};

void init_unrhdr(struct unrhdr *uh, const char *name);
int alloc_unr(struct unrhdr *uh);
void free_unr(struct unrhdr *uh, int unit);

#endif
