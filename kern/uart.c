// UART

#include "inc/abi.h"
#include "comreg.h"
#include "tty.h"
#include "defs.h"
#include "mmu.h"
#include "param.h"
#include "mutex.h"
#include "trap.h"
#include "x86.h"

#define NCOM 4
#define TXBUF 1024
#define RXRUN 64

enum {
  COM_UART_UNKNOWN,
  COM_UART_8250,
  COM_UART_16450,
  COM_UART_16550,
  COM_UART_16550A,
  COM_UART_ST16650,
  COM_UART_ST16650V2,
  COM_UART_TI16750,
};

static const char *const typename[] = {
    "unknown uart", "ns8250", "ns16450", "ns16550",
    "ns16550a",     "st16650", "st16650", "ti16750",
};

struct com_softc {
  int iobase;
  int irq;
  int unit;
  int present;
  int console;
  int uarttype;
  int fifolen;
  int fifo;
  int softcar;
  uint8_t ier, mcr, msr, lcr, dtr;
  int ospeed;
  int busy;
  struct mutex lock;
  struct {
    char buf[TXBUF];
    uint r, w;
  } tx;
};

static struct com_softc coms[NCOM];
static int ncom;
static struct com_softc *console_port;

static const int legacy_port[NCOM] = {0x3f8, 0x2f8, 0x3e8, 0x2e8};
static const int legacy_irq[NCOM] = {IRQ_COM1, IRQ_COM2, IRQ_COM1, IRQ_COM2};

static uint8_t rd(struct com_softc *sc, int reg) {
  return inb((ushort)(sc->iobase + reg));
}

static void wr(struct com_softc *sc, int reg, uint8_t v) {
  outb((ushort)(sc->iobase + reg), v);
}

static int comspeed(long freq, long speed) {
#define divrnd(n, q) (((n)*2 / (q) + 1) / 2)
  int x, err;

  if (speed == 0)
    return 0;
  if (speed < 0)
    return -1;
  x = (int)divrnd((freq / 16), speed);
  if (x <= 0 || x > 0xffff)
    return -1;
  err = (int)divrnd((int64_t)freq * 1000 / 16, speed * x) - 1000;
  if (err < 0)
    err = -err;
  if (err > COM_TOLERANCE)
    return -1;
  return x;
#undef divrnd
}

#define EARLY_RX 64
static char early_rx[EARLY_RX];
static int nearly_rx;

static void early_keep(int iobase) {
  while ((inb((ushort)(iobase + com_lsr)) & LSR_RXRDY) && nearly_rx < EARLY_RX)
    early_rx[nearly_rx++] = (char)inb((ushort)(iobase + com_data));
}

static int probe1(int iobase, char *kept, int *nkept) {
  int i;
  uint8_t lsr;

  *nkept = 0;
  outb((ushort)(iobase + com_lcr), LCR_8BITS);
  outb((ushort)(iobase + com_iir), 0);
  for (i = 0; i < 32; i++) {
    if (inb((ushort)(iobase + com_lcr)) != LCR_8BITS ||
        (inb((ushort)(iobase + com_iir)) & 0x38)) {
      lsr = inb((ushort)(iobase + com_lsr));
      if (lsr != 0xff && (lsr & LSR_RXRDY) && *nkept < EARLY_RX)
        kept[(*nkept)++] = (char)inb((ushort)(iobase + com_data));
      else
        inb((ushort)(iobase + com_data));
    } else
      break;
  }
  return i < 32;
}

