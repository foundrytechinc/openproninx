#include "defs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/stdarg.h"
#include "memlayout.h"
#include "proc.h"
#include "spinlock.h"
#include "trap.h"
#include "x86.h"


static struct termios term = {.c_lflag = ICANON | ECHO};

static void consputc_raw(int);
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
    consputc_raw(buf[i]);
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
      consputc_raw(c);
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
      consputc_raw(val_c);
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
        consputc_raw(*val_s);
      }
      break;
    case '%':
      consputc_raw('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc_raw('%');
      consputc_raw(c);
      break;
    }
  }

  va_end(va);
  console_flush();

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
#define FB_MAX_COLUMNS 320
#define FB_MAX_ROWS 100

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
static int console_pending_scrolls = 0;

// ANSI escape sequence state
static int ansi_state = 0;
static int ansi_params[8];
static int ansi_nparams = 0;
static ushort ansi_attr = 0x0700; // Default: grey on black


static uint32_t utf8_codepoint = 0;
static int utf8_expected = 0;

static int decode_utf8(uchar b) {
  if (utf8_expected == 0) {
    if ((b & 0x80) == 0) {
      return b;
    } else if ((b & 0xE0) == 0xC0) {
      utf8_codepoint = b & 0x1F;
      utf8_expected = 1;
      return -1;
    } else if ((b & 0xF0) == 0xE0) {
      utf8_codepoint = b & 0x0F;
      utf8_expected = 2;
      return -1;
    } else if ((b & 0xF8) == 0xF0) {
      utf8_codepoint = b & 0x07;
      utf8_expected = 3;
      return -1;
    } else {
      return '?';
    }
  } else {
    if ((b & 0xC0) == 0x80) {
      utf8_codepoint = (utf8_codepoint << 6) | (b & 0x3F);
      utf8_expected--;
      if (utf8_expected == 0) {
        uint32_t cp = utf8_codepoint;
        if (cp == 0x2014 || cp == 0x2013 || cp == 0x2015 || cp == 0x2212)
          return '-';
        if (cp == 0x2018 || cp == 0x2019)
          return '\'';
        if (cp == 0x201C || cp == 0x201D)
          return '"';
        if (cp == 0x2022 || cp == 0x00B7)
          return '*';
        if (cp == 0x00A0)
          return ' ';
        if (cp == 0x2026)
          return '.';
        if (cp < 128)
          return (int)cp;
        return '?';
      }
      return -1;
    } else {
      utf8_expected = 0;
      return '?';
    }
  }
}

static void cgaputc(int c) {
  int pos;

  if (c != BACKSPACE && (c & ~0xff) == 0) {
    c = decode_utf8((uchar)c);
    if (c < 0)
      return;
  }

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
          console_pending_scrolls = 0;
          if (framebuffer_available())
            framebuffer_clear();
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
      }
      ansi_state = 0;
      goto update_cursor;
    }
  }

  if (c == '\n') {
    pos += console_columns - pos % console_columns;
  } else if (c == '\r') {
    pos -= pos % console_columns;
  } else if (c == BACKSPACE) {
    if (pos > 0) {
      --pos;
      crt[pos] = ' ' | ansi_attr;
    }
  } else if (c == '\b') {
    if (pos > 0) {
      --pos;
    }
  } else if (c >= ' ') {
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
    console_pending_scrolls++;
  }

update_cursor:
  if (framebuffer_available()) {
    console_pos = pos;
  } else {
    outb(CRTPORT, 14);
    outb(CRTPORT + 1, pos >> 8);
    outb(CRTPORT, 15);
    outb(CRTPORT + 1, pos);
  }
}

static void consputc_raw(int c) {
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
}

static int console_dirty = 0;
static uint console_burst_start = 0;

