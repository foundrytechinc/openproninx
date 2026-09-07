#ifndef PRONINX_X86_64_REALMODE_H
#define PRONINX_X86_64_REALMODE_H

/* real mode bios calls, the way 9front pc64 does it. all of it below 64K. */
#define RM_CODE 0xb000   /* trampoline, one page */
#define RM_DATA 0xc000  /* register block and saved state */
#define RM_BUF 0xc200   /* transfer buffer for the bios, 3.5K */
#define RM_STACK 0xf000 /* real mode stack top, grows into 0xd000 */
#define RM_PML4 0x10000
#define RM_PDPT 0x11000
#define RM_PD 0x12000

/* struct bios_regs, at RM_DATA */
#define BR_AX 0x00
#define BR_BX 0x02
#define BR_CX 0x04
#define BR_DX 0x06
#define BR_SI 0x08
#define BR_DI 0x0a
#define BR_ES 0x0c
#define BR_VEC 0x0e

/* saved long mode state */
#define BS_RSP 0x20
#define BS_RBX 0x28
#define BS_RBP 0x30
#define BS_R12 0x38
#define BS_R13 0x40
#define BS_R14 0x48
#define BS_R15 0x50
#define BS_CR3 0x58
#define BS_CR4 0x60
#define BS_GDT 0x68 /* 10 bytes */
#define BS_IDT 0x78 /* 10 bytes */
#define BS_CS 0x88

#ifndef __ASSEMBLER__
#include "inc/types.h"

// what a bios call needs and answers
struct realregs {
  uint16_t ax, bx, cx, dx, si, di, es;
};

void realmode_init(void);
int realmode_available(void);
int realmode_int(int vector, struct realregs *r);
void *realmode_buffer(void);
uint32_t realmode_ivt(int vector);
#endif

#endif /* ifndef PRONINX_X86_64_REALMODE_H */
