#include "atkbdcreg.h"
#include "defs.h"
#include "mmu.h"
#include "uio.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/bootinfo.h"
#include "inc/open.h"
#include "inc/signal.h"
#include "devfs.h"
#include "inc/stat.h"
#include "kbdreg.h"
#include "inc/stdarg.h"
#include "memlayout.h"
#include "malloc.h"
#include "proc.h"
#include "mutex.h"
#include "teken.h"
#include "trap.h"
#include "tty.h"
#include "virtio_gpu.h"
#include "x86.h"

static void consputc_raw(int);

static volatile int panicked;

static struct {
  int locking;
} cons;

static void cons_put(int c, void *arg __unused) {
  consputc_raw(c);
}

int vprintf(const char *fmt, va_list ap) {
  int locking, n;

  if (fmt == 0)
    panic("null fmt");
  locking = cons.locking;
  if (locking)
    mtx_enter(&tty_lock);
  n = kvprintf(fmt, cons_put, 0, ap);
  console_said += (uint64_t)n;
  console_flush();
  if (locking)
    mtx_leave(&tty_lock);
  return n;
}

int printf(const char *fmt, ...) {
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

void panic(const char *fmt, ...) {
  char s[192];
  va_list ap;

  uint64_t *rbp;
  int i, me, again;

  cli();
  me = lapicid() + 1;
  // one cpu panics, the rest stop
  again = 0;
  if (!__sync_bool_compare_and_swap(&panicked, 0, me)) {
    if (panicked != me) {
      curcpu()->halted = 1;
      for (;;)
        hlt();
    }
    again = 1;
  }
  cons.locking = 0;
  if (!again)
    lapic_halt_others();
  va_start(ap, fmt);
  vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  printf("kernel panic! lapicid %d: panic: %s%s\n", me - 1, s, again ? " (recursive)" : "");
  if (again)
    machine_restart();
  __asm __volatile("mov %%rbp,%0" : "=r"(rbp));
  for (i = 0; i < 12 && ((uintptr_t)rbp >> 63) != 0; i++) {
    printf("  [%d] 0x%lx\n", i, rbp[1]);
    rbp = (uint64_t *)rbp[0];
  }
  console_flush();
  panicstr = fmt;
  msgbuf_dump(1);
  shutdown_panic();
  machine_restart();
}

// cncheckc: polled, at panic
int cncheckc(void) {
  int c;

  if ((c = comcncheckc()) >= 0)
    return c;
  if ((c = atkbd_cncheckc()) >= 0)
    return c;
  return ukbd_cncheckc();
}

#define BACKSPACE 0x100
#define CRTPORT 0x3d4
#define VGA_COLUMNS 80
#define VGA_ROWS 25

#define NVT 4
#define C(x) ((x) - '@')

#define HISTORY_ROWS 1000

struct vt {
  vtcell_t *cells;
  teken_t te;
  int te_ready;
  int pos;
  int pending_scrolls;
  int cursor_hidden;           // DECTCEM
  int mark_start, mark_end, mark_last;
  int mouse_level;             // DECSET 1000
  vtcell_t *hist;
  int hist_tail, hist_len;
  int view_off;
  int scrolling;
  int kbdstate;
  struct tty tty;
  int opens;
  int starting; // its output being drained
  int ser;
  int pty;
  int master_open;
  int slave_seen;
  int unit;
  struct selinfo psel; // the master's reader
};

#define NSER 4
#define NFIXED (NVT + NSER)
#define PTYCHUNK 8
#define MAXPTYS 992

static int consvt;

static struct vt vts[NFIXED];

static struct vt **ptychunks;
static int nptys, maxptys;

static struct vt *tty_vt(int t) {
  int k = t - NFIXED, n = __atomic_load_n(&nptys, __ATOMIC_ACQUIRE);

  if (t >= 0 && t < NFIXED)
    return &vts[t];
  return k >= 0 && k < n ? &ptychunks[k / PTYCHUNK][k % PTYCHUNK] : 0;
}

static int ntty(void) {
  return NFIXED + __atomic_load_n(&nptys, __ATOMIC_ACQUIRE);
}
static int active;
static int console_columns = VGA_COLUMNS;
static int console_rows = VGA_ROWS;

// THE MOUSE
static struct {
  int x, y, shown, buttons;
  vtcell_t *view;
  vtcell_t *snap;
  char *paste;
  int paste_len;
} mouse;
#define MARK_NONE 0
#define MARK_END 1
#define MARK_START 2
#define MARK_WORD 3
#define MARK_ROW 4
#define MARK_MOVE 6

static ushort *vga = (ushort *)P2V(0xb8000);

static void vga_show(const vtcell_t *cells, int n) {
  int i;

  for (i = 0; i < n; i++)
    vga[i] = (ushort)(cp437_of(CELL_CP(cells[i])) |
                      ((ushort)CELL_ATTR(cells[i]) << 8));
}

// THE TERMINAL

static int console_dirty;

static int put_utf8(char *out, uint32_t cp);
static void mouse_paste(struct vt *v);

// ANSI
static const uchar ansi_to_vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

#define VGA_COLOR(c) (ansi_to_vga[(c) & 7] | ((c) & 8))

static uchar vt_attr_of(const teken_attr_t *a) {
  int fg = a->ta_fgcolor, bg = a->ta_bgcolor, t;

  if (a->ta_format & TF_BOLD)
    fg |= 8;
  if (a->ta_format & TF_BLINK)
    bg |= 8;
  if (a->ta_format & TF_REVERSE) {
    t = fg;
    fg = bg;
    bg = t;
  }
  return (uchar)((VGA_COLOR(bg) << 4) | VGA_COLOR(fg));
}

static void vt_history_add(struct vt *v, int n) {
  int i;

  if (v->hist == 0)
    return;
  for (i = 0; i < n; i++) {
    memmove(v->hist + (size_t)v->hist_tail * console_columns,
            v->cells + (size_t)i * console_columns,
            sizeof(vtcell_t) * console_columns);
    v->hist_tail = (v->hist_tail + 1) % HISTORY_ROWS;
    if (v->hist_len < HISTORY_ROWS)
      v->hist_len++;
  }
  if (v->view_off > 0) {
    v->view_off += n;
    if (v->view_off > v->hist_len)
      v->view_off = v->hist_len;
  }
}

static int in_screen(const teken_pos_t *p) {
  return p->tp_row < console_rows && p->tp_col < console_columns;
}

static void tf_bell(void *s __unused) {}

static void tf_cursor(void *s, const teken_pos_t *p) {
  struct vt *v = s;

  if (v->te_ready && in_screen(p))
    v->pos = p->tp_row * console_columns + p->tp_col;
}

static void tf_putchar(void *s, const teken_pos_t *p, teken_char_t c,
                       const teken_attr_t *a) {
  struct vt *v = s;

  if (!v->te_ready || !in_screen(p))
    return;
  v->cells[p->tp_row * console_columns + p->tp_col] =
      CELL((a->ta_format & TF_CJK_RIGHT) ? ' ' : c, ATTR(vt_attr_of(a)));
}

static void tf_fill(void *s, const teken_rect_t *r, teken_char_t c,
                    const teken_attr_t *a) {
  struct vt *v = s;
  vtcell_t cell = CELL(c, ATTR(vt_attr_of(a)));
  int row, col;

  if (!v->te_ready)
    return;
  for (row = r->tr_begin.tp_row; row < r->tr_end.tp_row && row < console_rows; row++)
    for (col = r->tr_begin.tp_col; col < r->tr_end.tp_col && col < console_columns; col++)
      v->cells[row * console_columns + col] = cell;
}

static void tf_copy(void *s, const teken_rect_t *r, const teken_pos_t *p) {
  struct vt *v = s;
  int rows = r->tr_end.tp_row - r->tr_begin.tp_row;
  int cols = r->tr_end.tp_col - r->tr_begin.tp_col;
  int rdiff = (int)r->tr_begin.tp_row - (int)p->tp_row, row;

  if (!v->te_ready || r->tr_end.tp_row > console_rows ||
      r->tr_end.tp_col > console_columns || !in_screen(p))
    return;
  if (rdiff > 0 && r->tr_begin.tp_col == 0 && r->tr_end.tp_col == console_columns &&
      rows + rdiff == console_rows) {
    vt_history_add(v, rdiff);
    v->pending_scrolls += rdiff;
    v->mark_start = v->mark_end = 0;
  }
  if (p->tp_row < r->tr_begin.tp_row) {
    for (row = 0; row < rows; row++)
      memmove(v->cells + (p->tp_row + row) * console_columns + p->tp_col,
              v->cells + (r->tr_begin.tp_row + row) * console_columns +
                  r->tr_begin.tp_col,
              sizeof(vtcell_t) * cols);
  } else {
    for (row = rows - 1; row >= 0; row--)
      memmove(v->cells + (p->tp_row + row) * console_columns + p->tp_col,
              v->cells + (r->tr_begin.tp_row + row) * console_columns +
                  r->tr_begin.tp_col,
              sizeof(vtcell_t) * cols);
  }
}

static void tf_nothing(void *s __unused) {}

static void tf_param(void *s, int cmd, unsigned int arg) {
  struct vt *v = s;

  switch (cmd) {
  case TP_SETLOCALCURSOR:
    v->cursor_hidden = arg == 1;
    break;
  case TP_SHOWCURSOR:
    v->cursor_hidden = arg == 0;
    break;
  case TP_MOUSE:
    v->mouse_level = (int)arg;
    break;
  }
}

static void tf_respond(void *s, const void *buf, size_t len) {
  struct vt *v = s;
  const uchar *p = buf;
  size_t i;

  for (i = 0; i < len; i++)
    ttyinput(p[i], &v->tty);
}

static const teken_funcs_t vt_funcs = {
    .tf_bell = tf_bell,
    .tf_cursor = tf_cursor,
    .tf_putchar = tf_putchar,
    .tf_fill = tf_fill,
    .tf_copy = tf_copy,
    .tf_pre_input = tf_nothing,
    .tf_post_input = tf_nothing,
    .tf_param = tf_param,
    .tf_respond = tf_respond,
};

static void vt_place(struct vt *v) {
  if (v == &vts[active]) {
    console_dirty = 1;
    if (!framebuffer_available()) {
      outb(CRTPORT, 14);
      outb(CRTPORT + 1, v->pos >> 8);
      outb(CRTPORT, 15);
      outb(CRTPORT + 1, v->pos);
    }
  }
}

static int put_dec(char *out, int v) {
  char tmp[12];
  int n = 0, k = 0;

  do {
    tmp[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (k > 0)
    out[n++] = tmp[--k];
  return n;
}

static void cgaputc(struct vt *v, int c) {
  uchar b;

  if (v->cells == 0 || !v->te_ready)
    return;
  if (c == BACKSPACE) {
    const teken_pos_t *cp = teken_get_cursor(&v->te);

    if (cp->tp_col == 0 && cp->tp_row > 0) {
      char seq[24];
      int n = 0, row = cp->tp_row - 1;

      v->cells[row * console_columns + console_columns - 1] =
          CELL(' ', ATTR(vt_attr_of(teken_get_curattr(&v->te))));
      seq[n++] = 0x1b;
      seq[n++] = '[';
      n += put_dec(seq + n, row + 1);
      seq[n++] = ';';
      n += put_dec(seq + n, console_columns);
      seq[n++] = 'H';
      teken_input(&v->te, seq, (size_t)n);
    } else {
      teken_input(&v->te, "\b \b", 3);
    }
  } else {
    b = (uchar)c;
    teken_input(&v->te, &b, 1);
  }
  vt_place(v);
}

// the cells the view shows
static const vtcell_t *vt_scrolled(struct vt *v) {
  int r, off = v->view_off;

  if (off == 0 || mouse.snap == 0 || v->hist == 0)
    return v->cells;
  for (r = 0; r < console_rows; r++) {
    int src = r - off;
    const vtcell_t *from;

    if (src >= 0) {
      from = v->cells + (size_t)src * console_columns;
    } else {
      int idx = v->hist_len + src;

      from = v->hist + (size_t)((v->hist_tail - v->hist_len + idx + HISTORY_ROWS) %
                                HISTORY_ROWS) * console_columns;
    }
    memmove(mouse.snap + (size_t)r * console_columns, from,
            sizeof(vtcell_t) * console_columns);
  }
  return mouse.snap;
}

static void vt_scroll(struct vt *v, int off, int whence) {
  int n = v->view_off;

  if (whence == 0)
    n = v->hist_len - off;
  else if (whence == 1)
    n -= off;
  else
    n = 0;
  if (n < 0)
    n = 0;
  if (n > v->hist_len)
    n = v->hist_len;
  if (n != v->view_off) {
    v->view_off = n;
    v->mark_start = v->mark_end = 0;
    v->pending_scrolls = 0;
    console_dirty = 1;
  }
}

static void vtputc(struct vt *v, int c) {
  if (panicked && panicked != lapicid() + 1) {
    cli();
    curcpu()->halted = 1;
    for (;;)
      ;
  }
  cgaputc(v, c);
}

static void consputc_raw(int c) {
  msgbuf_putc(c);
  if (c == '\n')
    comcnputc('\r');
  if (c == BACKSPACE) {
    comcnputc('\b');
    comcnputc(' ');
    comcnputc('\b');
  } else {
    comcnputc(c);
  }
  // VT_CONSWINDOW
  if (c == '\n')
    vtputc(&vts[0], '\r');
  vtputc(&vts[0], c);
}

void console_drop_lock(void) { cons.locking = 0; }

int log_enter(void) {
  int locking = cons.locking;

  if (locking)
    mtx_enter(&tty_lock);
  return locking;
}

void log_leave(int locking) {
  if (locking)
    mtx_leave(&tty_lock);
}

static const vtcell_t *vt_view(struct vt *v) {
  int n = console_columns * console_rows, s, e, i;
  const vtcell_t *src = vt_scrolled(v);

  if (v->mark_start == v->mark_end || mouse.view == 0)
    return src;
  s = v->mark_start < v->mark_end ? v->mark_start : v->mark_end;
  e = v->mark_start < v->mark_end ? v->mark_end : v->mark_start;
  memmove(mouse.view, src, n * sizeof(vtcell_t));
  for (i = s; i < e && i < n; i++) {
    uchar a = CELL_ATTR(src[i]);
    mouse.view[i] = CELL(src[i], ATTR((a << 4) | (a >> 4)));
  }
  return mouse.view;
}

static void vt_flush(struct vt *v) {
  if (v != &vts[active] || v->cells == 0)
    return;
  if (framebuffer_available()) {
    if (v->pending_scrolls > 0) {
      if (v->view_off == 0)
        framebuffer_scroll_lines(
            v->pending_scrolls,
            CELL(' ', ATTR(CELL_ATTR(v->cells[(console_rows - 1) * console_columns]))));
      v->pending_scrolls = 0;
    }
    framebuffer_pointer(mouse.x, mouse.y, mouse.shown);
    framebuffer_flush_cells(vt_view(v), console_columns * console_rows,
                            v->cursor_hidden || v->view_off ? -1 : v->pos);
  } else {
    vga_show(vt_view(v), console_columns * console_rows);
  }
  console_dirty = 0;
}

void console_flush(void) { vt_flush(&vts[active]); }

void console_redraw(void) {
  mtx_enter(&tty_lock);
  console_flush();
  mtx_leave(&tty_lock);
}

void console_flush_if_dirty(void) {
  if (!console_dirty || mtx_locked(&tty_lock))
    return;
  mtx_enter(&tty_lock);
  if (console_dirty) {
    console_flush();
  }
  mtx_leave(&tty_lock);
}

// THE REPAINT
#define CONSOLE_REFRESH_HZ 25

void console_refresh_thread(void *arg __unused) {
  static int refresh_chan;

  for (;;) {
    tsleep(&refresh_chan, PUSER, "vtrefresh", HZ / CONSOLE_REFRESH_HZ);
    console_flush_if_dirty();
  }
}


static int consolepoll(struct inode *ip, int want) {
  struct vt *v = ip->minor < 0 ? &vts[consvt] : tty_vt(ip->minor);
  int ready = 0;

  if (v == 0)
    return 0;
  if ((want & FNU_SEL_READ) && tty_readable(&v->tty))
    ready |= FNU_SEL_READ;
  if ((want & FNU_SEL_WRITE) && tty_writable(&v->tty))
    ready |= FNU_SEL_WRITE;
  return ready;
}

pid_t console_leave_session(pid_t sid) {
  pid_t fg = 0, f;
  int i;

  for (i = 0; i < ntty(); i++)
    if ((f = tty_leave_session(&tty_vt(i)->tty, sid)) != 0 && fg == 0)
      fg = f;
  return fg;
}

static void vt_switch(int n) {
  struct vt *v;

  if (n < 0 || n >= NVT || n == active || vts[n].cells == 0)
    return;
  vts[active].kbdstate = kbd_lockstate();
  active = n;
  v = &vts[active];
  kbd_set_lockstate(v->kbdstate);
  v->pending_scrolls = 0;
  if (framebuffer_available()) {
    framebuffer_redraw_cells(vt_scrolled(v), console_columns * console_rows);
  } else {
    vga_show(vt_scrolled(v), console_columns * console_rows);
    outb(CRTPORT, 14);
    outb(CRTPORT + 1, v->pos >> 8);
    outb(CRTPORT, 15);
    outb(CRTPORT + 1, v->pos);
  }
}

// tty_lock held
static void input_char(struct vt *v, int c) { ttyinput(c, &v->tty); }

int cngets(char *buf, int size) {
  struct fnu_ttymode saved;
  struct tty *tp;
  int n = 0, c;

  mtx_enter(&tty_lock);
  tp = &vts[active].tty;
  saved = tp->t_mode;
  tp->t_mode.edit &= ~(FNU_TTYEDIT_LINES | FNU_TTYEDIT_ECHO | FNU_TTYEDIT_SIGNALS |
                       FNU_TTYEDIT_EXTENDED);
  tp->t_mode.in &= ~FNU_TTYIN_FLOWOUT;
  vt_flush(&vts[active]);
  for (;;) {
    while ((c = tty_getc(&tp->t_rawq)) < 0)
      msleep(&tp->t_rawq, &tty_lock, PZERO, "cngets", 0);
    c &= TTY_CHARMASK;
    mtx_leave(&tty_lock);
    if (c == '\n' || c == '\r') {
      printf("\n");
      mtx_enter(&tty_lock);
      break;
    }
    if (c == '\b' || c == 0x7f) {
      if (n > 0) {
        n--;
        printf("\b \b");
      }
    } else if (c == ('u' & 037)) {
      for (; n > 0; n--)
        printf("\b \b");
    } else if (c >= ' ' && c < 0x7f && n < size - 1) {
      buf[n++] = (char)c;
      printf("%c", c);
    }
    mtx_enter(&tty_lock);
  }
  buf[n] = 0;
  tp->t_mode = saved;
  mtx_leave(&tty_lock);
  return n;
}

void console_serial_input(int unit, const int *chars, int n) {
  struct vt *v;
  int i;

  if (unit < 0 || unit >= NSER)
    return;
  v = &vts[NVT + unit];
  mtx_enter(&tty_lock);
  for (i = 0; i < n; i++)
    ttyinput(chars[i], &v->tty);
  mtx_leave(&tty_lock);
}

void console_serial_carrier(int unit, int on) {
  int dtr = FNU_TTYMODEM_DTR;

  if (unit < 0 || unit >= NSER)
    return;
  if (!ttymodem(&vts[NVT + unit].tty, on))
    uart_ioctl(unit, FNU_TTYDROPLINES, &dtr);
}

static void ser_start(struct tty *tp);

void console_serial_txdone(int unit) {
  struct tty *tp;

  if (unit < 0 || unit >= NSER)
    return;
  tp = &vts[NVT + unit].tty;
  mtx_enter(&tty_lock);
  if (uart_outq(unit) == 0)
    tp->t_state &= ~TS_BUSY;
  ser_start(tp);
  mtx_leave(&tty_lock);
}

// THE KEYS

static void input_string(struct vt *v, const char *s) {
  for (; *s; s++)
    input_char(v, (uchar)*s);
}

// a key's sequence, from teken
static const char *key_sequence(struct vt *v, int fn) {
  if (fn >= 1 && fn <= 12)
    return teken_get_sequence(&v->te, TKEY_F1 + fn - 1);
  switch (fn) {
  case 49: return teken_get_sequence(&v->te, TKEY_HOME);
  case 50: return teken_get_sequence(&v->te, TKEY_UP);
  case 51: return teken_get_sequence(&v->te, TKEY_PAGE_UP);
  case 53: return teken_get_sequence(&v->te, TKEY_LEFT);
  case 55: return teken_get_sequence(&v->te, TKEY_RIGHT);
  case 57: return teken_get_sequence(&v->te, TKEY_END);
  case 58: return teken_get_sequence(&v->te, TKEY_DOWN);
  case 59: return teken_get_sequence(&v->te, TKEY_PAGE_DOWN);
  case 60: return teken_get_sequence(&v->te, TKEY_INSERT);
  case 61: return teken_get_sequence(&v->te, TKEY_DELETE);
  }
  return 0;
}

static void scrollmode_key(struct vt *v, int c) {
  switch (c) {
  case FNU_FKEY | FNU_KEY_F(49): // Home
    vt_scroll(v, 0, 0);
    break;
  case FNU_FKEY | FNU_KEY_F(50): // up
    vt_scroll(v, -1, 1);
    break;
  case FNU_FKEY | FNU_KEY_F(51): // Page Up
    vt_scroll(v, -console_rows, 1);
    break;
  case FNU_FKEY | FNU_KEY_F(57): // End
    vt_scroll(v, 0, 2);
    break;
  case FNU_FKEY | FNU_KEY_F(58): // down
    vt_scroll(v, 1, 1);
    break;
  case FNU_FKEY | FNU_KEY_F(59): // Page Down
    vt_scroll(v, console_rows, 1);
    break;
  }
}

static int kbd_halt = 1, kbd_poweroff = 1, kbd_reboot = 1, kbd_panic = 0;

static void console_sysctl(void) {
  sysctl_add_int("kern.vt.kbd_halt", &kbd_halt, FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("kern.vt.kbd_poweroff", &kbd_poweroff, FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("kern.vt.kbd_reboot", &kbd_reboot, FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("kern.vt.kbd_panic", &kbd_panic, FNU_CTLF_WRITE, 0, 1);
}

static int machine_key(int c) {
  switch (c) {
  case FNU_SPCLKEY | FNU_KEY_DBG:
    return 1;
  case FNU_SPCLKEY | FNU_KEY_HALT:
    if (kbd_halt)
      kill(1, FNU_SIG_USER1);
    return 1;
  case FNU_SPCLKEY | FNU_KEY_PDWN:
    if (kbd_poweroff)
      kill(1, FNU_SIG_USER2);
    return 1;
  case FNU_SPCLKEY | FNU_KEY_PNC:
    if (kbd_panic)
      panic("Forced by the panic key");
    return 1;
  case FNU_SPCLKEY | FNU_KEY_RBT:
    if (kbd_reboot)
      kill(1, FNU_SIG_INTERRUPT);
    return 1;
  case FNU_SPCLKEY | FNU_KEY_SPSC:
  case FNU_SPCLKEY | FNU_KEY_STBY:
  case FNU_SPCLKEY | FNU_KEY_SUSP:
    return 1;
  case FNU_SPCLKEY | FNU_KEY_PASTE:
    mouse_paste(&vts[active]);
    return 1;
  }
  return 0;
}

// kbd_lock held
void console_key(keyboard_t *kbd, int c) {
  static int alt_down;
  struct vt *v;
  const char *seq;
  char utf8[4];
  int n, i;

  if (c & FNU_RELKEY) {
    if ((c & ~FNU_RELKEY) == (int)(FNU_SPCLKEY | FNU_KEY_LALT))
      alt_down = 0;
    return;
  }
  // a key only wakes an idle sleep
  if (acpi_idle_key())
    return;
  if (c == (int)(FNU_SPCLKEY | FNU_KEY_LALT))
    alt_down = 1;
  mtx_enter(&tty_lock);
  v = &vts[active];
  if (machine_key(c)) {
    ;
  } else if (c & FNU_SPCLKEY) {
    c &= ~FNU_SPCLKEY;
    if (c >= FNU_KEY_F_SCR && c < FNU_KEY_F_SCR + NVT) {
      vt_switch(c - FNU_KEY_F_SCR);
    } else if (c == FNU_KEY_NEXT) {
      vt_switch((active + 1) % NVT);
    } else if (c == FNU_KEY_PREV) {
      vt_switch((active + NVT - 1) % NVT);
    } else if (c == FNU_KEY_SLK) {
      int state = 0;
      kbdd_ioctl(kbd, FNU_KBGETLOCKS, &state);
      v->scrolling = (state & FNU_KB_SLKED) != 0;
      if (!v->scrolling)
        vt_scroll(v, 0, 2);
    } else if (v->scrolling) {
      scrollmode_key(v, c);
    } else if (c == (FNU_BKEY | FNU_KEY_BTAB)) {
      input_string(v, teken_get_sequence(&v->te, TKEY_BTAB));
    } else if ((c & FNU_FKEY) && (seq = key_sequence(v, (c & ~FNU_FKEY) - FNU_KEY_F_FN + 1)) != 0) {
      input_string(v, seq);
    }
  } else if (v->scrolling) {
    ;
  } else if (FNU_KEYFLAGS(c) == 0 || FNU_KEYFLAGS(c) == FNU_MKEY) {
    if ((c & FNU_MKEY) || alt_down)
      input_char(v, 0x1b);
    n = put_utf8(utf8, (uint32_t)FNU_KEYCHAR(c));
    for (i = 0; i < n; i++)
      input_char(v, (uchar)utf8[i]);
  }
  vt_flush(&vts[active]);
  mtx_leave(&tty_lock);
}

// THE MOUSE

#define MARK_EXTEND 5

static int is_separator(uint32_t cp) {
  return cp == ' ' || cp == 0 || cp == 0x3000 || (cp >= 0x2002 && cp <= 0x200b);
}

static int mark_set(struct vt *v, int type, int cell) {
  int row = cell / console_columns, col = cell % console_columns, i;
  const vtcell_t *r = vt_scrolled(v) + row * console_columns;

  switch (type) {
  case MARK_END:
    if (v->mark_last != MARK_MOVE)
      return 0;
    // fallthrough
  case MARK_MOVE:
  case MARK_EXTEND:
    v->mark_end = cell;
    break;
  case MARK_START:
    v->mark_start = v->mark_end = cell;
    break;
  case MARK_WORD:
    for (i = col; i >= 0 && !is_separator(CELL_CP(r[i])); i--)
      ;
    v->mark_start = row * console_columns + i + 1;
    for (i = col; i < console_columns && !is_separator(CELL_CP(r[i])); i++)
      ;
    v->mark_end = row * console_columns + i;
    if (v->mark_start > v->mark_end)
      v->mark_start = v->mark_end;
    break;
  case MARK_ROW:
    v->mark_start = row * console_columns;
    v->mark_end = v->mark_start + console_columns;
    break;
  default:
    v->mark_last = type;
    return 0;
  }
  v->mark_last = type;
  return 1;
}

static int put_utf8(char *out, uint32_t cp) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  if (cp > 0x10ffff)
    return 0;
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

static void mark_extract(struct vt *v, int type) {
  int s = v->mark_start < v->mark_end ? v->mark_start : v->mark_end;
  int e = v->mark_start < v->mark_end ? v->mark_end : v->mark_start;
  int srow = s / console_columns, erow, ecol;
  int r, c, cs, ce, n = 0, j;
  const vtcell_t *cells = vt_scrolled(v);

  if (mouse.paste == 0 || e <= s)
    return;
  erow = (e - 1) / console_columns;
  ecol = e - erow * console_columns;
  for (r = srow; r <= erow && r < console_rows; r++) {
    cs = r == srow ? s % console_columns : 0;
    ce = r == erow ? ecol : console_columns;
    j = n;
    for (c = cs; c < ce; c++)
      n += put_utf8(mouse.paste + n, CELL_CP(cells[r * console_columns + c]));
    if (r != erow || type == MARK_ROW) {
      while (n > j && mouse.paste[n - 1] == ' ')
        n--;
      mouse.paste[n++] = '\r';
    }
  }
  mouse.paste_len = n;
}

static void mouse_paste(struct vt *v) {
  int i;

  for (i = 0; i < mouse.paste_len; i++)
    if (mouse.paste[i] != 0)
      input_char(v, (uchar)mouse.paste[i]);
}

static int mouse_cell(void) {
  int col = mouse.x / (int)framebuffer_cell_width();
  int row = mouse.y / (int)framebuffer_cell_height();

  return row * console_columns + col;
}

static void mouse_terminput_button(struct vt *v, int button) {
  int cell = mouse_cell(), x = cell % console_columns, y = cell / console_columns;
  char seq[6] = "\x1b[M";

  if (x > 255 - '!')
    x = 255 - '!';
  if (y > 255 - '!')
    y = 255 - '!';
  seq[3] = (char)(' ' + button);
  seq[4] = (char)('!' + x);
  seq[5] = (char)('!' + y);
  for (x = 0; x < 6; x++)
    input_char(v, (uchar)seq[x]);
}

static void mouse_terminput(struct vt *v, int type, int event, int cnt) {
  if (type != FNU_MOUSE_BUTTON_EVENT)
    return;
  if (cnt <= 0) {
    mouse_terminput_button(v, 3);
    return;
  }
  if (event & FNU_MOUSE_BUTTON1DOWN)
    mouse_terminput_button(v, 0);
  if (event & FNU_MOUSE_BUTTON2DOWN)
    mouse_terminput_button(v, 1);
  if (event & FNU_MOUSE_BUTTON3DOWN)
    mouse_terminput_button(v, 2);
}

void console_mouse_event(int type, int x, int y, int event, int cnt) {
  struct vt *v;
  int mark, maxx, maxy;

  mtx_enter(&tty_lock);
  v = &vts[active];
  if (!mouse.shown || v->cells == 0) {
    mtx_leave(&tty_lock);
    return;
  }
  if (v->mouse_level > 0)
    mouse_terminput(v, type, event, cnt);

  switch (type) {
  case FNU_MOUSE_ACTION:
  case FNU_MOUSE_MOTION_EVENT:
    maxx = console_columns * (int)framebuffer_cell_width() - 1;
    maxy = console_rows * (int)framebuffer_cell_height() - 1;
    x += mouse.x;
    y += mouse.y;
    mouse.x = x < 0 ? 0 : x > maxx ? maxx : x;
    mouse.y = y < 0 ? 0 : y > maxy ? maxy : y;
    if (mouse.buttons & (FNU_MOUSE_BUTTON1DOWN | FNU_MOUSE_BUTTON3DOWN))
      mark_set(v, MARK_MOVE, mouse_cell());
    vt_flush(v);
    mtx_leave(&tty_lock);
    return;
  case FNU_MOUSE_BUTTON_EVENT:
    break;
  default:
    mtx_leave(&tty_lock);
    return;
  }

  switch (event) {
  case FNU_MOUSE_BUTTON1DOWN:
    switch (cnt % 4) {
    case 0: mark = MARK_END; break;
    case 1: mark = MARK_START; break;
    case 2: mark = MARK_WORD; break;
    default: mark = MARK_ROW; break;
    }
    break;
  case FNU_MOUSE_BUTTON2DOWN:
    if (cnt > 0) {
      mouse_paste(v);
      v->mark_start = v->mark_end = 0;
    }
    vt_flush(v);
    mtx_leave(&tty_lock);
    return;
  case FNU_MOUSE_BUTTON3DOWN:
    if (cnt == 0)
      mark = mouse.buttons & FNU_MOUSE_BUTTON1DOWN ? MARK_NONE : MARK_EXTEND;
    else
      mark = MARK_EXTEND;
    break;
  default:
    mtx_leave(&tty_lock);
    return;
  }

  if (cnt > 0)
    mouse.buttons |= event;
  else
    mouse.buttons &= ~event;

  if (mark_set(v, mark, mouse_cell()) &&
      (mark == MARK_END || mark == MARK_WORD || mark == MARK_ROW ||
       mark == MARK_EXTEND))
    mark_extract(v, mark);
  vt_flush(v);
  mtx_leave(&tty_lock);
}

void console_mouse_show(int show) {
  mtx_enter(&tty_lock);
  if (show && !mouse.shown) {
    mouse.x = console_columns * (int)framebuffer_cell_width() / 2;
    mouse.y = console_rows * (int)framebuffer_cell_height() / 2;
  }
  mouse.shown = show;
  vt_flush(&vts[active]);
  mtx_leave(&tty_lock);
}

void console_mouse_move(int x, int y, int relative) {
  int maxx = console_columns * (int)framebuffer_cell_width() - 1;
  int maxy = console_rows * (int)framebuffer_cell_height() - 1;

  mtx_enter(&tty_lock);
  if (relative) {
    x += mouse.x;
    y += mouse.y;
  }
  mouse.x = x < 0 ? 0 : x > maxx ? maxx : x;
  mouse.y = y < 0 ? 0 : y > maxy ? maxy : y;
  vt_flush(&vts[active]);
  mtx_leave(&tty_lock);
}

void console_mouse_info(int *x, int *y, int *buttons) {
  mtx_enter(&tty_lock);
  *x = mouse.x;
  *y = mouse.y;
  *buttons = mouse.buttons;
  mtx_leave(&tty_lock);
}

// ttyN, ttyuN, ttypN
void tty_devname(int minor, char *out, size_t n) {
  if (minor >= 0 && minor < NVT)
    snprintf(out, n, "tty%d", minor + 1);
  else if (minor >= NVT && minor < NFIXED)
    snprintf(out, n, "ttyu%d", minor - NVT);
  else if (minor >= NFIXED)
    snprintf(out, n, "ttyp%d", minor - NFIXED);
  else if (n > 0)
    out[0] = 0;
}

pid_t tty_foreground(int minor) {
  struct vt *v = tty_vt(minor);

  return v != 0 ? __atomic_load_n(&v->tty.t_pgrp, __ATOMIC_RELAXED) : 0;
}

int64_t tty_inputat(int minor) {
  struct vt *v = tty_vt(minor);

  return v != 0 ? __atomic_load_n(&v->tty.t_inputat, __ATOMIC_RELAXED) : 0;
}

static struct vt *vt_of(struct inode *ip) {
  return ip->minor < 0 ? &vts[consvt] : tty_vt(ip->minor);
}

// THE DRIVERS UNDER THE DISCIPLINE
static void vt_start(struct tty *tp) {
  struct vt *v = tp->t_sc;
  int c;

  if (v->starting || (tp->t_state & TS_TTSTOP))
    return;
  v->starting = 1;
  while ((c = tty_getc(&tp->t_outq)) >= 0)
    vtputc(v, c & TTY_CHARMASK);
  v->starting = 0;
  ttwakeupwr(tp);
}

static void ser_start(struct tty *tp) {
  struct vt *v = tp->t_sc;
  int c;

  if (tp->t_state & TS_TTSTOP)
    return;
  while (tp->t_outq.cc > 0 && !uart_tx_full(v->ser - 1)) {
    c = tty_getc(&tp->t_outq);
    uart_tty_putc(v->ser - 1, c & TTY_CHARMASK);
  }
  if (uart_outq(v->ser - 1) > 0)
    tp->t_state |= TS_BUSY;
  else
    tp->t_state &= ~TS_BUSY;
  ttwakeupwr(tp);
}

static int ser_param(struct tty *tp, const struct fnu_ttymode *t) {
  struct vt *v = tp->t_sc;
  int r = uart_param(v->ser - 1, t);

  return r < 0 ? -r : 0;
}

static void pty_start(struct tty *tp) {
  struct vt *v = tp->t_sc;

  if (tp->t_state & TS_TTSTOP)
    return;
  wakeup(&tp->t_outq.cc);
  selwakeup(&v->psel);
}

static void nostop(struct tty *tp, int rw) {
  (void)tp;
  (void)rw;
}

int consoleread(struct inode *ip, struct uio *uio) {
  struct vt *v = vt_of(ip);
  size_t want = uio->uio_resid;
  int error;

  if (v == 0)
    return -FNU_ERR_NODEVICE;
  if (console_dirty)
    console_flush_if_dirty();
  iunlock(ip);
  error = ttread(&v->tty, uio, 0);
  ilock(ip);
  if (error != 0 && uio->uio_resid == want)
    return -error;
  return (int)(want - uio->uio_resid);
}

int consolewrite(struct inode *ip, struct uio *uio) {
  struct vt *v = vt_of(ip);
  size_t want = uio->uio_resid;
  int error;

  if (v == 0)
    return -FNU_ERR_NODEVICE;
  iunlock(ip);
  error = ttwrite(&v->tty, uio, 0);
  ilock(ip);
  if (error != 0 && uio->uio_resid == want)
    return -error;
  return (int)(want - uio->uio_resid);
}

static int pty_external(struct vt *v, uint64_t arg);

int consoleioctl(struct inode *ip, uint64_t cmd, uint64_t arg) {
  struct vt *v = vt_of(ip);
  int r;

  if (v == 0)
    return FNU_ERR_NODEVICE;
  if (cmd == FNU_TTYEXTERNAL && v->pty)
    return pty_external(v, arg);
  if (cmd == FNU_TTYOUTQ && v->ser) {
    mtx_enter(&tty_lock);
    r = v->tty.t_outq.cc + uart_outq(v->ser - 1);
    mtx_leave(&tty_lock);
    return copyout(curproc->p_p->pgdir, (uintptr_t)arg, &r, sizeof(r)) < 0 ? FNU_ERR_FAULT : 0;
  }
  if ((r = ttioctl(&v->tty, cmd, arg)) != TTY_PASS) {
    if (r == 0 && cmd == FNU_TTYDRAIN && v->ser)
      uart_drain(v->ser - 1);
    return r;
  }
  // screen, mouse and keys are the console's, not a pty's or a line's
  if (v->pty || (v->ser && cmd != FNU_TTYBREAK && cmd != FNU_TTYENDBREAK &&
                 cmd != FNU_TTYRAISEDTR && cmd != FNU_TTYDROPDTR &&
                 cmd != FNU_TTYSETLINES && cmd != FNU_TTYRAISELINES &&
                 cmd != FNU_TTYDROPLINES && cmd != FNU_TTYGETLINES))
    return FNU_ERR_BADREQUEST;
  switch (cmd) {
  case FNU_FBTYPE: {
    struct fnu_fbtype ft;
    uint w, h, bpp;

    display_get_resolution(&w, &h, &bpp);
    memset(&ft, 0, sizeof(ft));
    ft.fb_type = virtio_gpu_available() ? FNU_FBTYPE_PCIMISC
                 : bootinfo() && bootinfo()->firmware == FNU_FIRMWARE_UEFI
                     ? FNU_FBTYPE_EFIFB
                     : FNU_FBTYPE_VGA;
    ft.fb_width = (int)w;
    ft.fb_height = (int)h;
    ft.fb_depth = (int)bpp;
    ft.fb_size = (int)framebuffer_size();
    if (copyout(curproc->p_p->pgdir, (uintptr_t)arg, &ft, sizeof(ft)) < 0)
      return FNU_ERR_FAULT;
    return 0;
  }
  case FNU_FBGETMODE: {
    int mode = display_mode_current();

    if (mode < 0)
      return FNU_ERR_WRONGDEVICE;
    if (copyout(curproc->p_p->pgdir, (uintptr_t)arg, &mode, sizeof(mode)) < 0)
      return FNU_ERR_FAULT;
    return 0;
  }
  case FNU_FBMODEINFO:
  case FNU_FBFINDMODE: {
    struct fnu_video_info vi;
    int mode;

    if (copyin(curproc->p_p->pgdir, &vi, (uintptr_t)arg, sizeof(vi)) < 0)
      return FNU_ERR_FAULT;
    if (cmd == FNU_FBFINDMODE) {
      if (vi.vi_width <= 0 || vi.vi_height <= 0)
        return FNU_ERR_INVALID;
      mode = display_mode_find((uint)vi.vi_width, (uint)vi.vi_height);
    } else
      mode = vi.vi_mode;
    if (mode < 0 || display_mode_info(mode, &vi) < 0)
      return FNU_ERR_WRONGDEVICE;
    if (copyout(curproc->p_p->pgdir, (uintptr_t)arg, &vi, sizeof(vi)) < 0)
      return FNU_ERR_FAULT;
    return 0;
  }
  case FNU_FBSETMODE: {
    int mode;

    if (copyin(curproc->p_p->pgdir, &mode, (uintptr_t)arg, sizeof(mode)) < 0)
      return FNU_ERR_FAULT;
    return display_mode_set(mode) < 0 ? FNU_ERR_WRONGDEVICE : 0;
  }
  case FNU_CONSMOUSE: {
    struct fnu_mouse_info mi;

    if (copyin(curproc->p_p->pgdir, &mi, (uintptr_t)arg, sizeof(mi)) < 0)
      return FNU_ERR_FAULT;
    if (mi.operation != FNU_MOUSE_SHOW && mi.operation != FNU_MOUSE_HIDE)
      return FNU_ERR_INVALID;
    console_mouse_show(mi.operation == FNU_MOUSE_SHOW);
    return 0;
  }
  case FNU_TTYBREAK:
  case FNU_TTYENDBREAK:
  case FNU_TTYRAISEDTR:
  case FNU_TTYDROPDTR:
  case FNU_TTYSETLINES:
  case FNU_TTYRAISELINES:
  case FNU_TTYDROPLINES:
  case FNU_TTYGETLINES: {
    int val = 0;

    if (!v->ser)
      return FNU_ERR_BADREQUEST;
    if (cmd == FNU_TTYSETLINES || cmd == FNU_TTYRAISELINES || cmd == FNU_TTYDROPLINES) {
      if (copyin(curproc->p_p->pgdir, &val, (uintptr_t)arg, sizeof(val)) < 0)
        return FNU_ERR_FAULT;
    }
    r = uart_ioctl(v->ser - 1, cmd, &val);
    if (r < 0)
      return -r;
    if (cmd == FNU_TTYGETLINES &&
        copyout(curproc->p_p->pgdir, (uintptr_t)arg, &val, sizeof(val)) < 0)
      return FNU_ERR_FAULT;
    return 0;
  }
  default:
    if (IOCGROUP(cmd) == 'K' || IOCGROUP(cmd) == 'k')
      return kbd_ioctl_user(cmd, arg);
    return FNU_ERR_BADREQUEST;
  }
}

// PSEUDO TERMINALS

static int pty_grow(void) {
  struct vt *chunk;
  char name[16];
  int first, i;

  if ((first = nptys) + PTYCHUNK > maxptys)
    return FNU_ERR_NODEVICE;
  chunk = malloc(PTYCHUNK * sizeof(*chunk), M_DEVBUF, M_WAITOK | M_ZERO);
  for (i = 0; i < PTYCHUNK; i++) {
    chunk[i].pty = 1;
    chunk[i].unit = NFIXED + first + i;
    tty_attach(&chunk[i].tty, pty_start, nostop, 0, &chunk[i]);
    chunk[i].tty.t_unit = chunk[i].unit;
  }
  mtx_enter(&tty_lock);
  if (nptys != first) {
    mtx_leave(&tty_lock);
    free(chunk, M_DEVBUF);
    return 0;
  }
  ptychunks[first / PTYCHUNK] = chunk;
  __atomic_store_n(&nptys, first + PTYCHUNK, __ATOMIC_RELEASE);
  mtx_leave(&tty_lock);
  for (i = first; i < first + PTYCHUNK; i++) {
    snprintf(name, sizeof(name), "ttyp%d", i);
    devfs_add(name, FNU_TYPE_DEVICE, CONSOLE, (short)(NFIXED + i), 0666);
    snprintf(name, sizeof(name), "ptyp%d", i);
    devfs_add(name, FNU_TYPE_DEVICE, PTY, (short)i, 0600);
  }
  return 0;
}

static int pty_free(void) {
  struct vt *v;
  int k, found = -1;

  mtx_enter(&tty_lock);
  for (k = 0; k < nptys && found < 0; k++) {
    v = tty_vt(NFIXED + k);
    if (!v->master_open && v->opens == 0 && v->tty.t_session == 0)
      found = k;
  }
  mtx_leave(&tty_lock);
  return found;
}

// FNU_PTMGET
static int ptmioctl(struct inode *ip, uint64_t cmd, uint64_t arg) {
  struct proc *p = curproc;
  struct fnu_ptmget pm;
  struct file *cf, *sf;
  int cfd, sfd, k, error;

  (void)ip;
  if (cmd != FNU_PTMGET)
    return FNU_ERR_BADREQUEST;
  memset(&pm, 0, sizeof(pm));
  for (;;) {
    if ((k = pty_free()) < 0) {
      if ((error = pty_grow()) != 0)
        return error;
      continue;
    }
    snprintf(pm.cn, sizeof(pm.cn), "/dev/ptyp%d", k);
    snprintf(pm.sn, sizeof(pm.sn), "/dev/ttyp%d", k);
    if ((error = kern_open_file(p, 0, pm.cn, FNU_OPEN_READWRITE, 0, &cf)) == FNU_ERR_BUSY)
      continue;
    if (error != 0)
      return error;
    if ((error = kern_open_file(p, 0, pm.sn, FNU_OPEN_READWRITE, 0, &sf)) != 0) {
      fileclose(cf);
      return error;
    }
    if ((cfd = fd_alloc_at(p->p_p, cf, 0, 0)) < 0) {
      fileclose(cf);
      fileclose(sf);
      return FNU_ERR_TOOMANYOPEN;
    }
    if ((sfd = fd_alloc_at(p->p_p, sf, 0, 0)) < 0) {
      fd_untake(p->p_p, cfd, cf);
      fileclose(sf);
      return FNU_ERR_TOOMANYOPEN;
    }
    pm.cfd = (int32)cfd;
    pm.sfd = (int32)sfd;
    if (copyout(p->p_p->pgdir, (uintptr_t)arg, &pm, sizeof(pm)) < 0) {
      fd_untake(p->p_p, cfd, cf);
      fd_untake(p->p_p, sfd, sf);
      return FNU_ERR_FAULT;
    }
    return 0;
  }
}

static struct vt *pty_of(struct inode *ip) {
  return ip->minor >= 0 ? tty_vt(NFIXED + ip->minor) : 0;
}

static int consoleopen(struct inode *ip, int omode) {
  struct vt *v = vt_of(ip);

  (void)omode;
  if (v == 0)
    return FNU_ERR_NODEVICE;
  mtx_enter(&tty_lock);
  if (v->pty && !v->master_open) {
    mtx_leave(&tty_lock);
    return FNU_ERR_NODEVICE;
  }
  if (v->opens++ == 0)
    ttyopen(&v->tty);
  if (v->pty)
    v->slave_seen = 1;
  mtx_leave(&tty_lock);
  return 0;
}

static void consoleclose(struct inode *ip) {
  struct vt *v = vt_of(ip);

  if (v == 0)
    return;
  mtx_enter(&tty_lock);
  if (v->opens > 0 && --v->opens == 0) {
    ttylclose(&v->tty);
    ttyclose(&v->tty);
  }
  if (v->pty) {
    wakeup(&v->tty.t_outq.cc);
    selwakeup(&v->psel);
  }
  mtx_leave(&tty_lock);
}

static int ptyopen(struct inode *ip, int omode) {
  struct vt *v = pty_of(ip);

  (void)omode;
  if (v == 0)
    return FNU_ERR_NODEVICE;
  mtx_enter(&tty_lock);
  if (v->master_open) {
    mtx_leave(&tty_lock);
    return FNU_ERR_BUSY;
  }
  tty_attach(&v->tty, pty_start, nostop, 0, v);
  v->tty.t_unit = v->unit;
  v->tty.t_mode.line &= ~FNU_TTYLINE_LOCAL;
  v->master_open = 1;
  v->slave_seen = 0;
  mtx_leave(&tty_lock);
  return 0;
}

static void ptyclose(struct inode *ip) {
  struct vt *v = pty_of(ip);

  if (v == 0)
    return;
  mtx_enter(&tty_lock);
  v->master_open = 0;
  mtx_leave(&tty_lock);
  ttymodem(&v->tty, 0);
  mtx_enter(&tty_lock);
  v->tty.t_state &= ~TS_CARR_ON;
  v->tty.t_state |= TS_ZOMBIE;
  ttwakeup(&v->tty);
  wakeup(&v->tty.t_outq);
  mtx_leave(&tty_lock);
}

static int ptypoll(struct inode *ip, int want) {
  struct vt *v;
  int has, minor = ip->minor;

  if (minor < 0 || (v = tty_vt(NFIXED + minor)) == 0)
    return 0;
  mtx_enter(&tty_lock);
  has = (v->tty.t_outq.cc > 0 && !(v->tty.t_state & TS_TTSTOP)) ||
        (v->opens == 0 && v->slave_seen);
  if (!has && (want & FNU_SEL_READ))
    selrecord(&v->psel);
  mtx_leave(&tty_lock);
  return (has ? want & FNU_SEL_READ : 0) | (want & FNU_SEL_WRITE);
}

#define PTYCHUNK_IO 256

static int ptyread(struct inode *ip, struct uio *uio) {
  struct vt *v = pty_of(ip);
  char buf[PTYCHUNK_IO];
  int n, c, got = 0;

  if (v == 0)
    return -FNU_ERR_NODEVICE;
  iunlock(ip);
  mtx_enter(&tty_lock);
  while (v->tty.t_outq.cc == 0 || (v->tty.t_state & TS_TTSTOP)) {
    if (v->opens == 0 && v->slave_seen)
      goto done;
    if (proc_interrupted()) {
      mtx_leave(&tty_lock);
      ilock(ip);
      return -FNU_ERR_INTERRUPTED;
    }
    msleep(&v->tty.t_outq.cc, &tty_lock, PZERO | PCATCH, "ptcin", 0);
  }
  while (uio->uio_resid > 0 && v->tty.t_outq.cc > 0) {
    for (n = 0; n < (int)sizeof(buf) && (size_t)n < uio->uio_resid &&
                (c = tty_getc(&v->tty.t_outq)) >= 0;)
      buf[n++] = (char)c;
    ttwakeupwr(&v->tty);
    mtx_leave(&tty_lock);
    if (uiomove(buf, (size_t)n, uio) != 0) {
      ilock(ip);
      return got > 0 ? got : -FNU_ERR_FAULT;
    }
    got += n;
    mtx_enter(&tty_lock);
  }
done:
  mtx_leave(&tty_lock);
  ilock(ip);
  return got;
}

static int ptywrite(struct inode *ip, struct uio *uio) {
  struct vt *v = pty_of(ip);
  char buf[PTYCHUNK_IO];
  int n, i = 0, got = 0;

  if (v == 0)
    return -FNU_ERR_NODEVICE;
  iunlock(ip);
  while (uio->uio_resid > 0) {
    n = uio->uio_resid < sizeof(buf) ? (int)uio->uio_resid : (int)sizeof(buf);
    if (uiomove(buf, (size_t)n, uio) != 0) {
      ilock(ip);
      return got > 0 ? got : -FNU_ERR_FAULT;
    }
    mtx_enter(&tty_lock);
    for (i = 0; i < n; i++) {
      while (v->tty.t_rawq.cc + v->tty.t_canq.cc >= TTYQ_SIZE - 2 &&
             !(v->tty.t_mode.edit & FNU_TTYEDIT_EXTERNAL)) {
        if (v->opens == 0 && v->slave_seen)
          break;
        if (proc_interrupted() ||
            msleep(&v->tty.t_canq, &tty_lock, PZERO | PCATCH, "ptcout", 0) != 0) {
          mtx_leave(&tty_lock);
          uio->uio_resid += (size_t)(n - i);
          ilock(ip);
          return got + i > 0 ? got + i : -FNU_ERR_INTERRUPTED;
        }
      }
      ttyinput(buf[i] & 0xff, &v->tty);
    }
    mtx_leave(&tty_lock);
    got += n;
  }
  ilock(ip);
  return got;
}

// TIOCEXT
static int pty_external(struct vt *v, uint64_t arg) {
  int on;

  if (copyin(curproc->p_p->pgdir, &on, (uintptr_t)arg, sizeof(on)) < 0)
    return FNU_ERR_FAULT;
  mtx_enter(&tty_lock);
  if (on)
    v->tty.t_mode.edit |= FNU_TTYEDIT_EXTERNAL;
  else
    v->tty.t_mode.edit &= ~FNU_TTYEDIT_EXTERNAL;
  mtx_leave(&tty_lock);
  return 0;
}

static int ptyioctl(struct inode *ip, uint64_t cmd, uint64_t arg) {
  struct vt *v = pty_of(ip);
  int r;

  if (v == 0)
    return FNU_ERR_NODEVICE;
  if (cmd == FNU_TTYEXTERNAL)
    return pty_external(v, arg);
  r = ttioctl(&v->tty, cmd, arg);
  return r == TTY_PASS ? FNU_ERR_BADREQUEST : r;
}

static void vt_winsize(struct vt *v) {
  struct fnu_ttysize ws;
  uint32_t w, h, bpp;

  memset(&ws, 0, sizeof(ws));
  ws.rows = (uint16)console_rows;
  ws.cols = (uint16)console_columns;
  if (framebuffer_available()) {
    display_get_resolution(&w, &h, &bpp);
    ws.xpixels = (uint16)w;
    ws.ypixels = (uint16)h;
  }
  if (memcmp(&ws, &v->tty.t_winsize, sizeof(ws)) != 0) {
    v->tty.t_winsize = ws;
    if (v->tty.t_pgrp > 0)
      tty_pgsignal(v->tty.t_pgrp, v->tty.t_session, FNU_SIG_WINDOW);
  }
}

static int vt_reset_all(void) {
  uint64_t bytes = (uint64_t)console_columns * console_rows * sizeof(vtcell_t);
  int n;

  if (mouse.view != 0)
    contig_free(mouse.view);
  if (mouse.paste != 0)
    contig_free(mouse.paste);
  if (mouse.snap != 0)
    contig_free(mouse.snap);
  mouse.snap = contig_alloc(bytes);
  mouse.view = contig_alloc(bytes);
  mouse.paste = contig_alloc(bytes + console_rows + 1);
  mouse.paste_len = 0;
  mouse.buttons = 0;
  for (n = 0; n < NVT; n++) {
    struct vt *v = &vts[n];

    if (v->cells != 0)
      contig_free(v->cells);
    v->cells = contig_alloc(bytes);
    if (v->cells == 0)
      return -1;
    v->pos = 0;
    v->pending_scrolls = 0;
    v->cursor_hidden = 0;
    v->mark_start = v->mark_end = v->mark_last = 0;
    v->mouse_level = 0;
    if (v->hist != 0)
      contig_free(v->hist);
    v->hist = contig_alloc((uint64_t)HISTORY_ROWS * console_columns * sizeof(vtcell_t));
    v->hist_tail = v->hist_len = 0;
    v->view_off = 0;
    v->scrolling = 0;
    v->kbdstate = 0;
    v->te_ready = 0;
    teken_init(&v->te, &vt_funcs, v);
    v->te_ready = 1;
    {
      teken_pos_t size = {.tp_row = (teken_unit_t)console_rows,
                          .tp_col = (teken_unit_t)console_columns};

      teken_rect_t all = {.tr_begin = {0, 0}, .tr_end = size};

      teken_set_winsize(&v->te, &size);
      // fresh cells hold whatever the memory held. garbage
      tf_fill(v, &all, ' ', teken_get_defattr(&v->te));
    }
    vt_winsize(v);
  }
  return 0;
}

void consoleinit(void) {
  int i;

  tty_init();
  for (i = 0; i < NFIXED; i++) {
    vts[i].unit = i;
    if (i >= NVT) {
      vts[i].ser = i - NVT + 1;
      tty_attach(&vts[i].tty, ser_start, nostop, ser_param, &vts[i]);
      vts[i].tty.t_mode.speed_in = vts[i].tty.t_mode.speed_out =
          (uint32)uart_console_speed();
    } else
      tty_attach(&vts[i].tty, vt_start, nostop, 0, &vts[i]);
    vts[i].tty.t_unit = i;
  }
  if (framebuffer_available()) {
    console_columns = framebuffer_columns();
    console_rows = framebuffer_rows();
  }
  if (vt_reset_all() < 0)
    panic("console: no memory for terminals");
  if (framebuffer_available())
    framebuffer_redraw_cells(vts[active].cells, console_columns * console_rows);

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].class = FNU_DEVCLASS_TERMINAL;
  devsw[PTY].class = FNU_DEVCLASS_TERMINAL;
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].ioctl = consoleioctl;
  devsw[CONSOLE].poll = consolepoll;
  devsw[CONSOLE].open = consoleopen;
  devsw[CONSOLE].close = consoleclose;
  devsw[PTY].read = ptyread;
  devsw[PTY].write = ptywrite;
  devsw[PTY].ioctl = ptyioctl;
  devsw[PTY].poll = ptypoll;
  devsw[PTY].open = ptyopen;
  devsw[PTY].close = ptyclose;
  devsw[PTM].ioctl = ptmioctl;
  devsw[PTM].class = FNU_DEVCLASS_TERMINAL;
  devsw[CTTY].class = FNU_DEVCLASS_TERMINAL;
}

// kbd_lock held; not while a thread owns the controller
void kbdc_intr_locked(void) {
  if (kbdc_busy(atkbdc_open()))
    return;
  atkbd_intr_locked();
  psm_intr_locked();
}

static int kbd_irq(void *arg __unused) {
  mtx_enter(&kbd_lock);
  kbdc_intr_locked();
  mtx_leave(&kbd_lock);
  return 1;
}

static void console_choose(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  const char *c = kgetenv("console");

  consvt = 0;
  if (uart_ports() > 0 &&
      ((c != 0 && strncmp(c, "comconsole", 10) == 0) ||
       (bi != 0 && bi->console == FNU_CONSOLE_NONE)))
    consvt = NVT;
}

void console_enable_input(void) {
  int i;

  console_sysctl();
  console_choose();
  cn_tp = &vts[consvt].tty;
  cons.locking = 1;

  devfs_add("console", FNU_TYPE_DEVICE, CONSOLE, -1, 0666);
  devfs_add("ptm", FNU_TYPE_DEVICE, PTM, 0, 0666);
  devfs_add("tty", FNU_TYPE_DEVICE, CTTY, 0, 0666);
  for (i = 0; i < NVT; i++) {
    char name[8];

    strlcpy(name, "tty0", sizeof(name));
    name[3] = (char)('1' + i);
    devfs_add(name, FNU_TYPE_DEVICE, CONSOLE, (short)i, 0666);
  }
  for (i = 0; i < NSER && i < uart_ports(); i++) {
    char name[8];

    strlcpy(name, "ttyu0", sizeof(name));
    name[4] = (char)('0' + i);
    devfs_add(name, FNU_TYPE_DEVICE, CONSOLE, (short)(NVT + i), 0666);
  }
  maxptys = MAXPTYS;
  ptychunks = malloc((size_t)(maxptys / PTYCHUNK) * sizeof(*ptychunks),
                     M_DEVBUF, M_WAITOK | M_ZERO);
  pty_grow();

  atkbdinit();
  intr_establish(ioapic_isa_vector(IRQ_KBD, 0), kbd_irq, (void *)0, "atkbd");
  intr_establish(ioapic_isa_vector(IRQ_AUX, 0), kbd_irq, (void *)0, "atkbd");
}

// the cells stay when the grid is the same
void console_switch_to_gpu(void) {
  mtx_enter(&tty_lock);
  if (framebuffer_available() && vts[active].cells != 0 &&
      (int)framebuffer_columns() == console_columns && (int)framebuffer_rows() == console_rows) {
    console_dirty = 0;
    framebuffer_redraw_cells(vt_view(&vts[active]), console_columns * console_rows);
  } else if (framebuffer_available()) {
    console_columns = framebuffer_columns();
    console_rows = framebuffer_rows();
    console_dirty = 0;
    if (vt_reset_all() == 0)
      framebuffer_redraw_cells(vts[active].cells,
                               console_columns * console_rows);
  }
  mtx_leave(&tty_lock);
}
