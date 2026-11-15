// ATKBDC
#include "atkbdcreg.h"
#include "defs.h"
#include "kbdreg.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "x86.h"

static struct atkbdc_softc kbdc;

static int verbose;

static inline int read_status(KBDC p __unused) { return inb(KBD_STATUS_PORT); }
static inline int read_data(KBDC p __unused) { return inb(KBD_DATA_PORT); }
static inline void write_command(KBDC p __unused, int c) { outb(KBD_COMMAND_PORT, (uchar)c); }
static inline void write_data(KBDC p __unused, int c) { outb(KBD_DATA_PORT, (uchar)c); }

static inline int nextq(int i) { return (i + 1) % KBDQ_BUFSIZE; }
static inline int availq(struct kqueue *q) { return q->head != q->tail; }
static inline void emptyq(struct kqueue *q) { q->tail = q->head = 0; }

static int addq(struct kqueue *q, int c) {
  if (nextq(q->tail) != q->head) {
    q->q[q->tail] = (uchar)c;
    q->tail = nextq(q->tail);
    return 1;
  }
  return 0;
}

static int removeq(struct kqueue *q) {
  int c;

  if (q->tail != q->head) {
    c = q->q[q->head];
    q->head = nextq(q->head);
    return c;
  }
  return -1;
}

// an SMI-emulated controller reads slowly
static int retries(void) {
  uint64_t t0, t1, t2, slow;
  uint fl = readeflags();

  cli();
  t0 = rdtsc();
  (void)read_status(&kbdc);
  t1 = rdtsc();
  delay(1000);
  t2 = rdtsc();
  if (fl & FL_IF)
    sti();
  if (t2 - t1 < 1000)
    return 5000;
  slow = (t1 - t0) / ((t2 - t1) / 1000);
  return (int)(100000 / (KBDD_DELAYTIME * 2 + slow));
}

KBDC atkbdc_open(void) {
  if (kbdc.retry == 0) {
    kbdc.command_byte = -1;
    kbdc.command_mask = 0;
    kbdc.retry = retries();
    kbdc.aux_mux_enabled = 0;
    kbdc.aux_mux_port = 0;
  }
  return &kbdc;
}

// interrupt: try. thread: own it and wait
int kbdc_lock(KBDC p, int lock) {
  int prevlock;

  if (mtx_owned(&kbd_lock)) {
    prevlock = p->lock;
    if (lock != prevlock) {
      p->lock = lock;
      p->owner = 0;
      if (!lock)
        wakeup(&p->lock);
    }
    return prevlock != lock;
  }
  mtx_enter(&kbd_lock);
  if (lock) {
    while (p->lock && p->owner != curproc && curproc != 0)
      msleep(&p->lock, &kbd_lock, PZERO, "kbdc", 0);
    prevlock = p->lock;
    p->lock = 1;
    if (!prevlock)
      p->owner = curproc;
  } else {
    prevlock = p->lock;
    p->lock = 0;
    p->owner = 0;
    wakeup(&p->lock);
    kbdc_intr_locked();
  }
  mtx_leave(&kbd_lock);
  return prevlock != lock;
}

int kbdc_busy(KBDC p) { return p->lock; }

int kbdc_data_ready(KBDC p) {
  return availq(&p->kbd) || availq(&p->aux) ||
         (read_status(p) & KBDS_ANY_BUFFER_FULL);
}

static int wait_while_controller_busy(KBDC p) {
  int retry = p->retry, f;

  while ((f = read_status(p)) & KBDS_INPUT_BUFFER_FULL) {
    if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      addq(&p->kbd, read_data(p));
    } else if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      addq(&p->aux, read_data(p));
    }
    delay(KBDC_DELAYTIME);
    if (--retry < 0)
      return 0;
  }
  return 1;
}

static int wait_for_data(KBDC p) {
  int retry = p->retry * 2, f;

  while ((f = read_status(p) & KBDS_ANY_BUFFER_FULL) == 0) {
    delay(KBDC_DELAYTIME);
    if (--retry < 0)
      return 0;
  }
  delay(KBDD_DELAYTIME);
  return f;
}

static int wait_for_kbd_data(KBDC p) {
  int retry = p->retry * 2, f;

  while ((f = read_status(p) & KBDS_BUFFER_FULL) != KBDS_KBD_BUFFER_FULL) {
    if (f == KBDS_AUX_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      addq(&p->aux, read_data(p));
    }
    delay(KBDC_DELAYTIME);
    if (--retry < 0)
      return 0;
  }
  delay(KBDD_DELAYTIME);
  return f;
}

