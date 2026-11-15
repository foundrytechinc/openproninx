// OSPM

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"
#include "inc/abi.h"
#include "inc/signal.h"
#include "malloc.h"
#include "mutex.h"
#include "proc.h"
#include "trap.h"
#include "x86.h"

// THE REGISTERS

enum {
  R_SMI,
  R_PM1A_STS,
  R_PM1A_EN,
  R_PM1A_CNT,
  R_PM1B_STS,
  R_PM1B_EN,
  R_PM1B_CNT,
  R_PM2_CNT,
  R_GPE0_STS,
  R_GPE0_EN,
  R_GPE1_STS,
  R_GPE1_EN,
  R_NREG,
};

struct pmreg {
  int space;
  uint64_t addr;
  int size;   // bytes the block has
  int access;
};

struct gpe {
  int (*fn)(int, void *);
  void *arg;
  volatile int active;
  int flags;
};

struct task {
  void (*fn)(void *, int);
  void *arg0;
  int arg1;
  struct task *next;
};

struct poll {
  void (*fn)(void *);
  void *arg;
  uint64_t period, next; // in ticks
  struct poll *link;
};
#define POLL_SECONDS 10

static struct {
  struct acpi_fadt *fadt;
  struct pmreg reg[R_NREG];
  int events;
  struct mutex pm;
  struct {
    int typa, typb;
  } sleeptype[6];
  struct aml_node *pts;
  struct gpe *gpe;
  int ngpe;
  struct {
    int sts, en; // the registers
    int base;
    int bytes;
  } blk[2];
  int nblk;
  int on;
  int halting;
  int wake_pm1, wake_gpe; // set at the wake; the first gpe or -1
  int sci;
  int allowpowerdown;
  struct {
    struct mutex lock;
    struct task *head, **tail;
  } tasks;
  struct poll *polls;
  struct proc *thread;
  int waiting;
} os = {.pm = MUTEX_INITIALIZER("acpi pm"),
        .tasks = {.lock = MUTEX_INITIALIZER("acpi tasks"), .tail = &os.tasks.head},
        .allowpowerdown = 1};

int acpi_enabled(void) { return os.on; }

static int width(const struct acpi_gas *gas, int dflt) {
  if (gas != 0 && gas->access_size >= 1 && gas->access_size <= 4)
    return 1 << (gas->access_size - 1);
  return dflt;
}

