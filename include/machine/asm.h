// ASM CONVENTIONS. no return guard: RETGUARD is empty
#ifndef _MACHINE_ASM_H_
#define _MACHINE_ASM_H_

#define _C_LABEL(x)	x
#define _ENTRY(x)	.text; .globl x; .type x,@function; .align 16; x:
#define ENTRY(y)	_ENTRY(_C_LABEL(y))
#define NENTRY(y)	_ENTRY(_C_LABEL(y))
#define ENTRY_NB(y)	.text; .type y,@function; .align 16; y:
#define END(y)		.size y, . - y

#define STRONG_ALIAS(alias,sym)	.global alias; alias = sym
#define WEAK_ALIAS(alias,sym)	.weak alias; alias = sym

#define RETGUARD_SETUP_OFF(x, reg, off)
#define RETGUARD_SETUP(x, reg)
#define RETGUARD_CHECK(x, reg)
#define RETGUARD_PUSH(reg)
#define RETGUARD_POP(reg)
#define RETGUARD_SYMBOL(x)

#endif /* _MACHINE_ASM_H_ */
