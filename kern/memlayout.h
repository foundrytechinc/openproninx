#ifndef PRONINX_X86_64_MEMLAYOUT_H
#define PRONINX_X86_64_MEMLAYOUT_H

/*
 * Memory layout definitions for OpenProninx x86-64.
 */

// Start of extended memory
#define EXTMEM 0x100000

// Address for devices and PCI MMIO window (up to 4 GiB)
#define DEVSPACE_PHYS 0xe0000000
#define DEVSPACE_P2V(a) ((void *)(((uint64_t)(a)) + 0xffffffff00000000))

// Key addresses for address space layout (see kmap in vm.c for layout)
#define KERNBASE 0xffffffff80000000  // First kernel virtual address
#define KERNLINK (KERNBASE + EXTMEM) // Address where kernel is linked

// __ASSEMBLER__ is defined by gcc when processing assembly files
#ifndef __ASSEMBLER__

#include "inc/types.h"

#define V2P(a) (((uintptr_t)(a)) - KERNBASE)
#define P2V(a) ((void *)(((char *)(a)) + KERNBASE))

#endif /* __ASSEMBLER__ */

#define V2P_WO(x) ((x)-KERNBASE)   // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE) // same as P2V, but without casts

/*
 * Virtual memory map:                                Permissions
 *                                                    kernel/user
 *
 *    256 TB -------->  +------------------------------+
 *                      |                              | RW/--
 *     DEVSPACE ----->  +------------------------------+ 0xffffffffe0000000
 *                      :                              :
 *     phys_top ----->  +------------------------------+
 *                      |                              | RW/--
 *                      |   remapped physical memory   | RW/--
 *                      |                              | RW/--
 *     KERNLINK ---->   |                              | 0xffffffff80100000
 * (kernel entry point) |                              | RW/--
 *     KERNBASE ---->   +------------------------------+ 0xffffffff80000000
 *                      :              .               :
 *  start of    ---->   +------------------------------+ 0xffff800000000000
 *  kernel space
 */

#endif /* PRONINX_X86_64_MEMLAYOUT_H */
