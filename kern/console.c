#include "defs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/signal.h"
#include "kbd.h"
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

// Print to the console. %c %d %u %x %p %s, and an l prefix for 64 bit.
void cprintf(char *fmt, ...) {
  int i, c, locking, lng;

  va_list va;
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
    lng = 0;
    if (c == 'l') {
      lng = 1;
      c = fmt[++i] & 0xff;
    }
    if (c == 0)
      break;
    switch (c) {
    case 'c':
      consputc_raw((char)va_arg(va, int));
      break;
    case 'd':
      printint(lng ? va_arg(va, long) : (long)va_arg(va, int), 10, 1);
      break;
    case 'u':
      printint(lng ? (long)va_arg(va, ulong) : (long)va_arg(va, uint), 10, 0);
      break;
    case 'x':
    case 'p':
      printint(va_arg(va, long), 16, 0);
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
      // unknown % sequence, printed to draw attention
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
// vga text memory cannot be read back in vbe graphics mode; qemu answers
// 0xffff. the framebuffer console keeps a ram shadow instead.
#define VGA_COLUMNS 80
#define VGA_ROWS 25

#define NVT KBD_VT_MAX
#define INPUT_BUF 128

// one virtual terminal: its own screen, cursor, escape state and input.
// the cell buffer follows the console geometry, as FreeBSD's vt_buf does.
struct vt {
  ushort *cells;
  int pos;
  int pending_scrolls;
  int ansi_state;
  int ansi_params[8];
  int ansi_nparams;
  ushort attr;
  uint32_t utf8_codepoint;
  int utf8_expected;
  pid_t fgpid;
  struct {
    char buf[INPUT_BUF];
    uint r, w, e;
  } in;
};

static struct vt vts[NVT];
static int active;
static int console_columns = VGA_COLUMNS;
static int console_rows = VGA_ROWS;

// the text adapter shows one screen, so an inactive terminal lives in ram only
static ushort *vga = (ushort *)P2V(0xb8000);

static int decode_utf8(struct vt *v, uchar b) {
  if (v->utf8_expected == 0) {
    if ((b & 0x80) == 0) {
      return b;
    } else if ((b & 0xE0) == 0xC0) {
      v->utf8_codepoint = b & 0x1F;
      v->utf8_expected = 1;
      return -1;
    } else if ((b & 0xF0) == 0xE0) {
      v->utf8_codepoint = b & 0x0F;
      v->utf8_expected = 2;
      return -1;
    } else if ((b & 0xF8) == 0xF0) {
      v->utf8_codepoint = b & 0x07;
      v->utf8_expected = 3;
      return -1;
    } else {
      return '?';
    }
  } else {
    if ((b & 0xC0) == 0x80) {
      v->utf8_codepoint = (v->utf8_codepoint << 6) | (b & 0x3F);
      v->utf8_expected--;
      if (v->utf8_expected == 0) {
        uint32_t cp = v->utf8_codepoint;
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
      v->utf8_expected = 0;
      return '?';
    }
  }
}

static void cgaputc(struct vt *v, int c) {
  int pos;

  // kernel messages start before consoleinit; the serial line already has them
  if (v->cells == 0)
    return;

  if (c != BACKSPACE && (c & ~0xff) == 0) {
    c = decode_utf8(v, (uchar)c);
    if (c < 0)
      return;
  }

  // Cursor position: col + console_columns*row.
  pos = v->pos;

  if (v->ansi_state == 0) {
    if (c == 0x1b) { // ESC
      v->ansi_state = 1;
      return;
    }
  } else if (v->ansi_state == 1) {
    if (c == '[') {
      v->ansi_state = 2;
      v->ansi_nparams = 0;
      memset(v->ansi_params, 0, sizeof(v->ansi_params));
      return;
    } else {
      v->ansi_state = 0;
    }
  } else if (v->ansi_state == 2) {
    if (c >= '0' && c <= '9') {
      v->ansi_params[v->ansi_nparams] =
          v->ansi_params[v->ansi_nparams] * 10 + (c - '0');
      return;
    } else if (c == ';') {
      if (v->ansi_nparams < 7)
        v->ansi_nparams++;
      return;
    } else {
      v->ansi_nparams++; // Count the last parameter
      if (c == 'm') { // SGR - Select Graphic Rendition
        for (int i = 0; i < v->ansi_nparams; i++) {
          int p = v->ansi_params[i];
          if (p == 0) {
            v->attr = 0x0700; // Reset
          } else if (p == 7) {
            v->attr = 0x7000; // Inverse video (black on grey)
          } else if (p >= 30 && p <= 37) {
            // Foreground color
            static uchar ansi_to_vga[] = {0, 4, 2, 6, 1, 5, 3, 7};
            v->attr = (v->attr & 0xf000) | (ansi_to_vga[p - 30] << 8) |
                      0x0800; // bold-ish
          }
        }
      } else if (c == 'J') { // ED - Erase in Display
        if (v->ansi_params[0] == 2) {
          // Clear entire screen
          for (int i = 0; i < console_columns * console_rows; i++)
            v->cells[i] = ' ' | v->attr;
          pos = 0;
          v->pending_scrolls = 0;
          if (framebuffer_available())
            framebuffer_clear();
        }
      } else if (c == 'H' || c == 'f') { // CUP or HVP - Cursor Position
        int row = v->ansi_params[0] ? v->ansi_params[0] - 1 : 0;
        int col = v->ansi_params[1] ? v->ansi_params[1] - 1 : 0;
        if (row < 0)
          row = 0;
        if (row >= console_rows)
          row = console_rows - 1;
        if (col < 0)
          col = 0;
        if (col >= console_columns)
          col = console_columns - 1;
        pos = row * console_columns + col;
      } else if (c == 'K') { // EL - Erase in Line
        int mode = v->ansi_params[0];
        if (mode == 0) { // Erase from cursor to end of line
          for (int i = pos; i < (pos / console_columns + 1) * console_columns;
               i++)
            v->cells[i] = ' ' | v->attr;
        } else if (mode == 1) { // Erase from start of line to cursor
          for (int i = (pos / console_columns) * console_columns; i <= pos; i++)
            v->cells[i] = ' ' | v->attr;
        } else if (mode == 2) { // Erase entire line
          for (int i = (pos / console_columns) * console_columns;
               i < (pos / console_columns + 1) * console_columns; i++)
            v->cells[i] = ' ' | v->attr;
        }
      }
      v->ansi_state = 0;
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
      v->cells[pos] = ' ' | v->attr;
    }
  } else if (c == '\b') {
    if (pos > 0) {
      --pos;
    }
  } else if (c >= ' ') {
    v->cells[pos++] = (c & 0xff) | v->attr;
  }

  if (pos < 0 || pos > console_rows * console_columns) {
    panic("pos under/overflow");
  }

  if ((pos / console_columns) >= console_rows) { // Scroll up.
    memmove(v->cells, v->cells + console_columns,
            sizeof(v->cells[0]) * (console_rows - 1) * console_columns);
    pos -= console_columns;
    for (int i = pos; i < console_rows * console_columns; i++)
      v->cells[i] = ' ' | v->attr;
    v->pending_scrolls++;
  }

update_cursor:
  v->pos = pos;
  if (!framebuffer_available() && v == &vts[active]) {
    outb(CRTPORT, 14);
    outb(CRTPORT + 1, pos >> 8);
    outb(CRTPORT, 15);
    outb(CRTPORT + 1, pos);
  }
}

static void vtputc(struct vt *v, int c) {
  if (panicked) {
    cli();
    for (;;)
      ;
  }

  // the serial line mirrors whatever the user is looking at
  if (v == &vts[active]) {
    if (c == BACKSPACE) {
      uartputc('\b');
      uartputc(' ');
      uartputc('\b');
    } else {
      uartputc(c);
    }
  }
  cgaputc(v, c);
}

// kernel messages land on the terminal in front of the user
static void consputc_raw(int c) { vtputc(&vts[active], c); }

static int console_dirty = 0;
static uint console_burst_start = 0;

// a parked processor never releases the console
void console_drop_lock(void) { cons.locking = 0; }

static void vt_flush(struct vt *v) {
  if (v != &vts[active] || v->cells == 0)
    return;
  if (framebuffer_available()) {
    if (v->pending_scrolls > 0) {
      framebuffer_scroll_lines(v->pending_scrolls, ' ' | v->attr);
      v->pending_scrolls = 0;
    }
    framebuffer_flush_cells(v->cells, console_columns * console_rows);
  } else {
    memmove(vga, v->cells, sizeof(ushort) * console_columns * console_rows);
  }
  console_dirty = 0;
  console_burst_start = ticks;
}

void console_flush(void) { vt_flush(&vts[active]); }

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

void console_set_foreground(int tty, pid_t pid) {
  acquire(&cons.lock);
  vts[tty >= 0 && tty < NVT ? tty : active].fgpid = pid;
  release(&cons.lock);
}

// repaint the whole screen from the terminal being switched to
static void vt_switch(int n) {
  struct vt *v;

  if (n < 0 || n >= NVT || n == active || vts[n].cells == 0)
    return;
  active = n;
  v = &vts[active];
  v->pending_scrolls = 0;
  if (framebuffer_available()) {
    framebuffer_redraw_cells(v->cells, console_columns * console_rows);
  } else {
    memmove(vga, v->cells, sizeof(ushort) * console_columns * console_rows);
    outb(CRTPORT, 14);
    outb(CRTPORT + 1, v->pos >> 8);
    outb(CRTPORT, 15);
    outb(CRTPORT + 1, v->pos);
  }
}

void consoleintr(int (*getc)(void)) {
  struct vt *v;
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while ((c = getc()) >= 0) {
    if (c >= KBD_VT_BASE && c < KBD_VT_BASE + NVT) {
      vt_switch(c - KBD_VT_BASE);
      continue;
    }
    v = &vts[active];
    if (!(term.c_lflag & ICANON)) {
      if (c != 0 && v->in.e - v->in.r < INPUT_BUF) {
        v->in.buf[v->in.e++ % INPUT_BUF] = c;
        if (term.c_lflag & ECHO)
          vtputc(v, c);
        else if (term.c_lflag & ECHOPASS)
          vtputc(v, '*');
        v->in.w = v->in.e;
        wakeup(&v->in.r);
      }
      continue;
    }

    switch (c) {
    case C('P'): // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'): // Kill line.
      while (v->in.e != v->in.w &&
             v->in.buf[(v->in.e - 1) % INPUT_BUF] != '\n') {
        v->in.e--;
        vtputc(v, BACKSPACE);
      }
      break;
    case C('C'):
      while (v->in.e != v->in.w &&
             v->in.buf[(v->in.e - 1) % INPUT_BUF] != '\n') {
        v->in.e--;
        vtputc(v, BACKSPACE);
      }
      vtputc(v, '^');
      vtputc(v, 'C');
      vtputc(v, '\n');
      if (v->fgpid > 0)
        kill(v->fgpid, SIGINT);
      wakeup(&v->in.r);
      break;
    case C('H'):
    case '\x7f': // Backspace
      if (v->in.e != v->in.w) {
        v->in.e--;
        vtputc(v, BACKSPACE);
      }
      break;
    default:
      if (c != 0 && v->in.e - v->in.r < INPUT_BUF) {
        c = (c == '\r') ? '\n' : c;
        v->in.buf[v->in.e++ % INPUT_BUF] = c;
        if (term.c_lflag & ECHOPASS && c != '\n')
          vtputc(v, '*');
        else if (term.c_lflag & ECHO || c == '\n')
          vtputc(v, c);
        if (c == '\n' || c == C('D') || v->in.e == v->in.r + INPUT_BUF) {
          v->in.w = v->in.e;
          wakeup(&v->in.r);
        }
      }
      break;
    }
  }
  vt_flush(&vts[active]);
  release(&cons.lock);
  if (doprocdump) {
    procdump(); // now call procdump() wo. cons.lock held
  }
}

// minor 0..NVT-1 is /dev/ttyN; anything else is /dev/console, the terminal
// the user is actually looking at
static struct vt *vt_of(struct inode *ip) {
  return (ip->minor >= 0 && ip->minor < NVT) ? &vts[ip->minor] : &vts[active];
}

int consoleread(struct inode *ip, char *dst, int n) {
  struct vt *v = vt_of(ip);
  int target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  if (console_dirty) {
    vt_flush(v);
  }
  while (n > 0) {
    while (v->in.r == v->in.w) {
      if (proc_interrupted()) {
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      sleep(&v->in.r, &cons.lock);
    }
    c = v->in.buf[v->in.r++ % INPUT_BUF];
    if (term.c_lflag & ICANON) {
      if (c == C('D')) { // EOF
        if (n < target) {
          // Save ^D for next time, to make sure
          // caller gets a 0-byte result.
          v->in.r--;
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
  case FBIODUMPMODES:
    display_dump_modes();
    return 0;
  case FBIOGET_MODELIST: {
    static struct fb_modelist list;
    int n;
    // display_mode_list talks to the video bios and takes no console lock
    n = display_mode_list(&list);
    if (n < 0)
      return -1;
    if (copyout(curproc->pgdir, (uintptr_t)arg, &list, sizeof(list)) < 0)
      return -1;
    return n;
  }
  default:
    return -1;
  }
}

int consolewrite(struct inode *ip, char *buf, int n) {
  struct vt *v = vt_of(ip);
  int i;

  iunlock(ip);
  acquire(&cons.lock);
  for (i = 0; i < n; i++)
    vtputc(v, buf[i] & 0xff);

  vt_flush(v);
  release(&cons.lock);
  ilock(ip);

  return n;
}

// resize every terminal to the geometry now in force and blank it
static int vt_reset_all(void) {
  uint64_t bytes = (uint64_t)console_columns * console_rows * sizeof(ushort);
  int n, i;

  for (n = 0; n < NVT; n++) {
    struct vt *v = &vts[n];
    if (v->cells != 0)
      vmem_free(v->cells);
    v->cells = vmem_alloc(bytes);
    if (v->cells == 0)
      return -1;
    v->attr = 0x0700;
    v->pos = 0;
    v->pending_scrolls = 0;
    v->ansi_state = 0;
    v->utf8_expected = 0;
    for (i = 0; i < console_columns * console_rows; i++)
      v->cells[i] = ' ' | v->attr;
  }
  return 0;
}

void consoleinit(void) {
  initlock(&cons.lock, "console");
  if (framebuffer_available()) {
    console_columns = framebuffer_columns();
    console_rows = framebuffer_rows();
  }
  if (vt_reset_all() < 0)
    panic("console: no memory for terminals");
  if (framebuffer_available())
    framebuffer_redraw_cells(vts[active].cells, console_columns * console_rows);

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}

void console_switch_to_gpu(void) {
  acquire(&cons.lock);
  if (framebuffer_available()) {
    console_columns = framebuffer_columns();
    console_rows = framebuffer_rows();
    console_dirty = 0;
    if (vt_reset_all() == 0)
      framebuffer_redraw_cells(vts[active].cells,
                               console_columns * console_rows);
  }
  release(&cons.lock);
}
