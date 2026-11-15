#ifndef PRONINX_X86_64_FPU_H
#define PRONINX_X86_64_FPU_H

#include "inc/types.h"

struct fxsave {
  uchar data[512];
} __aligned(16);

void fpuinit(void);
void fpusave(struct fxsave *area);
void fpurestore(const struct fxsave *area);
void fpureset(struct fxsave *area);
void fpuclean(struct fxsave *area);

#endif
