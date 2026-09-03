#include "defs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/stdarg.h"
#include "memlayout.h"
#include "proc.h"
#include "spinlock.h"
#include "trap.h"
#include "x86.h"

// ref. http://oswiki.osask.jp/?VGA
// ref. https://os.phil-opp.com/vga-text-mode/

static struct termios term = {.c_lflag = ICANON | ECHO};

static void consputc(int);

static int panicked = 0;

static struct {
  struct spinlock lock;
  int locking;
} cons;

static void printint(long xx, int base, int sign) {
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i;
  ulong x;

  if (sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);

  if (sign)
    buf[i++] = '-';

  while (--i >= 0)
    consputc(buf[i]);
}

// Print to the console. only understands %c, %d, %x, %p, %s.
void cprintf(char *fmt, ...) {
  int i, c, locking;
  void **argp;
  char *s;

  va_list va;
  char val_c;
  int val_d;
  long val_l;
  char *val_s;

  locking = cons.locking;
  if (locking) {
    acquire(&cons.lock);
  }

  if (fmt == 0) {
    panic("null fmt");
  }

  va_start(va, fmt);

  for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
    if (c != '%') {
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if (c == 0)
      break;
    switch (c) {
    case 'c':
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
      val_c = (char)va_arg(va, int);
      consputc(val_c);
#pragma GCC diagnostic pop
      break;
    case 'd':
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
      val_d = va_arg(va, int);
      printint((long)val_d, 10, 1);
#pragma GCC diagnostic pop
      break;
    case 'x':
    case 'p':
      val_l = va_arg(va, long);
      printint(val_l, 16, 0);
      break;
    case 's':
      val_s = va_arg(va, char *);
      if (val_s == 0) {
        val_s = "(null)";
      }
      for (; *val_s; val_s++) {
        consputc(*val_s);
      }
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  va_end(va);

  if (locking) {
    release(&cons.lock);
  }
}

void panic(char *s) {
  int i;
  uint pcs[10];

  cli();
  cons.locking = 0;
  // use lapiccpunum so that we can call panic from mycpu()
  cprintf("kernel panic! lapicid %d: panic: %s\n", lapicid(), s);
  panicked = 1; // freeze other CPU
  for (;;)
    ;
}

#define BACKSPACE 0x100
#define CRTPORT 0x3d4
/*
 * VGA text memory cannot be read back while VBE graphics mode is active:
 * QEMU returns 0xffff for its cells, which turns every framebuffer glyph
 * into a solid white block.  Keep a RAM shadow for the framebuffer console;
 * retain the hardware text buffer when VBE is unavailable.
 */
#define VGA_COLUMNS 80
#define VGA_ROWS 25
#define FB_MAX_COLUMNS 240
#define FB_MAX_ROWS 67

static ushort crt_shadow[FB_MAX_COLUMNS * FB_MAX_ROWS];
static ushort *crt = (ushort *)P2V(0xb8000); // CGA memory or RAM shadow
static int console_columns = VGA_COLUMNS;
static int console_rows = VGA_ROWS;
/*
 * The VGA CRTC cursor belongs to the legacy text-mode display.  Once VBE is
 * active, its value is a BIOS leftover and must not determine where the
 * framebuffer console starts drawing.
 */
static int console_pos;

// ANSI escape sequence state
static int ansi_state = 0;
static int ansi_params[8];
static int ansi_nparams = 0;
static ushort ansi_attr = 0x0700; // Default: grey on black
static int framebuffer_dirty_cell = -1;
static int framebuffer_cursor_cell = -1;
static int framebuffer_needs_redraw;

static void cgaputc(int c) {
  int pos;

  framebuffer_dirty_cell = -1;
  framebuffer_cursor_cell = -1;
  framebuffer_needs_redraw = 0;

  // Cursor position: col + console_columns*row.
  if (framebuffer_available()) {
    pos = console_pos;
  } else {
    outb(CRTPORT, 14);
    pos = inb(CRTPORT + 1) << 8;
    outb(CRTPORT, 15);
    pos |= inb(CRTPORT + 1);
  }

  if (ansi_state == 0) {
    if (c == 0x1b) { // ESC
      ansi_state = 1;
      return;
    }
  } else if (ansi_state == 1) {
    if (c == '[') {
      ansi_state = 2;
      ansi_nparams = 0;
      memset(ansi_params, 0, sizeof(ansi_params));
      return;
    } else {
      ansi_state = 0;
    }
  } else if (ansi_state == 2) {
    if (c >= '0' && c <= '9') {
      ansi_params[ansi_nparams] = ansi_params[ansi_nparams] * 10 + (c - '0');
      return;
    } else if (c == ';') {
      if (ansi_nparams < 7)
        ansi_nparams++;
      return;
    } else {
      ansi_nparams++; // Count the last parameter
      if (c == 'm') { // SGR - Select Graphic Rendition
        for (int i = 0; i < ansi_nparams; i++) {
          int p = ansi_params[i];
          if (p == 0) {
            ansi_attr = 0x0700; // Reset
          } else if (p == 7) {
            ansi_attr = 0x7000; // Inverse video (black on grey)
          } else if (p >= 30 && p <= 37) {
            // Foreground color
            static uchar ansi_to_vga[] = {0, 4, 2, 6, 1, 5, 3, 7};
            ansi_attr = (ansi_attr & 0xf000) | (ansi_to_vga[p - 30] << 8) | 0x0800; // Bold-ish
          }
        }
      } else if (c == 'J') { // ED - Erase in Display
        if (ansi_params[0] == 2) {
          // Clear entire screen
          for (int i = 0; i < console_columns * console_rows; i++)
            crt[i] = ' ' | ansi_attr;
          pos = 0;
          framebuffer_needs_redraw = 1;
        }
      } else if (c == 'H' || c == 'f') { // CUP or HVP - Cursor Position
        int row = ansi_params[0] ? ansi_params[0] - 1 : 0;
        int col = ansi_params[1] ? ansi_params[1] - 1 : 0;
        if (row < 0) row = 0;
        if (row >= console_rows) row = console_rows - 1;
        if (col < 0) col = 0;
        if (col >= console_columns) col = console_columns - 1;
        pos = row * console_columns + col;
      } else if (c == 'K') { // EL - Erase in Line
        int mode = ansi_params[0];
        if (mode == 0) { // Erase from cursor to end of line
          for (int i = pos; i < (pos / console_columns + 1) * console_columns; i++)
            crt[i] = ' ' | ansi_attr;
        } else if (mode == 1) { // Erase from start of line to cursor
          for (int i = (pos / console_columns) * console_columns; i <= pos; i++)
            crt[i] = ' ' | ansi_attr;
        } else if (mode == 2) { // Erase entire line
          for (int i = (pos / console_columns) * console_columns;
               i < (pos / console_columns + 1) * console_columns; i++)
            crt[i] = ' ' | ansi_attr;
        }
        framebuffer_needs_redraw = 1;
      }
      ansi_state = 0;
      goto update_cursor;
    }
  }

  if (c == '\n') {
    pos += console_columns - pos % console_columns;
  } else if (c == '\r') {
    pos -= pos % console_columns;
  } else if (c == BACKSPACE || c == '\b') {
    if (pos > 0) {
      --pos;
    }
  } else if (c >= ' ') {
    framebuffer_dirty_cell = pos;
    crt[pos++] = (c & 0xff) | ansi_attr;
  }

  if (pos < 0 || pos > console_rows * console_columns) {
    panic("pos under/overflow");
  }

  if ((pos / console_columns) >= console_rows) { // Scroll up.
    memmove(crt, crt + console_columns,
            sizeof(crt[0]) * (console_rows - 1) * console_columns);
    pos -= console_columns;
    memset(crt + pos, 0,
           sizeof(crt[0]) * (console_rows * console_columns - pos));
    // Fill with current attribute
    for (int i = pos; i < console_rows * console_columns; i++)
      crt[i] = ' ' | ansi_attr;
    framebuffer_needs_redraw = 1;
  }

update_cursor:
  if (framebuffer_available()) {
    console_pos = pos;
    /* crt[pos] below clears this cell, so redraw it as well. */
    framebuffer_cursor_cell = pos;
  } else {
    outb(CRTPORT, 14);
    outb(CRTPORT + 1, pos >> 8);
    outb(CRTPORT, 15);
    outb(CRTPORT + 1, pos);
  }
  crt[pos] = ' ' | ansi_attr;
}

void consputc(int c) {
  if (panicked) {
    cli();
    for (;;)
      ;
  }

  if (c == BACKSPACE) {
    uartputc('\b');
    uartputc(' ');
    uartputc('\b');
  } else {
    uartputc(c);
  }
  cgaputc(c);
  if (framebuffer_available()) {
    if (framebuffer_needs_redraw)
      framebuffer_redraw_cells(crt, console_columns * console_rows);
    else {
      if (framebuffer_dirty_cell >= 0)
        framebuffer_draw_cell(framebuffer_dirty_cell,
                              crt[framebuffer_dirty_cell]);
      if (framebuffer_cursor_cell >= 0 &&
          framebuffer_cursor_cell != framebuffer_dirty_cell)
        framebuffer_draw_cell(framebuffer_cursor_cell,
                              crt[framebuffer_cursor_cell]);
    }
  }
}

#define INPUT_BUF 128

struct {
  char buf[INPUT_BUF];
  uint r; // Read index
  uint w; // Write index
  uint e; // Edit index
} input;
static pid_t foreground_pid;

#define C(x) ((x) - '@') // Control-x

void console_set_foreground(pid_t pid) {
  acquire(&cons.lock);
  foreground_pid = pid;
  release(&cons.lock);
}

void consoleintr(int (*getc)(void)) {
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while ((c = getc()) >= 0) {
    switch (c) {
    case C('P'): // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'): // Kill line.
      while (input.e != input.w &&
             input.buf[(input.e - 1) % INPUT_BUF] != '\n') {
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('C'):
      while (input.e != input.w &&
             input.buf[(input.e - 1) % INPUT_BUF] != '\n') {
        input.e--;
        consputc(BACKSPACE);
      }
      consputc('^');
      consputc('C');
      consputc('\n');
      if (foreground_pid > 0)
        kill(foreground_pid);
      wakeup(&input.r);
      break;
    case C('H'):
    case '\x7f': // Backspace
      if (input.e != input.w) {
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    default:
      if (c != 0 && input.e - input.r < INPUT_BUF) {
        if (!(term.c_lflag & ICANON)) {
          input.buf[input.e++ % INPUT_BUF] = c;
          if (term.c_lflag & ECHO)
            consputc(c);
          else if (term.c_lflag & ECHOPASS)
            consputc('*');
          input.w = input.e;
          wakeup(&input.r);
        } else {
          c = (c == '\r') ? '\n' : c;
          input.buf[input.e++ % INPUT_BUF] = c;
          if (term.c_lflag & ECHOPASS && c != '\n')
            consputc('*');
          else if (term.c_lflag & ECHO || c == '\n')
            consputc(c);
          if (c == '\n' || c == C('D') || input.e == input.r + INPUT_BUF) {
            input.w = input.e;
            wakeup(&input.r);
          }
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if (doprocdump) {
    procdump(); // now call procdump() wo. cons.lock held
  }
}

int consoleread(struct inode *ip, char *dst, int n) {
  int target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while (n > 0) {
    while (input.r == input.w) {
      if (myproc()->killed) {
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if (term.c_lflag & ICANON) {
      if (c == C('D')) { // EOF
        if (n < target) {
          // Save ^D for next time, to make sure
          // caller gets a 0-byte result.
          input.r--;
        }
        break;
      }
      *dst++ = c;
      --n;
      if (c == '\n')
        break;
    } else {
      *dst++ = c;
      --n;
      break; // in non-canonical mode, return what we have
    }
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

int consoleioctl(struct inode *ip, uint64_t cmd, uint64_t arg) {
  struct proc *curproc = myproc();

  switch (cmd) {
  case TCGETS:
    if (copyout(curproc->pgdir, (uintptr_t)arg, &term, sizeof(term)) < 0)
      return -1;
    return 0;
  case TCSETS:
    if (copyin(curproc->pgdir, &term, (uintptr_t)arg, sizeof(term)) < 0)
      return -1;
    return 0;
  default:
    return -1;
  }
}

int consolewrite(struct inode *ip, char *buf, int n) {
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for (i = 0; i < n; i++)
    consputc(buf[i] & 0xff);
  release(&cons.lock);
  ilock(ip);

  return n;
}

void consoleinit(void) {
  initlock(&cons.lock, "console");
  if (framebuffer_available()) {
    crt = crt_shadow;
    console_columns = framebuffer_columns();
    console_rows = framebuffer_rows();
    console_pos = 0;
    if (console_columns > FB_MAX_COLUMNS || console_rows > FB_MAX_ROWS)
      panic("framebuffer console too large");
    for (int i = 0; i < console_columns * console_rows; i++)
      crt[i] = ' ' | ansi_attr;
    framebuffer_redraw_cells(crt, console_columns * console_rows);
  }

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}