static void map_regs(struct acpi_fadt *f) {
  struct pmreg *r = os.reg;
  int ext = f->header.revision >= 3 &&
            f->header.length >= __offsetof(struct acpi_fadt, x_gpe1_blk) + 12;
  int evt = f->pm1_evt_len >> 1;

#define SETLEGACY(i, a, sz, acc)                                               \
  do {                                                                         \
    r[i].space = GAS_SYSTEM_IOSPACE;                                           \
    r[i].addr = (a);                                                           \
    r[i].size = (sz);                                                          \
    r[i].access = (acc);                                                       \
  } while (0)
#define SETGAS(i, g, sz, dflt)                                                 \
  do {                                                                         \
    r[i].space = (g).address_space_id;                                         \
    r[i].addr = (g).address;                                                   \
    r[i].size = (sz);                                                          \
    r[i].access = width(&(g), dflt);                                           \
  } while (0)

  if (f->smi_cmd)
    SETLEGACY(R_SMI, f->smi_cmd, 1, 1);
  if (ext && f->x_pm1a_evt_blk.address) {
    SETGAS(R_PM1A_STS, f->x_pm1a_evt_blk, evt, 2);
    SETGAS(R_PM1A_EN, f->x_pm1a_evt_blk, evt, 2);
    r[R_PM1A_EN].addr += (uint)evt;
  } else if (f->pm1a_evt_blk) {
    SETLEGACY(R_PM1A_STS, f->pm1a_evt_blk, evt, 2);
    SETLEGACY(R_PM1A_EN, f->pm1a_evt_blk + (uint)evt, evt, 2);
  }
  if (ext && f->x_pm1b_evt_blk.address) {
    SETGAS(R_PM1B_STS, f->x_pm1b_evt_blk, evt, 2);
    SETGAS(R_PM1B_EN, f->x_pm1b_evt_blk, evt, 2);
    r[R_PM1B_EN].addr += (uint)evt;
  } else if (f->pm1b_evt_blk) {
    SETLEGACY(R_PM1B_STS, f->pm1b_evt_blk, evt, 2);
    SETLEGACY(R_PM1B_EN, f->pm1b_evt_blk + (uint)evt, evt, 2);
  }
  if (ext && f->x_pm1a_cnt_blk.address)
    SETGAS(R_PM1A_CNT, f->x_pm1a_cnt_blk, f->pm1_cnt_len, 2);
  else if (f->pm1a_cnt_blk)
    SETLEGACY(R_PM1A_CNT, f->pm1a_cnt_blk, f->pm1_cnt_len, 2);
  if (ext && f->x_pm1b_cnt_blk.address)
    SETGAS(R_PM1B_CNT, f->x_pm1b_cnt_blk, f->pm1_cnt_len, 2);
  else if (f->pm1b_cnt_blk)
    SETLEGACY(R_PM1B_CNT, f->pm1b_cnt_blk, f->pm1_cnt_len, 2);
  if (ext && f->x_pm2_cnt_blk.address)
    SETGAS(R_PM2_CNT, f->x_pm2_cnt_blk, f->pm2_cnt_len, 1);
  else if (f->pm2_cnt_blk)
    SETLEGACY(R_PM2_CNT, f->pm2_cnt_blk, f->pm2_cnt_len, f->pm2_cnt_len);
  if (ext && f->x_gpe0_blk.address) {
    SETGAS(R_GPE0_STS, f->x_gpe0_blk, f->gpe0_blk_len >> 1, 1);
    SETGAS(R_GPE0_EN, f->x_gpe0_blk, f->gpe0_blk_len >> 1, 1);
    r[R_GPE0_EN].addr += (uint)(f->gpe0_blk_len >> 1);
  } else if (f->gpe0_blk) {
    SETLEGACY(R_GPE0_STS, f->gpe0_blk, f->gpe0_blk_len >> 1, 1);
    SETLEGACY(R_GPE0_EN, f->gpe0_blk + (uint)(f->gpe0_blk_len >> 1),
              f->gpe0_blk_len >> 1, 1);
  }
  if (ext && f->x_gpe1_blk.address) {
    SETGAS(R_GPE1_STS, f->x_gpe1_blk, f->gpe1_blk_len >> 1, 1);
    SETGAS(R_GPE1_EN, f->x_gpe1_blk, f->gpe1_blk_len >> 1, 1);
    r[R_GPE1_EN].addr += (uint)(f->gpe1_blk_len >> 1);
  } else if (f->gpe1_blk) {
    SETLEGACY(R_GPE1_STS, f->gpe1_blk, f->gpe1_blk_len >> 1, 1);
    SETLEGACY(R_GPE1_EN, f->gpe1_blk + (uint)(f->gpe1_blk_len >> 1),
              f->gpe1_blk_len >> 1, 1);
  }
#undef SETLEGACY
#undef SETGAS
}

static int pm_read(int reg, int offset) {
  struct pmreg *r = &os.reg[reg];
  int n = r->size < r->access ? r->size : r->access;
  uint32_t v = 0;

  if (r->size == 0)
    return 0;
  if (n > 4)
    n = 4;
  if (r->space == GAS_SYSTEM_IOSPACE) {
    uint16_t port = (uint16_t)(r->addr + (uint64_t)offset);

    switch (n) {
    case 1:
      return inb(port);
    case 2:
      return inw(port);
    default:
      return (int)inl(port);
    }
  }
  acpi_gasio(ACPI_IOREAD, r->space, r->addr + (uint64_t)offset, n, n, &v);
  return (int)v;
}

static void pm_write(int reg, int offset, int val) {
  struct pmreg *r = &os.reg[reg];
  int n = r->size < r->access ? r->size : r->access;
  uint32_t v = (uint32_t)val;

  if (r->size == 0)
    return;
  if (n > 4)
    n = 4;
  if (r->space == GAS_SYSTEM_IOSPACE) {
    uint16_t port = (uint16_t)(r->addr + (uint64_t)offset);

    switch (n) {
    case 1:
      outb(port, (uint8_t)val);
      break;
    case 2:
      outw(port, (uint16_t)val);
      break;
    default:
      outl(port, v);
    }
    return;
  }
  acpi_gasio(ACPI_IOWRITE, r->space, r->addr + (uint64_t)offset, n, n, &v);
}

static int pm1_sts(void) { return pm_read(R_PM1A_STS, 0) | pm_read(R_PM1B_STS, 0); }
static int pm1_en(void) { return pm_read(R_PM1A_EN, 0) | pm_read(R_PM1B_EN, 0); }
static int pm1_cnt(void) { return pm_read(R_PM1A_CNT, 0) | pm_read(R_PM1B_CNT, 0); }