static int wait_for_kbd_ack(KBDC p) {
  int retry = p->retry * 2, f, b;

  while (retry-- > 0) {
    if ((f = read_status(p)) & KBDS_ANY_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      b = read_data(p);
      if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
        if (b == KBD_ACK || b == KBD_RESEND || b == KBD_RESET_FAIL)
          return b;
        addq(&p->kbd, b);
      } else if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
        addq(&p->aux, b);
      }
    }
    delay(KBDC_DELAYTIME);
  }
  return -1;
}

static int wait_for_aux_data(KBDC p) {
  int retry = p->retry * 2, f;

  while ((f = read_status(p) & KBDS_BUFFER_FULL) != KBDS_AUX_BUFFER_FULL) {
    if (f == KBDS_KBD_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      addq(&p->kbd, read_data(p));
    }
    delay(KBDC_DELAYTIME);
    if (--retry < 0)
      return 0;
  }
  delay(KBDD_DELAYTIME);
  return f;
}

static int wait_for_aux_ack(KBDC p) {
  int retry = p->retry * 2, f, b;

  while (retry-- > 0) {
    if ((f = read_status(p)) & KBDS_ANY_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      b = read_data(p);
      if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL) {
        if (b == PSM_ACK || b == PSM_RESEND || b == PSM_RESET_FAIL)
          return b;
        addq(&p->aux, b);
      } else if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL) {
        addq(&p->kbd, b);
      }
    }
    delay(KBDC_DELAYTIME);
  }
  return -1;
}

int write_controller_command(KBDC p, int c) {
  if (!wait_while_controller_busy(p))
    return 0;
  write_command(p, c);
  return 1;
}

int write_controller_data(KBDC p, int c) {
  if (!wait_while_controller_busy(p))
    return 0;
  write_data(p, c);
  return 1;
}

int write_kbd_command(KBDC p, int c) {
  if (!wait_while_controller_busy(p))
    return 0;
  write_data(p, c);
  return 1;
}

int write_aux_command(KBDC p, int c) {
  int f = aux_mux_is_enabled(p) ? KBDC_WRITE_TO_AUX_MUX + p->aux_mux_port
                                : KBDC_WRITE_TO_AUX;

  if (!write_controller_command(p, f))
    return 0;
  return write_controller_data(p, c);
}

int send_kbd_command(KBDC p, int c) {
  int retry = KBD_MAXRETRY, res = -1;

  while (retry-- > 0) {
    if (!write_kbd_command(p, c))
      continue;
    res = wait_for_kbd_ack(p);
    if (res == KBD_ACK)
      break;
  }
  return res;
}

int send_aux_command(KBDC p, int c) {
  int retry = KBD_MAXRETRY, res = -1;

  while (retry-- > 0) {
    if (!write_aux_command(p, c))
      continue;
    emptyq(&p->aux);
    res = wait_for_aux_ack(p);
    if (res == PSM_ACK)
      break;
  }
  return res;
}

int send_kbd_command_and_data(KBDC p, int c, int d) {
  int retry, res = -1;

  for (retry = KBD_MAXRETRY; retry > 0; --retry) {
    if (!write_kbd_command(p, c))
      continue;
    res = wait_for_kbd_ack(p);
    if (res == KBD_ACK)
      break;
    else if (res != KBD_RESEND)
      return res;
  }
  if (retry <= 0)
    return res;

  for (retry = KBD_MAXRETRY, res = -1; retry > 0; --retry) {
    if (!write_kbd_command(p, d))
      continue;
    res = wait_for_kbd_ack(p);
    if (res != KBD_RESEND)
      break;
  }
  return res;
}

int send_aux_command_and_data(KBDC p, int c, int d) {
  int retry, res = -1;

  for (retry = KBD_MAXRETRY; retry > 0; --retry) {
    if (!write_aux_command(p, c))
      continue;
    emptyq(&p->aux);
    res = wait_for_aux_ack(p);
    if (res == PSM_ACK)
      break;
    else if (res != PSM_RESEND)
      return res;
  }
  if (retry <= 0)
    return res;

  for (retry = KBD_MAXRETRY, res = -1; retry > 0; --retry) {
    if (!write_aux_command(p, d))
      continue;
    res = wait_for_aux_ack(p);
    if (res != PSM_RESEND)
      break;
  }
  return res;
}

int read_controller_data(KBDC p) {
  if (availq(&p->kbd))
    return removeq(&p->kbd);
  if (availq(&p->aux))
    return removeq(&p->aux);
  if (!wait_for_data(p))
    return -1;
  return read_data(p);
}

int read_kbd_data(KBDC p) {
  if (availq(&p->kbd))
    return removeq(&p->kbd);
  if (!wait_for_kbd_data(p))
    return -1;
  return read_data(p);
}