static void type_probe(struct com_softc *sc) {
  uint8_t lcr;

  sc->ier = 0;
  wr(sc, com_ier, 0);

  lcr = rd(sc, com_lcr);
  wr(sc, com_lcr, LCR_EFR);
  wr(sc, com_efr, 0);
  wr(sc, com_lcr, 0);
  wr(sc, com_fifo, FIFO_ENABLE);
  delay(100);

  switch (rd(sc, com_iir) >> 6) {
  case 0:
    sc->uarttype = COM_UART_16450;
    break;
  case 2:
    sc->uarttype = COM_UART_16550;
    break;
  case 3:
    sc->uarttype = COM_UART_16550A;
    break;
  default:
    sc->uarttype = COM_UART_UNKNOWN;
    break;
  }

  if (sc->uarttype == COM_UART_16550A) {
    wr(sc, com_lcr, (uint8_t)(lcr | LCR_DLAB));
    if (rd(sc, com_efr) == 0) {
      wr(sc, com_efr, EFR_CTS);
      if (rd(sc, com_efr) != 0)
        sc->uarttype = COM_UART_ST16650;
      wr(sc, com_efr, 0);
    } else {
      wr(sc, com_lcr, LCR_EFR);
      if (rd(sc, com_efr) == 0)
        sc->uarttype = COM_UART_ST16650V2;
    }
  }

  if (sc->uarttype == COM_UART_16550A) {
    wr(sc, com_lcr, (uint8_t)(lcr | LCR_DLAB));
    wr(sc, com_fifo, FIFO_ENABLE | FIFO_ENABLE_64BYTE);
    if ((rd(sc, com_iir) >> 5) == 7)
      sc->uarttype = COM_UART_TI16750;
    wr(sc, com_fifo, FIFO_ENABLE);
  }

  wr(sc, com_lcr, lcr);

  if (sc->uarttype == COM_UART_16450) {
    uint8_t scr0, scr1, scr2;

    scr0 = rd(sc, com_scratch);
    wr(sc, com_scratch, 0xa5);
    scr1 = rd(sc, com_scratch);
    wr(sc, com_scratch, 0x5a);
    scr2 = rd(sc, com_scratch);
    wr(sc, com_scratch, scr0);
    if (scr1 != 0xa5 || scr2 != 0x5a)
      sc->uarttype = COM_UART_8250;
  }

  switch (sc->uarttype) {
  case COM_UART_16550A:
    sc->fifolen = 16;
    sc->fifo = 1;
    break;
  case COM_UART_ST16650V2:
    sc->fifolen = 32;
    sc->fifo = 1;
    break;
  case COM_UART_TI16750:
    sc->fifolen = 64;
    sc->fifo = 1;
    break;
  default:
    sc->fifolen = 1;
    sc->fifo = 0;
    break;
  }
}

static void fifo_probe(struct com_softc *sc) {
  int timo, len;
  uint8_t fifo;

  if (!sc->fifo || sc->console)
    return;
  wr(sc, com_ier, 0);
  wr(sc, com_lcr, LCR_DLAB);
  wr(sc, com_dlbl, 3);
  wr(sc, com_dlbh, 0);
  wr(sc, com_lcr, LCR_PNONE | LCR_8BITS);
  wr(sc, com_mcr, MCR_LOOPBACK);

  fifo = FIFO_ENABLE | FIFO_RCV_RST | FIFO_XMT_RST;
  if (sc->uarttype == COM_UART_TI16750)
    fifo |= FIFO_ENABLE_64BYTE;
  wr(sc, com_fifo, fifo);

  for (len = 0; len < 256; len++) {
    wr(sc, com_data, (uint8_t)(len + 1));
    timo = 2000;
    while (!(rd(sc, com_lsr) & LSR_TXRDY) && --timo)
      delay(1);
    if (!timo)
      break;
  }
  delay(100);
  for (len = 0; len < 256; len++) {
    timo = 2000;
    while (!(rd(sc, com_lsr) & LSR_RXRDY) && --timo)
      delay(1);
    if (!timo || rd(sc, com_data) != (uint8_t)(len + 1))
      break;
  }
  if (sc->fifolen > len)
    sc->fifolen = len;
  if (sc->fifolen == 0) {
    sc->fifo = 0;
    sc->fifolen = 1;
  }
  wr(sc, com_mcr, 0);
  wr(sc, com_fifo, 0);
}