static void pm1_sts_write(int v) {
  pm_write(R_PM1A_STS, 0, v);
  pm_write(R_PM1B_STS, 0, v);
}

static void pm1_en_write(int v) {
  pm_write(R_PM1A_EN, 0, v);
  pm_write(R_PM1B_EN, 0, v);
}

// TASKS

void acpi_task_add(void (*fn)(void *, int), void *arg0, int arg1) {
  struct task *t = malloc(sizeof(*t), M_ACPI, M_NOWAIT);

  if (t == 0) {
    printf("acpi: no memory for a task\n");
    return;
  }
  t->fn = fn;
  t->arg0 = arg0;
  t->arg1 = arg1;
  t->next = 0;
  mtx_enter(&os.tasks.lock);
  *os.tasks.tail = t;
  os.tasks.tail = &t->next;
  mtx_leave(&os.tasks.lock);
  wakeup(&os);
}

int acpi_task_run(void) {
  struct task *t;

  mtx_enter(&os.tasks.lock);
  if ((t = os.tasks.head) != 0 && (os.tasks.head = t->next) == 0)
    os.tasks.tail = &os.tasks.head;
  mtx_leave(&os.tasks.lock);
  if (t == 0)
    return 0;
  t->fn(t->arg0, t->arg1);
  free(t, M_ACPI);
  return 1;
}

static int sci_intr(void *arg);

int acpi_tasks_pending(void) {
  int n;

  mtx_enter(&os.tasks.lock);
  n = os.tasks.head != 0;
  mtx_leave(&os.tasks.lock);
  return n;
}

// what the hardware latched while it slept, queued now
void acpi_sci_poll(void) {
  uint64_t flags = readeflags();

  if (!os.events)
    return;
  cli();
  sci_intr(0);
  if (flags & FL_IF)
    sti();
}

void acpi_poll_now(void) {
  struct poll *p;

  for (p = os.polls; p != 0; p = p->link)
    p->next = ticks;
  wakeup(&os);
}

void acpi_poll_register(void (*fn)(void *), void *arg) {
  acpi_poll_every(fn, arg, POLL_SECONDS * 10);
}

void acpi_poll_every(void (*fn)(void *), void *arg, int tenths) {
  uint period = (uint)(tenths > 0 ? tenths : 1) * HZ / 10;
  struct poll *p;

  if (period == 0)
    period = 1;
  for (p = os.polls; p != 0; p = p->link)
    if (p->fn == fn && p->arg == arg)
      break;
  if (p == 0) {
    p = malloc(sizeof(*p), M_ACPI, M_WAITOK | M_ZERO);
    p->fn = fn;
    p->arg = arg;
    p->link = os.polls;
    os.polls = p;
  }
  p->period = period;
  p->next = ticks + period;
  wakeup(&os);
}

// GPES

static void gpe_blocks(void) {
  int b;

  os.nblk = 0;
  if (os.reg[R_GPE0_STS].size) {
    os.blk[os.nblk].sts = R_GPE0_STS;
    os.blk[os.nblk].en = R_GPE0_EN;
    os.blk[os.nblk].base = 0;
    os.blk[os.nblk].bytes = os.reg[R_GPE0_STS].size;
    os.nblk++;
  }
  if (os.reg[R_GPE1_STS].size) {
    os.blk[os.nblk].sts = R_GPE1_STS;
    os.blk[os.nblk].en = R_GPE1_EN;
    os.blk[os.nblk].base = os.fadt->gpe1_base;
    os.blk[os.nblk].bytes = os.reg[R_GPE1_STS].size;
    os.nblk++;
  }
  os.ngpe = 0;
  for (b = 0; b < os.nblk; b++)
    if (os.blk[b].base + 8 * os.blk[b].bytes > os.ngpe)
      os.ngpe = os.blk[b].base + 8 * os.blk[b].bytes;
}

static int gpe_where(int gpe, int *b, int *off) {
  for (*b = 0; *b < os.nblk; (*b)++)
    if (gpe >= os.blk[*b].base && gpe < os.blk[*b].base + 8 * os.blk[*b].bytes) {
      *off = (gpe - os.blk[*b].base) >> 3;
      return 0;
    }
  return -1;
}

static int gpe_bit(int gpe, int b) { return 1 << ((gpe - os.blk[b].base) & 7); }

void acpi_gpe_enable(int gpe) {
  int b, off;

  if (gpe_where(gpe, &b, &off) != 0)
    return;
  mtx_enter(&os.pm);
  pm_write(os.blk[b].en, off, pm_read(os.blk[b].en, off) | gpe_bit(gpe, b));
  mtx_leave(&os.pm);
}