int read_kbd_data_no_wait(KBDC p) {
  int f;

  if (availq(&p->kbd))
    return removeq(&p->kbd);
  f = read_status(p) & KBDS_BUFFER_FULL;
  if (f == KBDS_AUX_BUFFER_FULL) {
    delay(KBDD_DELAYTIME);
    addq(&p->aux, read_data(p));
    f = read_status(p) & KBDS_BUFFER_FULL;
  }
  if (f == KBDS_KBD_BUFFER_FULL) {
    delay(KBDD_DELAYTIME);
    return read_data(p);
  }
  return -1;
}

int read_aux_data(KBDC p) {
  if (availq(&p->aux))
    return removeq(&p->aux);
  if (!wait_for_aux_data(p))
    return -1;
  return read_data(p);
}

int read_aux_data_no_wait(KBDC p) {
  int f;

  if (availq(&p->aux))
    return removeq(&p->aux);
  f = read_status(p) & KBDS_BUFFER_FULL;
  if (f == KBDS_KBD_BUFFER_FULL) {
    delay(KBDD_DELAYTIME);
    addq(&p->kbd, read_data(p));
    f = read_status(p) & KBDS_BUFFER_FULL;
  }
  if (f == KBDS_AUX_BUFFER_FULL) {
    delay(KBDD_DELAYTIME);
    return read_data(p);
  }
  return -1;
}

void empty_kbd_buffer(KBDC p, int wait) {
  int t, b, f, delta = 2;

  for (t = wait; t > 0;) {
    if ((f = read_status(p)) & KBDS_ANY_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      b = read_data(p);
      if ((f & KBDS_BUFFER_FULL) == KBDS_AUX_BUFFER_FULL)
        addq(&p->aux, b);
      t = wait;
    } else {
      t -= delta;
    }
    delay(delta * 1000);
  }
  emptyq(&p->kbd);
}

void empty_aux_buffer(KBDC p, int wait) {
  int t, b, f, delta = 2;

  for (t = wait; t > 0;) {
    if ((f = read_status(p)) & KBDS_ANY_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      b = read_data(p);
      if ((f & KBDS_BUFFER_FULL) == KBDS_KBD_BUFFER_FULL)
        addq(&p->kbd, b);
      t = wait;
    } else {
      t -= delta;
    }
    delay(delta * 1000);
  }
  emptyq(&p->aux);
}

void empty_both_buffers(KBDC p, int wait) {
  int t, f, waited = 0, delta = 2;

  for (t = wait; t > 0;) {
    if ((f = read_status(p)) & KBDS_ANY_BUFFER_FULL) {
      delay(KBDD_DELAYTIME);
      (void)read_data(p);
      t = wait;
    } else {
      t -= delta;
    }
    waited += delta * 1000;
    if (waited == delta * 1000000)
      return;
    delay(delta * 1000);
  }
  emptyq(&p->kbd);
  emptyq(&p->aux);
}

// reset answer, KBD_MAXWAIT turns
static int reset_answer(KBDC p, int (*read)(KBDC)) {
  int ms, c = -1;

  for (ms = 0; ms < KBD_MAXWAIT * KBD_RESETDELAY; ms++) {
    delay(1000);
    if ((c = read(p)) != -1)
      break;
  }
  return c;
}

int reset_kbd(KBDC p) {
  int retry = KBD_MAXRETRY, c = KBD_RESEND;

  while (retry-- > 0) {
    empty_both_buffers(p, 10);
    if (!write_kbd_command(p, KBDC_RESET_KBD))
      continue;
    emptyq(&p->kbd);
    c = read_controller_data(p);
    if (verbose)
      printf("kbdc: RESET_KBD return code:%x\n", c);
    if (c == KBD_ACK)
      break;
  }
  if (retry < 0)
    return 0;

  c = reset_answer(p, read_controller_data);
  if (verbose)
    printf("kbdc: RESET_KBD status:%x\n", c);
  return c == KBD_RESET_DONE;
}

int reset_aux_dev(KBDC p) {
  int retry = KBD_MAXRETRY, c = PSM_RESEND;

  while (retry-- > 0) {
    empty_both_buffers(p, 10);
    if (!write_aux_command(p, PSMC_RESET_DEV))
      continue;
    emptyq(&p->aux);
    c = reset_answer(p, read_aux_data_no_wait);
    if (verbose)
      printf("kbdc: RESET_AUX return code:%x\n", c);
    if (c == PSM_ACK)
      break;
  }
  if (retry < 0)
    return 0;

  c = reset_answer(p, read_aux_data_no_wait);
  if (verbose)
    printf("kbdc: RESET_AUX status:%x\n", c);
  if (c != PSM_RESET_DONE)
    return 0;

  c = read_aux_data(p);
  if (verbose)
    printf("kbdc: RESET_AUX ID:%x\n", c);
  return 1;
}

