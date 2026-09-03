#ifndef PRONINX_X86_64_TRAP_H
#define PRONINX_X86_64_TRAP_H

// These are arbitrarily chosen, but with care not to overlap
// processor defined exceptions or interrupt vectors.
#define T_SYSCALL 0x30 // system call

#define T_IRQ0 32 // IRQ 0 corresponds to int T_IRQ

#define IRQ_TIMER 0
#define IRQ_KBD 1
#define IRQ_COM1 4
#define IRQ_VIRTIO_NET 11
#define IRQ_IDE_PRIMARY 14
#define IRQ_IDE_SECONDARY 15
#define IRQ_ERROR 19
#define IRQ_SPURIOUS 31

#endif /* ifndef PRONINX_X86_64_DEFS_H */