static void gpes_off(void) {
  int b, off;

  for (b = 0; b < os.nblk; b++)
    for (off = 0; off < os.blk[b].bytes; off++) {
      pm_write(os.blk[b].en, off, 0);
      pm_write(os.blk[b].sts, off, 0xff);
    }
}

int acpi_gpe_handler(int gpe, int (*fn)(int, void *), void *arg, int flags) {
  if (gpe < 0 || gpe >= os.ngpe || fn == 0)
    return -1;
  if (!(flags & (GPE_LEVEL | GPE_EDGE)) || ((flags & GPE_LEVEL) && (flags & GPE_EDGE)))
    return -1;
  if (os.gpe[gpe].fn != 0)
    printf("acpi: GPE 0x%x already has a handler\n", gpe);
  os.gpe[gpe].fn = fn;
  os.gpe[gpe].arg = arg;
  os.gpe[gpe].flags = flags;
  return 0;
}

void acpi_gpe_done(int gpe) {
  int b, off, mask;

  if (gpe_where(gpe, &b, &off) != 0)
    return;
  mask = gpe_bit(gpe, b);
  mtx_enter(&os.pm);
  if (os.gpe[gpe].flags & GPE_LEVEL)
    pm_write(os.blk[b].sts, off, mask);
  pm_write(os.blk[b].en, off, pm_read(os.blk[b].en, off) | mask);
  mtx_leave(&os.pm);
}

static int gpe_method(int gpe, void *arg) {
  aml_eval((struct aml_node *)arg, 0, 0, 0);
  acpi_gpe_done(gpe);
  return 0;
}

static void gpe_task(void *arg0 __unused, int gpe) {
  struct gpe *g = &os.gpe[gpe];

  if (g->fn != 0 && g->active) {
    g->active = 0;
    g->fn(gpe, g->arg);
  }
}

static void init_gpes(void) {
  char name[16];
  int idx;

  gpe_blocks();
  os.gpe = malloc((size_t)(os.ngpe ? os.ngpe : 1) * sizeof(struct gpe), M_ACPI,
                  M_WAITOK | M_ZERO);
  gpes_off();
  for (idx = 0; idx < os.ngpe; idx++) {
    struct aml_node *n;

    snprintf(name, sizeof(name), "\\_GPE._L%02X", idx);
    if ((n = aml_search(aml_root(), name)) != 0) {
      acpi_gpe_handler(idx, gpe_method, n, GPE_LEVEL);
      continue;
    }
    snprintf(name, sizeof(name), "\\_GPE._E%02X", idx);
    if ((n = aml_search(aml_root(), name)) != 0)
      acpi_gpe_handler(idx, gpe_method, n, GPE_EDGE);
  }
}

static void enable_rungpes(void) {
  int idx;

  for (idx = 0; idx < os.ngpe; idx++)
    if (os.gpe[idx].fn != 0)
      acpi_gpe_enable(idx);
}

void acpi_glk_release_signal(void) {
  mtx_enter(&os.pm);
  pm_write(R_PM1A_CNT, 0, pm_read(R_PM1A_CNT, 0) | ACPI_PM1_GBL_RLS);
  pm_write(R_PM1B_CNT, 0, pm_read(R_PM1B_CNT, 0) | ACPI_PM1_GBL_RLS);
  mtx_leave(&os.pm);
}

// THE POWER BUTTON

void acpi_powerbutton(void) {
  if (os.allowpowerdown == 1) {
    os.allowpowerdown = 0;
    kill(1, FNU_SIG_USER2);
  }
}

static void pbtn_task(void *arg0 __unused, int dummy __unused) {
  mtx_enter(&os.pm);
  pm1_en_write(pm1_en() | ACPI_PM1_PWRBTN_EN);
  mtx_leave(&os.pm);
  acpi_power_pressed();
}

static void sbtn_task(void *arg0 __unused, int dummy __unused) {
  mtx_enter(&os.pm);
  pm1_en_write(pm1_en() | ACPI_PM1_SLPBTN_EN);
  mtx_leave(&os.pm);
  acpi_sleep_pressed();
}

// THE SCI