static int set_params(struct com_softc *sc, const struct fnu_ttymode *t) {
  int ospeed = comspeed(COM_FREQ, (long)t->speed_out);
  uint8_t lcr;

  if (ospeed < 0 || (t->speed_in != 0 && t->speed_in != t->speed_out))
    return -FNU_ERR_INVALID;

  lcr = (uint8_t)(sc->lcr & LCR_SBREAK);
  switch (t->line & FNU_TTYLINE_BITS) {
  case FNU_TTYLINE_5BITS:
    lcr |= LCR_5BITS;
    break;
  case FNU_TTYLINE_6BITS:
    lcr |= LCR_6BITS;
    break;
  case FNU_TTYLINE_7BITS:
    lcr |= LCR_7BITS;
    break;
  default:
    lcr |= LCR_8BITS;
    break;
  }
  if (t->line & FNU_TTYLINE_PARITY) {
    lcr |= LCR_PENAB;
    if (!(t->line & FNU_TTYLINE_ODD))
      lcr |= LCR_PEVEN;
  }
  if (t->line & FNU_TTYLINE_TWOSTOP)
    lcr |= LCR_STOPB;
  sc->lcr = lcr;

  if (ospeed == 0) {
    // B0 drops the line
    sc->mcr &= (uint8_t)~MCR_DTR;
    wr(sc, com_mcr, sc->mcr);
    wr(sc, com_lcr, lcr);
  } else {
    wr(sc, com_lcr, (uint8_t)(lcr | LCR_DLAB));
    wr(sc, com_dlbl, (uint8_t)ospeed);
    wr(sc, com_dlbh, (uint8_t)(ospeed >> 8));
    wr(sc, com_lcr, lcr);
    sc->mcr |= MCR_DTR;
    wr(sc, com_mcr, sc->mcr);
  }
  sc->ospeed = (int)t->speed_out;

  if (sc->fifo) {
    uint8_t fifo = FIFO_ENABLE |
                   (t->speed_out <= 1200 ? FIFO_TRIGGER_1 : FIFO_TRIGGER_8);

    if (sc->uarttype == COM_UART_TI16750) {
      wr(sc, com_lcr, (uint8_t)(lcr | LCR_DLAB));
      wr(sc, com_fifo, (uint8_t)(fifo | FIFO_ENABLE_64BYTE));
      wr(sc, com_lcr, lcr);
    } else {
      wr(sc, com_fifo, fifo);
    }
  }

  // without CRTSCTS, RTS follows DTR
  if (!(t->line & FNU_TTYLINE_HWFLOW)) {
    if (sc->mcr & MCR_DTR)
      sc->mcr |= MCR_RTS;
    else
      sc->mcr &= (uint8_t)~MCR_RTS;
    wr(sc, com_mcr, sc->mcr);
    sc->dtr = MCR_DTR | MCR_RTS;
  } else {
    sc->dtr = MCR_DTR;
  }
  return 0;
}

static void com_start(struct com_softc *sc) {
  int i, n;

  if (sc->busy)
    return;
  if (sc->tx.r == sc->tx.w) {
    if (sc->ier & IER_ETXRDY) {
      sc->ier &= (uint8_t)~IER_ETXRDY;
      wr(sc, com_ier, sc->ier);
    }
    return;
  }
  if (!(sc->ier & IER_ETXRDY)) {
    sc->ier |= IER_ETXRDY;
    wr(sc, com_ier, sc->ier);
  }
  n = sc->fifo ? sc->fifolen : 1;
  for (i = 0; i < n && sc->tx.r != sc->tx.w; i++)
    wr(sc, com_data, (uint8_t)sc->tx.buf[sc->tx.r++ % TXBUF]);
  sc->busy = 1;
}

