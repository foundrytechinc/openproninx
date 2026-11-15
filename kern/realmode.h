#ifndef PRONINX_X86_64_REALMODE_H
#define PRONINX_X86_64_REALMODE_H

// real mode bios calls from lowmem pages, one segment over all of them
#define RM_PAGES 4
#define RM_DATA 0x1000  // the registers, then the long-mode state
#define RM_BUF 0x1200   // the caller's buffer, up to the stack
#define RM_STACK 0x4000 // its top
#define X86BIOS_BUF 0x20000 // the emulated bios's buffer, 64K of its own low memory

/* struct bios_regs, at RM_DATA */
#define BR_AX 0x00
#define BR_BX 0x02
#define BR_CX 0x04
#define BR_DX 0x06
#define BR_SI 0x08
#define BR_DI 0x0a
#define BR_ES 0x0c
#define BR_VEC 0x0e

#define BS_RSP 0x20
#define BS_RBX 0x28
#define BS_RBP 0x30
#define BS_R12 0x38
#define BS_R13 0x40
#define BS_R14 0x48
#define BS_R15 0x50
#define BS_CR3 0x58
#define BS_CR4 0x60
#define BS_GDT 0x68
#define BS_IDT 0x78
#define BS_CS 0x88

#ifndef LOCORE
#include "inc/types.h"

struct realregs {
  uint16_t ax, bx, cx, dx, si, di, es;
};

void realmode_init(void);
int realmode_available(void);
int realmode_int(int vector, struct realregs *r);
void *realmode_buffer(void);
uint32_t realmode_buffer_lin(void);
uint32_t realmode_ivt(int vector);

void x86bios_init(void);
int x86bios_ready(void);
void *x86bios_buffer(void);
void *x86bios_ptr(uint32_t lin);
int x86bios_int(int vector, struct realregs *r);
int x86bios_post(void);
int x86bios_has_rom(void);
#endif

#endif
