#ifndef FNU_PRONINX_RAMDISK_H
#define FNU_PRONINX_RAMDISK_H

#include "inc/types.h"

struct buf;

void ramdisk_init(void);
uint64_t ramdisk_size(void);
int ramdisk_available(void);
void ramdiskrw(struct buf *b);

#endif /* FNU_PRONINX_RAMDISK_H */