void com_init(void) {
  char kept[EARLY_RX];
  int i, nkept;

  for (i = 0; i < NCOM; i++) {
    struct com_softc *sc = &coms[i];

    sc->unit = i;
    sc->iobase = legacy_port[i];
    sc->irq = legacy_irq[i];
    mtx_init(&sc->lock, "uart");
    if (!probe1(sc->iobase, kept, &nkept))
      continue;
    if (console_port == 0) {
      memmove(early_rx, kept, nkept);
      nearly_rx = nkept;
    }
    sc->present = 1;
    sc->console = console_port == 0;
    sc->softcar = sc->console;
    if (sc->console)
      early_keep(sc->iobase);
    type_probe(sc);
    fifo_probe(sc);
    sc->lcr = LCR_PNONE | LCR_8BITS;
    sc->ospeed = sc->console ? UART_CONSOLE_SPEED : FNU_TTY_SPEED;
    if (sc->console)
      early_keep(sc->iobase);
    wr(sc, com_lcr, (uint8_t)(sc->lcr | LCR_DLAB));
    wr(sc, com_dlbl, (uint8_t)comspeed(COM_FREQ, sc->ospeed));
    wr(sc, com_dlbh, (uint8_t)(comspeed(COM_FREQ, sc->ospeed) >> 8));
    wr(sc, com_lcr, sc->lcr);
    if (sc->fifo)
      wr(sc, com_fifo, FIFO_ENABLE | FIFO_RCV_RST | FIFO_XMT_RST |
                           FIFO_TRIGGER_8);
    else
      wr(sc, com_fifo, FIFO_RCV_RST | FIFO_XMT_RST);
    sc->mcr = MCR_DTR | MCR_RTS | MCR_IENABLE;
    sc->dtr = MCR_DTR | MCR_RTS;
    wr(sc, com_mcr, sc->mcr);
    sc->msr = rd(sc, com_msr);
    ncom++;
    if (console_port == 0)
      console_port = sc;
  }
}

// SLEEP
void uart_resume(void) {
  int i;

  for (i = 0; i < NCOM; i++) {
    struct com_softc *sc = &coms[i];

    if (!sc->present)
      continue;
    mtx_enter(&sc->lock);
    wr(sc, com_lcr, (uint8_t)(sc->lcr | LCR_DLAB));
    wr(sc, com_dlbl, (uint8_t)comspeed(COM_FREQ, sc->ospeed));
    wr(sc, com_dlbh, (uint8_t)(comspeed(COM_FREQ, sc->ospeed) >> 8));
    wr(sc, com_lcr, sc->lcr);
    if (sc->fifo)
      wr(sc, com_fifo, FIFO_ENABLE | FIFO_RCV_RST | FIFO_XMT_RST |
                           FIFO_TRIGGER_8);
    wr(sc, com_mcr, sc->mcr);
    rd(sc, com_iir);
    rd(sc, com_data);
    sc->msr = rd(sc, com_msr);
    wr(sc, com_ier, sc->ier);
    sc->busy = 0;
    com_start(sc);
    mtx_leave(&sc->lock);
  }
}

int uart_ports(void) { return ncom; }

int uart_console_speed(void) {
  return console_port != 0 ? console_port->ospeed : FNU_TTY_SPEED;
}

void uart_announce(void) {
  int i;

  for (i = 0; i < NCOM; i++) {
    struct com_softc *sc = &coms[i];

    if (!sc->present)
      continue;
    printf("ttyu%d: %s at 0x%x irq %d", i, typename[sc->uarttype],
            sc->iobase, sc->irq);
    if (sc->fifo)
      printf(", %d byte fifo", sc->fifolen);
    if (sc->console)
      printf(", console at %d baud", sc->ospeed);
    printf("\n");
  }
}

static int uart_irq(void *arg __unused) {
  uartintr();
  return 1;
}

void uartintr_enable(void) {
  int i, vector, done[2] = {0, 0};

  for (i = 0; i < NCOM; i++) {
    struct com_softc *sc = &coms[i];

    if (!sc->present)
      continue;
    mtx_enter(&sc->lock);
    rd(sc, com_iir);
    if (sc->console)
      early_keep(sc->iobase);
    else
      rd(sc, com_data);
    sc->ier = IER_ERXRDY | IER_ERLS | IER_EMSC;
    wr(sc, com_ier, sc->ier);
    mtx_leave(&sc->lock);
    vector = ioapic_isa_vector(sc->irq, 0);
    if (!done[sc->irq == IRQ_COM1 ? 0 : 1]++)
      intr_establish(vector, uart_irq, (void *)0, "com");
  }
  if (nearly_rx > 0 && console_port != 0) {
    int early[EARLY_RX], i;

    for (i = 0; i < nearly_rx; i++)
      early[i] = early_rx[i] & 0xff;
    console_serial_input(console_port->unit, early, nearly_rx);
    nearly_rx = 0;
  }
}