void console_flush(void) {
  if (framebuffer_available()) {
    if (console_pending_scrolls > 0) {
      framebuffer_scroll_lines(console_pending_scrolls, ' ' | ansi_attr);
      console_pending_scrolls = 0;
    }
    framebuffer_flush_cells(crt, console_columns * console_rows);
  }
  console_dirty = 0;
  console_burst_start = ticks;
}

void console_flush_if_dirty(void) {
  if (!console_dirty || cons.lock.locked)
    return;
  acquire(&cons.lock);
  if (console_dirty) {
    console_flush();
  }
  release(&cons.lock);
}

void consputc(int c) {
  consputc_raw(c);
  console_flush();
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
    if (!(term.c_lflag & ICANON)) {
      if (c != 0 && input.e - input.r < INPUT_BUF) {
        input.buf[input.e++ % INPUT_BUF] = c;
        if (term.c_lflag & ECHO)
          consputc_raw(c);
        else if (term.c_lflag & ECHOPASS)
          consputc_raw('*');
        input.w = input.e;
        wakeup(&input.r);
      }
      continue;
    }

    switch (c) {
    case C('P'): // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'): // Kill line.
      while (input.e != input.w &&
             input.buf[(input.e - 1) % INPUT_BUF] != '\n') {
        input.e--;
        consputc_raw(BACKSPACE);
      }
      break;
    case C('C'):
      while (input.e != input.w &&
             input.buf[(input.e - 1) % INPUT_BUF] != '\n') {
        input.e--;
        consputc_raw(BACKSPACE);
      }
      consputc_raw('^');
      consputc_raw('C');
      consputc_raw('\n');
      if (foreground_pid > 0)
        kill(foreground_pid);
      wakeup(&input.r);
      break;
    case C('H'):
    case '\x7f': // Backspace
      if (input.e != input.w) {
        input.e--;
        consputc_raw(BACKSPACE);
      }
      break;
    default:
      if (c != 0 && input.e - input.r < INPUT_BUF) {
        c = (c == '\r') ? '\n' : c;
        input.buf[input.e++ % INPUT_BUF] = c;
        if (term.c_lflag & ECHOPASS && c != '\n')
          consputc_raw('*');
        else if (term.c_lflag & ECHO || c == '\n')
          consputc_raw(c);
        if (c == '\n' || c == C('D') || input.e == input.r + INPUT_BUF) {
          input.w = input.e;
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  console_flush();
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
  if (console_dirty) {
    console_flush();
  }
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
  case FBIOGET_VSCREENINFO: {
    struct fb_var_screeninfo vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    display_get_resolution(&vinfo.xres, &vinfo.yres, &vinfo.bits_per_pixel);
    if (copyout(curproc->pgdir, (uintptr_t)arg, &vinfo, sizeof(vinfo)) < 0)
      return -1;
    return 0;
  }
  case FBIOPUT_VSCREENINFO: {
    struct fb_var_screeninfo vinfo;
    if (copyin(curproc->pgdir, &vinfo, (uintptr_t)arg, sizeof(vinfo)) < 0)
      return -1;
    return display_set_resolution(vinfo.xres, vinfo.yres);
  }
  default:
    return -1;
  }
}

int consolewrite(struct inode *ip, char *buf, int n) {
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for (i = 0; i < n; i++)
    consputc_raw(buf[i] & 0xff);

  console_flush();
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

void console_switch_to_gpu(void) {
  acquire(&cons.lock);
  if (framebuffer_available()) {
    crt = crt_shadow;
    console_columns = framebuffer_columns();
    console_rows = framebuffer_rows();
    console_pos = 0;
    console_pending_scrolls = 0;
    console_dirty = 0;
    if (console_columns > FB_MAX_COLUMNS || console_rows > FB_MAX_ROWS)
      panic("framebuffer console too large");
    for (int i = 0; i < console_columns * console_rows; i++)
      crt[i] = ' ' | ansi_attr;
    framebuffer_redraw_cells(crt, console_columns * console_rows);
  }
  release(&cons.lock);
}
