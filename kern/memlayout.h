#ifndef PRONINX_X86_64_MEMLAYOUT_H
#define PRONINX_X86_64_MEMLAYOUT_H

#define EXTMEM 0x100000

#define DMAP_BASE 0xffff800000000000 // every physical page, 1G entries
#define DMAP_MAX 0x0000400000000000

#define IOMAP_BASE 0xffffc00000000000 // ioremap window
#define IOMAP_SIZE 0x0000004000000000

#define KERNBASE 0xffffffff80000000 // kernel image, top 2G
#define KERNLINK (KERNBASE + EXTMEM)

#ifndef __ASSEMBLER__

#include "inc/types.h"

static inline uintptr_t v2p_(uintptr_t va) {
  return va >= KERNBASE ? va - KERNBASE : va - DMAP_BASE;
}

#define V2P(a) v2p_((uintptr_t)(a))
#define P2V(a) ((void *)(DMAP_BASE + (uintptr_t)(a)))

#endif /* __ASSEMBLER__ */

#define V2P_WO(x) ((x) - KERNBASE)
#define P2V_WO(x) ((x) + KERNBASE)

#endif /* PRONINX_X86_64_MEMLAYOUT_H */
