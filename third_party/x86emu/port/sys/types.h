// x86emu's sys/types.h: the kernel's own types
#ifndef X86EMU_PORT_TYPES_H
#define X86EMU_PORT_TYPES_H

#include "inc/types.h"

#define __BEGIN_DECLS
#define __END_DECLS
#define __dead __attribute__((__noreturn__))

typedef struct {
  long val[8];
} label_t;

#endif
