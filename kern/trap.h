#ifndef PRONINX_X86_64_TRAP_H
#define PRONINX_X86_64_TRAP_H

#define T_SYSCALL 0x30

#define T_DIVIDE 0
#define T_DEBUG 1
#define T_NMI 2
#define T_BRKPT 3
#define T_OFLOW 4
#define T_BOUND 5
#define T_ILLOP 6
#define T_DEVNOTAVAIL 7
#define T_DBLFLT 8
#define T_TSS 10
#define T_SEGNP 11
#define T_STACK 12
#define T_GPFLT 13
#define T_PGFLT 14
#define T_FPERR 16
#define T_ALIGN 17
#define T_MCHK 18
#define T_SIMDERR 19

#define IST_DBLFLT 1
#define IST_NMI 2
#define IST_MCHK 3
#define IST_DEBUG 4
#define NIST 4

// THE VECTORS
#define T_IRQ0 32
#define T_DEVICE0 49
#define T_DEVICEN 239
#define T_APIC_ERROR 240
#define T_APIC_TIMER 241
#define T_IPI_HALT 243
#define T_IPI_SIGNOTIFY 244
#define T_IPI_TLB 245
#define T_APIC_SPURIOUS 255

#define IRQ_KBD 1
#define IRQ_AUX 12
#define IRQ_COM2 3
#define IRQ_COM1 4
#define IRQ_IDE_PRIMARY 14
#define IRQ_IDE_SECONDARY 15

#endif