void comcnputc(int c) {
  struct com_softc *sc = console_port;
  uint64_t flags;
  int i;

  if (sc == 0)
    return;
  flags = readeflags();
  cli();
  wr(sc, com_ier, 0);
  for (i = 0; i < 2000 && !(rd(sc, com_lsr) & LSR_TXRDY); i++)
    delay(1);
  wr(sc, com_data, (uint8_t)c);
  for (i = 0; i < 2000 && !(rd(sc, com_lsr) & LSR_TXRDY); i++)
    delay(1);
  wr(sc, com_ier, sc->ier);
  if (flags & FL_IF)
    sti();
}

int comcncheckc(void) {
  struct com_softc *sc = console_port;

  if (sc == 0 || !(rd(sc, com_lsr) & LSR_RXRDY))
    return -1;
  return rd(sc, com_data);
}

void uart_tty_putc(int unit, int c) {
  struct com_softc *sc;

  if (unit < 0 || unit >= NCOM || !coms[unit].present)
    return;
  sc = &coms[unit];
  mtx_enter(&sc->lock);
  if (sc->tx.w - sc->tx.r < TXBUF)
    sc->tx.buf[sc->tx.w++ % TXBUF] = (char)c;
  com_start(sc);
  mtx_leave(&sc->lock);
}

int uart_tx_full(int unit) {
  struct com_softc *sc;
  int full;

  if (unit < 0 || unit >= NCOM || !coms[unit].present)
    return 0;
  sc = &coms[unit];
  mtx_enter(&sc->lock);
  full = sc->tx.w - sc->tx.r >= TXBUF;
  mtx_leave(&sc->lock);
  return full;
}

int uart_outq(int unit) {
  struct com_softc *sc;
  int n;

  if (unit < 0 || unit >= NCOM || !coms[unit].present)
    return 0;
  sc = &coms[unit];
  mtx_enter(&sc->lock);
  n = (int)(sc->tx.w - sc->tx.r);
  mtx_leave(&sc->lock);
  return n;
}

void *uart_tx_chan(int unit) {
  if (unit < 0 || unit >= NCOM)
    return 0;
  return &coms[unit].tx.r;
}

// FNU_TTYDRAIN
void uart_drain(int unit) {
  struct com_softc *sc;

  if (unit < 0 || unit >= NCOM || !coms[unit].present)
    return;
  sc = &coms[unit];
  mtx_enter(&sc->lock);
  while (sc->tx.r != sc->tx.w || sc->busy) {
    if (proc_interrupted())
      break;
    msleep(&sc->tx.r, &sc->lock, PZERO | PCATCH, "ttyout", 0);
  }
  mtx_leave(&sc->lock);
}

int uart_param(int unit, const struct fnu_ttymode *t) {
  struct com_softc *sc;
  int r;

  if (unit < 0 || unit >= NCOM || !coms[unit].present)
    return -FNU_ERR_BADREQUEST;
  sc = &coms[unit];
  mtx_enter(&sc->lock);
  r = set_params(sc, t);
  mtx_leave(&sc->lock);
  return r;
}

static uint8_t tiocm_to_mcr(int bits) {
  uint8_t m = 0;

  if (bits & FNU_TTYMODEM_DTR)
    m |= MCR_DTR;
  if (bits & FNU_TTYMODEM_RTS)
    m |= MCR_RTS;
  return m;
}

