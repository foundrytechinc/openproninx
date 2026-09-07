#ifndef PRONINX_X86_64_TRAP_H
#define PRONINX_X86_64_TRAP_H

// These are arbitrarily chosen, but with care not to overlap
// processor defined exceptions or interrupt vectors.
#define T_SYSCALL 0x30 // system call

// processor defined exceptions
#define T_DIVIDE 0
#define T_DEBUG 1
#define T_NMI 2
#define T_BRKPT 3
#define T_OFLOW 4
#define T_BOUND 5
#define T_ILLOP 6
#define T_DEVNOTAVAIL 7 // #NM, not the T_DEVICE file type
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

// interrupt stack table slots, one private stack each
#define IST_DBLFLT 1
#define IST_NMI 2
#define IST_MCHK 3
#define IST_DEBUG 4
#define NIST 4

#define T_IRQ0 32 // IRQ 0 corresponds to int T_IRQ

#define IRQ_TIMER 0
#define IRQ_KBD 1
#define IRQ_COM1 4
#define IRQ_VIRTIO_NET 11
#define IRQ_IDE_PRIMARY 14
#define IRQ_IDE_SECONDARY 15
#define IRQ_ERROR 19
#define IRQ_HALT 20
#define IRQ_SPURIOUS 31

#endif /* ifndef PRONINX_X86_64_DEFS_H */
