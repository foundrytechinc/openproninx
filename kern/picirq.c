
#include "trap.h"
#include "x86.h"

#define PIC_MASTER_COMMAND 0x20
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_COMMAND 0xA0
#define PIC_SLAVE_DATA 0xA1

#define ICW1_INIT 0x11 // begin initialization, expect icw4
#define ICW3_SLAVE_LINE 2
#define ICW4_8086 0x01

#define OCW2_EOI 0x20
#define OCW3_READ_IRR 0x0a
#define OCW3_READ_ISR 0x0b

void i8259_init(void) {
  outb(PIC_MASTER_COMMAND, ICW1_INIT);
  outb(PIC_SLAVE_COMMAND, ICW1_INIT);

  outb(PIC_MASTER_DATA, T_IRQ0);
  outb(PIC_SLAVE_DATA, T_IRQ0 + 8);

  outb(PIC_MASTER_DATA, 1 << ICW3_SLAVE_LINE);
  outb(PIC_SLAVE_DATA, ICW3_SLAVE_LINE);

  outb(PIC_MASTER_DATA, ICW4_8086);
  outb(PIC_SLAVE_DATA, ICW4_8086);

  outb(PIC_MASTER_COMMAND, OCW3_READ_IRR);
  outb(PIC_SLAVE_COMMAND, OCW3_READ_IRR);

  outb(PIC_MASTER_DATA, 0xFF);
  outb(PIC_SLAVE_DATA, 0xFF);
}

int pic_spurious(int irq) {
  int port = (irq == 15) ? PIC_SLAVE_COMMAND : PIC_MASTER_COMMAND;
  int isr;

  if (irq != 7 && irq != 15)
    return 0;

  outb(port, OCW3_READ_ISR);
  isr = inb(port);
  outb(port, OCW3_READ_IRR);
  if (isr & (1 << 7))
    return 0;

  if (irq == 15)
    outb(PIC_MASTER_COMMAND, OCW2_EOI);
  return 1;
}