static int sci_intr(void *arg __unused) {
  int processed = 0, b, idx, jdx, sts, en;

  if (!os.events)
    return 0;
  for (b = 0; b < os.nblk; b++)
    for (idx = 0; idx < os.blk[b].bytes; idx++) {
      sts = pm_read(os.blk[b].sts, idx);
      en = pm_read(os.blk[b].en, idx);
      if (!(en & sts))
        continue;
      pm_write(os.blk[b].en, idx, en & ~sts);
      for (jdx = 0; jdx < 8; jdx++) {
        int gpe = os.blk[b].base + 8 * idx + jdx;

        if (!(en & sts & (1 << jdx)))
          continue;
        os.gpe[gpe].active = 1;
        acpi_task_add(gpe_task, 0, gpe);
        if (os.gpe[gpe].flags & GPE_EDGE)
          pm_write(os.blk[b].sts, idx, 1 << jdx);
        processed = 1;
      }
    }
  sts = pm1_sts();
  en = pm1_en();
  if (sts & en) {
    sts &= en;
    if (sts & ACPI_PM1_PWRBTN_STS) {
      pm1_en_write(en & ~ACPI_PM1_PWRBTN_EN);
      pm1_sts_write(ACPI_PM1_PWRBTN_STS);
      sts &= ~ACPI_PM1_PWRBTN_STS;
      acpi_task_add(pbtn_task, 0, 0);
    }
    if (sts & ACPI_PM1_SLPBTN_STS) {
      pm1_en_write(en & ~ACPI_PM1_SLPBTN_EN);
      pm1_sts_write(ACPI_PM1_SLPBTN_STS);
      sts &= ~ACPI_PM1_SLPBTN_STS;
      acpi_task_add(sbtn_task, 0, 0);
    }
    if (sts & ACPI_PM1_RTC_STS) {
      pm1_en_write(en & ~ACPI_PM1_RTC_EN);
      pm1_sts_write(ACPI_PM1_RTC_STS);
      sts &= ~ACPI_PM1_RTC_STS;
    }
    if (sts) {
      printf("acpi: PM1 stuck (en 0x%x st 0x%x), clearing\n", en, sts);
      pm1_en_write(en & ~sts);
      pm1_sts_write(sts);
    }
    processed = 1;
  }
  return processed;
}

static int sci_setup(void) {
  int gsi, level, low, vector;

  acpi_isa_irq(os.fadt->sci_int, 1, &gsi, &level, &low);
  if ((vector = ioapic_gsi_vector(gsi, 0, level, low)) < 0 ||
      intr_establish(vector, sci_intr, 0, "acpi") != 0)
    return -1;
  os.sci = gsi;
  return 0;
}

// THE THREAD

static uint run_polls(void) {
  uint wait = POLL_SECONDS * HZ;
  struct poll *p;

  for (p = os.polls; p != 0; p = p->link) {
    if (ticks >= p->next) {
      p->next = ticks + p->period;
      p->fn(p->arg);
    }
    if (p->next > ticks && p->next - ticks < wait)
      wait = (uint)(p->next - ticks);
  }
  return wait;
}

static void acpi_thread(void *arg __unused) {
  uint wait;
  int en;

  sched_peg_curproc(&cpus[0]);

  if (os.events) {
    mtx_enter(&os.pm);
    en = pm1_en();
    if (!(os.fadt->flags & FADT_PWR_BUTTON))
      en |= ACPI_PM1_PWRBTN_EN;
    if (!(os.fadt->flags & FADT_SLP_BUTTON))
      en |= ACPI_PM1_SLPBTN_EN;
    pm1_en_write(en);
    mtx_leave(&os.pm);
    enable_rungpes();
  }
  for (;;) {
    while (acpi_task_run())
      ;
    wait = run_polls();
    sleep_setup(&os, PWAIT, "acpi");
    mtx_enter(&os.tasks.lock);
    en = os.tasks.head == 0;
    mtx_leave(&os.tasks.lock);
    sleep_finish((uint64_t)wait * (1000000000ull / HZ), en);
  }
}

void acpi_thread_start(void) {
  if (!os.on)
    return;
  kthread_create(acpi_thread, 0, &os.thread, "acpi");
}

// THE MACHINE'S OWN

int64_t acpi_getsta(struct aml_node *node) {
  int64_t sta;

  if (aml_eval_int(node, "_STA", 0, 0, &sta))
    sta = STA_PRESENT | STA_ENABLED | STA_SHOW_UI | STA_DEV_OK;
  return sta;
}

// a slow step: how long, how many SMIs. blame SMM
struct slow {
  uint64_t tsc;
  int64_t smi;
};

static char step[64];

static void slow_begin(struct slow *w, const char *what) {
  strlcpy(step, what, sizeof(step));
  boot_step = step;
  w->tsc = rdtsc();
  w->smi = smi_count();
}

