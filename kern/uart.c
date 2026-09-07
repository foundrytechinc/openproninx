// ref. https://wiki.osdev.org/Serial_Ports

#include "defs.h"
#include "trap.h"
#include "x86.h"

#define COM1 0x3f8

static int uart; // is there a uart?

void uartinit(void) {
  char *p;

  // Turn on 16550A FIFO: enable FIFO, clear TX/RX, 14-byte threshold
  outb(COM1 + 2, 0xC7);

  // 115200 baud, 8 data bits, 1 stop bit, parity off.
  outb(COM1 + 3, 0x80); // Unlock divisor
  outb(COM1 + 0, 115200 / 115200); // 115200 baud divisor = 1
  outb(COM1 + 1, 0);
  outb(COM1 + 3, 0x03); // Lock divisor, 8 data bits.
  outb(COM1 + 4, 0x0B); // Modem control: DTR + RTS + OUT2
  outb(COM1 + 1, 0x01); // Enable receive interrupts.

  // If status is 0xFF, no serial port.
  if (inb(COM1 + 5) == 0xFF) {
    return;
  }
  uart = 1;

  for (p = "OpenProninx...\n"; *p; p++) {
    uartputc(*p);
  }
}

void uartintr_enable(void) {
  if (!uart)
    return;
  inb(COM1 + 2);
  inb(COM1 + 0);
  ioapicenable(IRQ_COM1, 0);
}

void uartputc(int c) {
  int i;

  if (!uart) {
    return;
  }
  for (i = 0; i < 128 && !(inb(COM1 + 5) & 0x20); i++) {
    // Fast non-blocking polling
  }
  outb(COM1 + 0, c);
}

static int uartgetc(void) {
  if (!uart) {
    return -1;
  }
  if (!(inb(COM1 + 5) & 0x01)) {
    return -1;
  }
  return inb(COM1 + 0);
}

void uartintr(void) { consoleintr(uartgetc); }