int uart_ioctl(int unit, uint64_t cmd, int *val) {
  struct com_softc *sc;
  int r = 0;

  if (unit < 0 || unit >= NCOM || !coms[unit].present)
    return -FNU_ERR_BADREQUEST;
  sc = &coms[unit];
  mtx_enter(&sc->lock);
  switch (cmd) {
  case FNU_TTYBREAK:
    sc->lcr |= LCR_SBREAK;
    wr(sc, com_lcr, sc->lcr);
    break;
  case FNU_TTYENDBREAK:
    sc->lcr &= (uint8_t)~LCR_SBREAK;
    wr(sc, com_lcr, sc->lcr);
    break;
  case FNU_TTYRAISEDTR:
    sc->mcr |= sc->dtr;
    wr(sc, com_mcr, sc->mcr);
    break;
  case FNU_TTYDROPDTR:
    sc->mcr &= (uint8_t)~sc->dtr;
    wr(sc, com_mcr, sc->mcr);
    break;
  case FNU_TTYSETLINES:
    sc->mcr &= (uint8_t)~(MCR_DTR | MCR_RTS);
    sc->mcr |= tiocm_to_mcr(*val);
    wr(sc, com_mcr, sc->mcr);
    break;
  case FNU_TTYRAISELINES:
    sc->mcr |= tiocm_to_mcr(*val);
    wr(sc, com_mcr, sc->mcr);
    break;
  case FNU_TTYDROPLINES:
    sc->mcr &= (uint8_t)~tiocm_to_mcr(*val);
    wr(sc, com_mcr, sc->mcr);
    break;
  case FNU_TTYGETLINES: {
    int bits = 0;

    if (sc->mcr & MCR_DTR)
      bits |= FNU_TTYMODEM_DTR;
    if (sc->mcr & MCR_RTS)
      bits |= FNU_TTYMODEM_RTS;
    if (sc->msr & MSR_DCD)
      bits |= FNU_TTYMODEM_CD;
    if (sc->msr & MSR_CTS)
      bits |= FNU_TTYMODEM_CTS;
    if (sc->msr & MSR_DSR)
      bits |= FNU_TTYMODEM_DSR;
    if (sc->msr & (MSR_RI | MSR_TERI))
      bits |= FNU_TTYMODEM_RI;
    if (rd(sc, com_ier))
      bits |= FNU_TTYMODEM_LE;
    *val = bits;
    break;
  }
  default:
    r = -FNU_ERR_BADREQUEST;
    break;
  }
  mtx_leave(&sc->lock);
  return r;
}

static int com_intr(struct com_softc *sc) {
  int worked = 0;

  for (;;) {
    int run[RXRUN];
    int n = 0, wake = 0, dcd = -1;
    uint8_t lsr, msr, delta;

    mtx_enter(&sc->lock);
    if (rd(sc, com_iir) & IIR_NOPEND) {
      mtx_leave(&sc->lock);
      return worked;
    }
    lsr = rd(sc, com_lsr);
    while ((lsr & LSR_RXRDY) && n < RXRUN) {
      int c = rd(sc, com_data);

      if (lsr & LSR_BI)
        c = TTY_FE;
      else {
        if (lsr & LSR_FE)
          c |= TTY_FE;
        if (lsr & LSR_PE)
          c |= TTY_PE;
      }
      run[n++] = c;
      lsr = rd(sc, com_lsr);
    }
    msr = rd(sc, com_msr);
    if (msr != sc->msr) {
      delta = (uint8_t)(msr ^ sc->msr);
      sc->msr = msr;
      if ((delta & msr & MSR_CTS) != 0)
        com_start(sc);
      if (delta & MSR_DCD)
        dcd = (msr & MSR_DCD) != 0;
    }
    if ((lsr & LSR_TXRDY) && sc->busy) {
      sc->busy = 0;
      com_start(sc);
      wake = 1;
    }
    mtx_leave(&sc->lock);
    if (wake) {
      wakeup(&sc->tx.r);
      console_serial_txdone(sc->unit);
    }
    if (n > 0)
      console_serial_input(sc->unit, run, n);
    if (dcd >= 0)
      console_serial_carrier(sc->unit, dcd);
    worked = 1;
  }
}

void uartintr(void) {
  int i;

  for (i = 0; i < NCOM; i++)
    if (coms[i].present)
      com_intr(&coms[i]);
}