static void slow_end(struct slow *w) {
  const char *what = step;
  uint64_t hz = delay_tsc_hz(), ms;

  boot_step = "";
  if (hz == 0 || (ms = (rdtsc() - w->tsc) * 1000 / hz) < 500)
    return;
  if (w->smi >= 0)
    printf("acpi: %s took %lu ms, %ld SMIs\n", what, (unsigned long)ms,
           (long)(smi_count() - w->smi));
  else
    printf("acpi: %s took %lu ms\n", what, (unsigned long)ms);
}

static void inidev(struct aml_node *parent) {
  struct aml_node *node, *ini;
  struct slow w;
  int64_t sta;

  for (node = parent->son; node != 0; node = node->sib) {
    if (node->value == 0 || node->value->type != AML_DEVICE)
      continue;
    slow_begin(&w, aml_nodename(node));
    sta = acpi_getsta(node);
    slow_end(&w);
    if (!(sta & (STA_PRESENT | STA_DEV_OK)))
      continue;
    if ((sta & STA_PRESENT) && (ini = aml_search(node, "_INI")) != 0) {
      slow_begin(&w, aml_nodename(ini));
      aml_eval(ini, 0, 0, 0);
      slow_end(&w);
    }
    inidev(node);
  }
}

static void init_states(void) {
  struct aml_value res;
  char name[8];
  int i;

  printf("acpi: sleep states");
  for (i = ACPI_STATE_S0; i <= ACPI_STATE_S5; i++) {
    os.sleeptype[i].typa = -1;
    os.sleeptype[i].typb = -1;
    snprintf(name, sizeof(name), "_S%d_", i);
    if (aml_eval_name(aml_root(), name, 0, 0, &res) != 0)
      continue;
    if (res.type != AML_PACKAGE || res.length < 2) {
      aml_freevalue(&res);
      continue;
    }
    os.sleeptype[i].typa = (int)aml_val2int(res.pkg[0]);
    os.sleeptype[i].typb = (int)aml_val2int(res.pkg[1]);
    aml_freevalue(&res);
    printf(" S%d", i);
  }
  printf("\n");
}

static int enable_acpi(void) {
  int i;

  if (pm1_cnt() & ACPI_PM1_SCI_EN)
    return 0;
  if (os.fadt->smi_cmd == 0 || os.fadt->acpi_enable == 0)
    return -1;
  pm_write(R_SMI, 0, os.fadt->acpi_enable);
  for (i = 0; i < 100; i++) {
    if (pm1_cnt() & ACPI_PM1_SCI_EN)
      return 0;
    delay(5000);
  }
  return -1;
}

