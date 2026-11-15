#ifndef PRONINX_X86_64_MEMLAYOUT_H
#define PRONINX_X86_64_MEMLAYOUT_H

#define EXTMEM 0x100000

#define DMAP_BASE 0xffff800000000000
#define DMAP_MAX 0x0000400000000000

#define IOMAP_BASE 0xffffc00000000000
#define IOMAP_SIZE 0x0000004000000000

#define KHEAP_BASE 0xffffe00000000000
#define KHEAP_SIZE 0x0000008000000000

#define KERNBASE 0xffffffff80000000
#define KERNLINK (KERNBASE + EXTMEM)


#define USERBASE 0x200000
#define USERTOP 0x0000800000000000
#define SIGCODE_VA (USERTOP - 2 * 4096)
#define USRSTACK (SIGCODE_VA - 4096)

#ifndef LOCORE

#include "inc/types.h"

void v2p_bad(uintptr_t va) __attribute__((noreturn));

static inline uintptr_t v2p_(uintptr_t va) {
  if (va >= KERNBASE)
    return va - KERNBASE;
  if (va - DMAP_BASE >= DMAP_MAX)
    v2p_bad(va);
  return va - DMAP_BASE;
}

#define V2P(a) v2p_((uintptr_t)(a))
#define P2V(a) ((void *)(DMAP_BASE + (uintptr_t)(a)))

#endif

#define V2P_WO(x) ((x) - KERNBASE)
#define P2V_WO(x) ((x) + KERNBASE)

#endif
