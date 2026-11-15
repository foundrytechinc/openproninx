#ifndef _LIBC_AMD64_ASM_H_
#define _LIBC_AMD64_ASM_H_

#define _C_LABEL(x) x
#define _ENTRY(x) .text; .globl x; .type x, @function; x:
#define ENTRY(y) _ENTRY(_C_LABEL(y))
#define NENTRY(y) _ENTRY(_C_LABEL(y))
#define END(y) .size y, . - y

// without _RET_PROTECTOR
#define RETGUARD_SETUP_OFF(x, reg, off)
#define RETGUARD_SETUP(x, reg)
#define RETGUARD_CHECK(x, reg)
#define RETGUARD_PUSH(reg)
#define RETGUARD_POP(reg)
#define RETGUARD_SYMBOL(x)

#endif