void acpi_start(void) {
  struct acpi_sdt_header *dsdt = acpi_dsdt();
  struct aml_value arg;
  struct aml_node *sb;
  struct slow w;
  int i, n, r;

  acpi_tables_attach();
  os.fadt = acpi_fadt();
  if (os.fadt == 0 || dsdt == 0) {
    printf("acpi: no FADT or no DSDT, no OSPM\n");
    return;
  }
  if (os.fadt->flags & FADT_HW_REDUCED_ACPI) {
    printf("acpi: hardware-reduced ACPI is not run\n");
    return;
  }
  map_regs(os.fadt);
  os.events = 1;
  for (i = 0; i < R_NREG; i++)
    if (os.reg[i].size != 0 && os.reg[i].space != GAS_SYSTEM_IOSPACE)
      os.events = 0;

  if (!(pm1_cnt() & ACPI_PM1_SCI_EN) &&
      (os.fadt->smi_cmd == 0 ||
       (os.fadt->acpi_enable == 0 && os.fadt->acpi_disable == 0))) {
    printf("acpi: control of the hardware is not to be had\n");
    return;
  }

  hwlat_report("acpi");
  aml_hashopcodes();
  aml_init();
  aml_intlen = dsdt->revision < 2 ? 32 : 64;

  {
    uint len = dsdt->length - (uint)sizeof(*dsdt);
    uint8_t *aml = malloc(len + ACPI_AML_PAD, M_ACPI, M_WAITOK | M_ZERO);

    memcpy(aml, (uint8_t *)(dsdt + 1), len);
    aml_load(aml, len, 0);
  }
  n = acpi_table_count();
  for (i = 0; i < n; i++) {
    struct acpi_sdt_header *t = acpi_table(i);

    if (memcmp(t->signature, "SSDT", 4) == 0)
      aml_load(acpi_table_aml(i), t->length - (uint)sizeof(*t), 0);
  }
  aml_fixup();

  init_states();
  os.pts = aml_search(aml_root(), "_PTS");
  init_gpes();

  slow_begin(&w, "the switch to ACPI mode");
  r = enable_acpi();
  slow_end(&w);
  if (r != 0) {
    printf("acpi: cannot enable ACPI\n");
    return;
  }
  os.on = 1;
  printf("acpi: ACPI %d.%d, tables", os.fadt->header.revision,
          os.fadt->header.revision > 4 ? os.fadt->fadt_minor : 0);
  printf(" DSDT");
  for (i = 0; i < n; i++) {
    const char *sig = acpi_table(i)->signature;

    printf(" %c%c%c%c", sig[0], sig[1], sig[2], sig[3]);
  }
  printf("\n");

  memset(&arg, 0, sizeof(arg));
  arg.type = AML_INTEGER;
  arg.integer = 1;
  slow_begin(&w, "\\_PIC");
  aml_eval_name(aml_root(), "_PIC", 1, &arg, 0);
  slow_end(&w);

  slow_begin(&w, "the embedded controller");
  acpi_ec_ecdt();
  acpi_attach_early();
  slow_end(&w);
  if ((sb = aml_search(aml_root(), "_SB_")) != 0) {
    slow_begin(&w, "\\_SB_._INI");
    aml_eval_name(sb, "_INI", 0, 0, 0);
    slow_end(&w);
    inidev(sb);
  }
  slow_begin(&w, "interrupt routing and processors");
  acpi_prt_init();
  acpi_cpu_attach();
  slow_end(&w);
  slow_begin(&w, "the devices");
  acpi_attach_devices();
  slow_end(&w);

  if (sci_setup() != 0)
    printf("acpi: no SCI, events are not heard\n");
  acpi_sleep_sysctl();
  sysctl_add_int("kern.allowpowerdown", &os.allowpowerdown, FNU_CTLF_WRITE, 0, 1);
}

// SLEEP

int acpi_sleep_supported(int state) {
  return os.on && state >= 0 && state <= ACPI_STATE_S5 &&
         os.sleeptype[state].typa >= 0;
}

int acpi_sleep_registers(int state) {
  int rega, regb, i;

  pm_write(R_PM1A_STS, 0, ACPI_PM1_WAK_STS);
  pm_write(R_PM1B_STS, 0, ACPI_PM1_WAK_STS);
  rega = pm_read(R_PM1A_CNT, 0) & ~(ACPI_PM1_SLP_TYP_MASK | ACPI_PM1_SLP_EN);
  regb = pm_read(R_PM1B_CNT, 0) & ~(ACPI_PM1_SLP_TYP_MASK | ACPI_PM1_SLP_EN);
  rega |= os.sleeptype[state].typa << ACPI_PM1_SLP_TYP_SHIFT;
  regb |= os.sleeptype[state].typb << ACPI_PM1_SLP_TYP_SHIFT;
  pm_write(R_PM1A_CNT, 0, rega);
  pm_write(R_PM1B_CNT, 0, regb);
  wbinvd();
  pm_write(R_PM1A_CNT, 0, rega | ACPI_PM1_SLP_EN);
  pm_write(R_PM1B_CNT, 0, regb | ACPI_PM1_SLP_EN);
  // S1 comes back here; a deeper state never does
  for (i = 0; i < 100000; i++) {
    if (pm1_sts() & ACPI_PM1_WAK_STS)
      return 0;
    delay(10);
  }
  return -1;
}

void acpi_sleep_events(void) {
  mtx_enter(&os.pm);
  gpes_off();
  pm1_sts_write(0xffff);
  pm1_en_write(ACPI_PM1_PWRBTN_EN | ACPI_PM1_SLPBTN_EN);
  mtx_leave(&os.pm);
}

// what woke us. first, before something eats it
void acpi_wake_capture(void) {
  int b, off, bit;

  os.wake_pm1 = pm1_sts();
  os.wake_gpe = -1;
  for (b = 0; b < os.nblk && os.wake_gpe < 0; b++)
    for (off = 0; off < os.blk[b].bytes && os.wake_gpe < 0; off++) {
      int live = pm_read(os.blk[b].sts, off) & pm_read(os.blk[b].en, off);

      for (bit = 0; bit < 8; bit++)
        if (live & (1 << bit)) {
          os.wake_gpe = os.blk[b].base + off * 8 + bit;
          break;
        }
    }
}

