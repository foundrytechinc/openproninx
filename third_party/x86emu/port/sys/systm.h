// x86emu's sys/systm.h: the kernel's setjmp and longjmp
#ifndef X86EMU_PORT_SYSTM_H
#define X86EMU_PORT_SYSTM_H

int setjmp(label_t *);
__dead void longjmp(label_t *);

#endif
