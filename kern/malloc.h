#ifndef PRONINX_MALLOC_H
#define PRONINX_MALLOC_H

#include "inc/types.h"

// MALLOC

#define M_TEMP 0
#define M_DEVBUF 1
#define M_ACPI 2
#define M_SENSOR 3
#define M_VM 4
#define M_PROC 5
#define M_UFS 6
#define M_MOUNT 7
#define M_FILE 8
#define M_VNODE 9
#define M_IOV 10
#define M_LAST 11

#define M_WAITOK 0x01
#define M_NOWAIT 0x02
#define M_CANFAIL 0x04
#define M_ZERO 0x08

void *malloc(size_t size, int type, int flags);
void free(void *p, int type);
int64_t malloc_inuse(void);
int64_t malloc_inuse_type(int type);

void kmeminit(void);

#endif