int test_controller(KBDC p) {
  int retry = KBD_MAXRETRY, c = KBD_DIAG_FAIL;

  while (retry-- > 0) {
    empty_both_buffers(p, 10);
    if (write_controller_command(p, KBDC_DIAGNOSE))
      break;
  }
  if (retry < 0)
    return 0;

  emptyq(&p->kbd);
  c = reset_answer(p, read_controller_data);
  if (verbose)
    printf("kbdc: DIAGNOSE status:%x\n", c);
  return c == KBD_DIAG_DONE;
}

int test_kbd_port(KBDC p) {
  int retry = KBD_MAXRETRY, again = KBD_MAXWAIT, c = -1;

  while (retry-- > 0) {
    empty_both_buffers(p, 10);
    if (write_controller_command(p, KBDC_TEST_KBD_PORT))
      break;
  }
  if (retry < 0)
    return 0;

  emptyq(&p->kbd);
  while (again-- > 0) {
    c = read_controller_data(p);
    if (c != -1)
      break;
  }
  if (verbose)
    printf("kbdc: TEST_KBD_PORT status:%x\n", c);
  return c;
}

int test_aux_port(KBDC p) {
  int retry = KBD_MAXRETRY, again = KBD_MAXWAIT, c = -1;

  while (retry-- > 0) {
    empty_both_buffers(p, 10);
    if (write_controller_command(p, KBDC_TEST_AUX_PORT))
      break;
  }
  if (retry < 0)
    return 0;

  emptyq(&p->kbd);
  while (again-- > 0) {
    c = read_controller_data(p);
    if (c != -1)
      break;
  }
  if (verbose)
    printf("kbdc: TEST_AUX_PORT status:%x\n", c);
  return c;
}

int kbdc_get_device_mask(KBDC p) { return p->command_mask; }

void kbdc_set_device_mask(KBDC p, int mask) {
  p->command_mask = mask & (KBD_KBD_CONTROL_BITS | KBD_AUX_CONTROL_BITS);
}

int get_controller_command_byte(KBDC p) {
  if (p->command_byte != -1)
    return p->command_byte;
  if (!write_controller_command(p, KBDC_GET_COMMAND_BYTE))
    return -1;
  emptyq(&p->kbd);
  p->command_byte = read_controller_data(p);
  return p->command_byte;
}

int set_controller_command_byte(KBDC p, int mask, int command) {
  if (get_controller_command_byte(p) == -1)
    return 0;

  command = (p->command_byte & ~mask) | (command & mask);
  if (command & KBD_DISABLE_KBD_PORT) {
    if (!write_controller_command(p, KBDC_DISABLE_KBD_PORT))
      return 0;
  }
  if (!write_controller_command(p, KBDC_SET_COMMAND_BYTE))
    return 0;
  if (!write_controller_data(p, command))
    return 0;
  p->command_byte = command;

  if (verbose)
    printf("kbdc: new command byte:%x (set_controller...)\n", command);
  return 1;
}

// ACTIVE MULTIPLEXING

static int set_aux_mux_state(KBDC p, int enabled) {
  int command, version;

  if (write_controller_command(p, KBDC_FORCE_AUX_OUTPUT) == 0 ||
      write_controller_data(p, 0xf0) == 0 || read_controller_data(p) != 0xf0)
    return -1;

  if (write_controller_command(p, KBDC_FORCE_AUX_OUTPUT) == 0 ||
      write_controller_data(p, 0x56) == 0 || read_controller_data(p) != 0x56)
    return -1;

  command = enabled ? 0xa4 : 0xa5;
  if (write_controller_command(p, KBDC_FORCE_AUX_OUTPUT) == 0 ||
      write_controller_data(p, command) == 0 ||
      (version = read_controller_data(p)) == command)
    return -1;

  return version;
}

int set_active_aux_mux_port(KBDC p, int port) {
  if (!aux_mux_is_enabled(p))
    return 0;
  if (port < 0 || port >= KBDC_AUX_MUX_NUM_PORTS)
    return 0;
  p->aux_mux_port = port;
  return 1;
}

int enable_aux_mux(KBDC p) {
  int version = set_aux_mux_state(p, 1);

  if (version >= 0) {
    p->aux_mux_enabled = 1;
    set_active_aux_mux_port(p, 0);
  }
  return version;
}

int disable_aux_mux(KBDC p) {
  p->aux_mux_enabled = 0;
  return set_aux_mux_state(p, 0);
}

int aux_mux_is_enabled(KBDC p) { return p->aux_mux_enabled; }
