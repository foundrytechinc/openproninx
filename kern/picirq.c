// ref. https://wiki.osdev.org/8259_PIC
// ref. https://pdos.csail.mit.edu/6.828/2018/readings/hardware/8259A.pdf

#include "trap.h"
#include "x86.h"

// I/O Addresses of the two programmable interrupt controllers
#define PIC_MASTER_COMMAND 0x20 // Master - Command (IRQs 0-7)
#define PIC_MASTER_DATA 0x21    // Master - Data (IRQs 0-7)
#define PIC_SLAVE_COMMAND 0xA0  // Slave - Command (IRQs 8-15)
#define PIC_SLAVE_DATA 0xA1     // Slave - Data (IRQs 8-15)

#define ICW1_INIT 0x11 // begin initialization, expect icw4
#define ICW3_SLAVE_LINE 2
#define ICW4_8086 0x01

#define OCW2_EOI 0x20
#define OCW3_READ_IRR 0x0a
#define OCW3_READ_ISR 0x0b

// Don't use the 8259A interrupt controllers.  PRONINX assumes SMP hardware.
// Masking alone leaves them on the firmware vector base, on top of the
// processor exception vectors.
void picinit(void) {
  outb(PIC_MASTER_COMMAND, ICW1_INIT);
  outb(PIC_SLAVE_COMMAND, ICW1_INIT);

  outb(PIC_MASTER_DATA, T_IRQ0);
  outb(PIC_SLAVE_DATA, T_IRQ0 + 8);

  outb(PIC_MASTER_DATA, 1 << ICW3_SLAVE_LINE);
  outb(PIC_SLAVE_DATA, ICW3_SLAVE_LINE);

  outb(PIC_MASTER_DATA, ICW4_8086);
  outb(PIC_SLAVE_DATA, ICW4_8086);

  // reads return the request register unless asked otherwise
  outb(PIC_MASTER_COMMAND, OCW3_READ_IRR);
  outb(PIC_SLAVE_COMMAND, OCW3_READ_IRR);

  outb(PIC_MASTER_DATA, 0xFF);
  outb(PIC_SLAVE_DATA, 0xFF);
}

// A line that drops before the acknowledge leaves the 8259 answering irq 7,
// or 15 on the slave. Not real, and must not be acknowledged.
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

  // the master counted the slave phantom and still wants an eoi
  if (irq == 15)
    outb(PIC_MASTER_COMMAND, OCW2_EOI);
  return 1;
}