// why, in words
const char *acpi_wake_reason(char *buf, size_t n) {
  if (os.wake_pm1 & ACPI_PM1_PWRBTN_STS)
    return "the power button";
  if (os.wake_pm1 & ACPI_PM1_SLPBTN_STS)
    return "the sleep button";
  if (os.wake_pm1 & ACPI_PM1_RTC_STS)
    return "the rtc alarm";
  if (os.wake_gpe >= 0) {
    snprintf(buf, n, "gpe 0x%x", os.wake_gpe);
    return buf;
  }
  return "no event it knows";
}

void acpi_wake_registers(void) {
  int en;

  enable_acpi();
  mtx_enter(&os.pm);
  pm1_sts_write(0xffff);
  gpes_off();
  en = 0;
  if (!(os.fadt->flags & FADT_PWR_BUTTON))
    en |= ACPI_PM1_PWRBTN_EN;
  if (!(os.fadt->flags & FADT_SLP_BUTTON))
    en |= ACPI_PM1_SLPBTN_EN;
  pm1_en_write(en);
  mtx_leave(&os.pm);
  enable_rungpes();
}

// POWER OFF AND RESET

// _PTS, _SST, gpes off. a reboot only stops the interpreter
void acpi_poweroff_prepare(int off) {
  struct aml_value arg;

  if (!os.on)
    return;
  if (off) {
    if (acpi_pm_trace_on())
      printf("acpi: _PTS(5)\n");
    aml_set_int(os.pts, ACPI_STATE_S5);
    memset(&arg, 0, sizeof(arg));
    arg.type = AML_INTEGER;
    arg.integer = 0;
    aml_eval_name(aml_root(), "\\_SI_._SST", 1, &arg, 0);
    gpes_off();
  }
  aml_halt();
  os.halting = 1;
}

void acpi_poweroff(void) {
  int s5 = ACPI_STATE_S5, rega, regb, tr;

  cli();
  if (!os.on || os.sleeptype[s5].typa < 0) {
    printf("ACPI: no S5 description, cannot power off\n");
    for (;;)
      hlt();
  }
  tr = acpi_pm_trace_on();

  pm_write(R_PM1A_STS, 0, ACPI_PM1_WAK_STS);
  pm_write(R_PM1B_STS, 0, ACPI_PM1_WAK_STS);
  rega = pm_read(R_PM1A_CNT, 0) & ~(ACPI_PM1_SLP_TYP_MASK | ACPI_PM1_SLP_EN);
  regb = pm_read(R_PM1B_CNT, 0) & ~(ACPI_PM1_SLP_TYP_MASK | ACPI_PM1_SLP_EN);
  rega |= os.sleeptype[s5].typa << ACPI_PM1_SLP_TYP_SHIFT;
  regb |= os.sleeptype[s5].typb << ACPI_PM1_SLP_TYP_SHIFT;
  if (tr)
    printf("acpi: SLP_TYP 0x%x/0x%x\n", rega, regb);
  pm_write(R_PM1A_CNT, 0, rega);
  pm_write(R_PM1B_CNT, 0, regb);
  rega |= ACPI_PM1_SLP_EN;
  regb |= ACPI_PM1_SLP_EN;
  if (tr)
    printf("acpi: SLP_EN\n");
  wbinvd();
  pm_write(R_PM1A_CNT, 0, rega);
  pm_write(R_PM1B_CNT, 0, regb);
  delay(1000000);
  printf("ACPI: the firmware refused soft-off\n");
  for (;;)
    hlt();
}

int acpi_reset_describe(uint32_t *space, uint64_t *address, uint32_t *value) {
  struct acpi_fadt *f = acpi_fadt();

  if (f == 0 || f->header.revision < 2 ||
      f->header.length < __offsetof(struct acpi_fadt, reset_value) + 1)
    return -1;
  *space = f->reset_reg.address_space_id;
  *address = f->reset_reg.address;
  *value = f->reset_value;
  return (f->flags & FADT_RESET_REG_SUP) ? 0 : 1;
}

int acpi_reset_via_fadt(void) {
  struct acpi_fadt *f = acpi_fadt();
  uint32_t v;
  int len;

  if (f == 0 || f->header.revision < 2 || !(f->flags & FADT_RESET_REG_SUP) ||
      f->reset_reg.address == 0)
    return -1;
  v = f->reset_value;
  len = f->reset_reg.register_bit_width / 8;
  if (len == 0)
    len = 1;
  acpi_gasio(ACPI_IOWRITE, f->reset_reg.address_space_id, f->reset_reg.address,
             len, len, &v);
  delay(100000);
  return 0;
}
