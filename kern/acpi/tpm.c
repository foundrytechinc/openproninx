// TRUSTED PLATFORM MODULE
// told of sleep and power off; CRB or TIS
// random numbers go to the pool

#include "acpi.h"
#include "aml.h"
#include "callout.h"
#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "malloc.h"
#include "param.h"
#include "sleeplock.h"
#include "task.h"
#include "uio.h"

// the TPM2 table's start methods
#define SM_ACPI 2
#define SM_TIS 6
#define SM_CRB 7
#define SM_CRB_ACPI 8

struct tpm2_table {
  struct acpi_sdt_header hdr;
  uint16_t platform_class;
  uint16_t reserved;
  uint64_t control;
  uint32_t start_method;
} __packed;

// TIS, locality 0
#define TIS_ACCESS 0x00
#define TIS_INT_ENABLE 0x08
#define TIS_STS 0x18
#define TIS_DATA 0x24
#define TIS_DID_VID 0xf00
#define ACCESS_VALID 0x80
#define ACCESS_ACTIVE 0x20
#define ACCESS_REQUEST 0x02
#define STS_VALID 0x80
#define STS_READY 0x40
#define STS_GO 0x20
#define STS_DATA 0x10
#define STS_EXPECT 0x08

// CRB, locality 0, then the control area
#define CRB_LOC_STATE 0x00
#define CRB_LOC_CTRL 0x08
#define CRB_REQ 0x00
#define CRB_STS 0x04
#define CRB_CANCEL 0x08
#define CRB_START 0x0c
#define CRB_INT_ENABLE 0x10
#define CRB_CMD_SIZE 0x18
#define CRB_CMD_LADDR 0x1c
#define CRB_CMD_HADDR 0x20
#define CRB_RSP_SIZE 0x24
#define CRB_RSP_ADDR 0x28
#define LOC_STATE_VALID 0x80
#define LOC_STATE_ASSIGNED 0x02
#define LOC_CTRL_REQUEST 0x01
#define LOC_CTRL_RELINQUISH 0x02
#define REQ_READY 0x01
#define REQ_IDLE 0x02
#define STS_ERROR 0x01

// TCG PTP timeouts, and a command's longest
#define TIMEOUT_A 750
#define TIMEOUT_B 2000
#define TIMEOUT_C 750
#define TIMEOUT_CMD 5000

#define HEADER 10
#define BUFSIZE 64

#define EARLY_SIZE 32
#define HARVEST_SIZE 16
#define HARVEST_INTERVAL 10 // seconds

// return codes: TPM 1.2's, then 2.0's
#define RC_DEACTIVATED 0x06
#define RC_DISABLED 0x07
#define RC_INVALID_POSTINIT 0x26
#define RC_RETRY 0x800
#define RC_NEEDS_SELFTEST 0x801
#define RC_DOING_SELFTEST 0x802
#define RC2_INITIALIZE 0x100
#define RC2_YIELDED 0x908
#define RC2_TESTING 0x90a
#define RC2_RETRY 0x922

#define WINDOWS 3
#define DEVSIZE 4096 // /dev/tpm0, one command or answer

struct window {
  uint64_t base, len;
  volatile uint8_t *va;
};

static struct tpm {
  struct aml_node *node;
  struct window win[WINDOWS]; // the device's memory, from _CRS
  int nwin, idle;
  volatile uint8_t *regs; // locality 0
  volatile uint8_t *ctrl; // the CRB control area
  volatile uint8_t *cmd, *rsp;
  uint32_t cmd_size, rsp_size;
  int method, v2;
  char name[32];
  struct sleeplock lock; // one command at a time
  int asleep;            // its state saved: nothing more until the wake
  int complained;
  const char *fail;     // where the last transfer stopped, 0 if it did not
  uint32_t fregs[4];    // LOC_STATE REQ STS START where it stopped
  uint8_t rhdr[HEADER];
  uint32_t sts0, start1; // status before the command, start just after it
  int garbage;
  uint8_t chdr[HEADER];  // the command buffer read back
  uint8_t *dev;         // /dev/tpm0's answer
  int devlen, devopen;
  struct callout co;
  struct task task;
} *tpm;

static int harvest_on = 1, harvest_bytes;

static uint32_t rd4(volatile uint8_t *r, uint off) { return *(volatile uint32_t *)(r + off); }
static void wr4(volatile uint8_t *r, uint off, uint32_t v) { *(volatile uint32_t *)(r + off) = v; }
static uint8_t rd1(volatile uint8_t *r, uint off) { return r[off]; }
static void wr1(volatile uint8_t *r, uint off, uint8_t v) { r[off] = v; }

static int until4(volatile uint8_t *r, uint off, uint32_t mask, uint32_t want, int ms) {
  int us;

  for (us = 0; us <= ms * 1000; us += 50) {
    if ((rd4(r, off) & mask) == want)
      return 0;
    delay(50);
  }
  return -1;
}

static int until1(volatile uint8_t *r, uint off, uint8_t mask, uint8_t want, int ms) {
  int us;

  for (us = 0; us <= ms * 1000; us += 50) {
    if ((rd1(r, off) & mask) == want)
      return 0;
    delay(50);
  }
  return -1;
}

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// TIS

static int tis_burst(struct tpm *t) {
  uint32_t v;
  int ms;

  for (ms = 0; ms <= TIMEOUT_B; ms++) {
    if ((v = (rd4(t->regs, TIS_STS) >> 8) & 0xffff) != 0)
      return (int)v;
    delay(1000);
  }
  return 0;
}

static int tis_transmit(struct tpm *t, const uint8_t *in, int len, uint8_t *out, int max) {
  int i = 0, n, size, r = -1;

  wr1(t->regs, TIS_ACCESS, ACCESS_REQUEST);
  if (until1(t->regs, TIS_ACCESS, ACCESS_VALID | ACCESS_ACTIVE, ACCESS_VALID | ACCESS_ACTIVE,
             TIMEOUT_A) < 0)
    return -1;
  wr1(t->regs, TIS_STS, STS_READY);
  if (until1(t->regs, TIS_STS, STS_READY, STS_READY, TIMEOUT_B) < 0)
    goto out;
  while (i < len - 1) {
    if ((n = tis_burst(t)) == 0)
      goto out;
    for (; n > 0 && i < len - 1; n--)
      wr1(t->regs, TIS_DATA, in[i++]);
    if (until1(t->regs, TIS_STS, STS_VALID, STS_VALID, TIMEOUT_C) < 0 ||
        !(rd1(t->regs, TIS_STS) & STS_EXPECT))
      goto out;
  }
  wr1(t->regs, TIS_DATA, in[i]);
  if (until1(t->regs, TIS_STS, STS_VALID, STS_VALID, TIMEOUT_C) < 0 ||
      (rd1(t->regs, TIS_STS) & STS_EXPECT))
    goto out;
  wr1(t->regs, TIS_STS, STS_GO);
  if (until1(t->regs, TIS_STS, STS_VALID | STS_DATA, STS_VALID | STS_DATA, TIMEOUT_CMD) < 0)
    goto out;
  for (i = 0, size = HEADER; i < size && i < max;) {
    if ((n = tis_burst(t)) == 0)
      goto out;
    for (; n > 0 && i < size && i < max; n--)
      out[i++] = rd1(t->regs, TIS_DATA);
    if (i == HEADER)
      size = (int)be32(out + 2);
  }
  r = i >= HEADER ? i : -1;
out:
  wr1(t->regs, TIS_STS, STS_READY);
  wr1(t->regs, TIS_ACCESS, ACCESS_ACTIVE);
  return r;
}

// CRB

// _DSM 1 of the start UUID
static int crb_acpi_start(struct tpm *t) {
  static const uint8_t uuid[16] = {0xab, 0x6c, 0xbf, 0x6b, 0x63, 0x54, 0x14, 0x47,
                                   0xb7, 0xcd, 0xf0, 0x20, 0x3c, 0x03, 0x68, 0xd4};
  struct aml_value arg[4];

  memset(arg, 0, sizeof(arg));
  arg[0].type = AML_BUFFER;
  arg[0].length = sizeof(uuid);
  arg[0].buf = (uint8_t *)uuid;
  arg[1].type = AML_INTEGER;
  arg[1].integer = 1;
  arg[2].type = AML_INTEGER;
  arg[2].integer = 1;
  arg[3].type = AML_PACKAGE;
  return aml_eval_name(t->node, "_DSM", 4, arg, 0);
}

static int crb_note(struct tpm *t, const char *step) {
  t->fail = step;
  t->fregs[0] = t->regs != 0 ? rd4(t->regs, CRB_LOC_STATE) : 0;
  t->fregs[1] = rd4(t->ctrl, CRB_REQ);
  t->fregs[2] = rd4(t->ctrl, CRB_STS);
  t->fregs[3] = rd4(t->ctrl, CRB_START);
  return -1;
}

// locality, then cmdReady
// intel PTT forgets its registers when idle. пиздец
static int crb_wake(struct tpm *t) {
  if (t->regs != 0) {
    wr4(t->regs, CRB_LOC_CTRL, LOC_CTRL_REQUEST);
    if (until4(t->regs, CRB_LOC_STATE, LOC_STATE_VALID | LOC_STATE_ASSIGNED,
               LOC_STATE_VALID | LOC_STATE_ASSIGNED, TIMEOUT_C) < 0)
      return crb_note(t, "locality");
  }
  if (t->idle) {
    wr4(t->ctrl, CRB_REQ, REQ_READY);
    if (until4(t->ctrl, CRB_REQ, REQ_READY, 0, TIMEOUT_C) < 0) {
      crb_note(t, "cmdReady");
      if (t->regs != 0)
        wr4(t->regs, CRB_LOC_CTRL, LOC_CTRL_RELINQUISH);
      return -1;
    }
  }
  return 0;
}

static void crb_rest(struct tpm *t) {
  if (t->idle)
    wr4(t->ctrl, CRB_REQ, REQ_IDLE);
  if (t->regs != 0)
    wr4(t->regs, CRB_LOC_CTRL, LOC_CTRL_RELINQUISH);
}

static int crb_transmit(struct tpm *t, const uint8_t *in, int len, uint8_t *out, int max) {
  int i, size, r = -1;

  t->fail = 0;
  if ((uint32_t)len > t->cmd_size || crb_wake(t) < 0)
    return -1;
  if (rd4(t->ctrl, CRB_STS) & STS_ERROR) {
    crb_note(t, "error status");
    goto out;
  }
  wr4(t->ctrl, CRB_CANCEL, 0);
  t->sts0 = rd4(t->ctrl, CRB_STS);
  for (i = 0; i < len; i++)
    t->cmd[i] = in[i];
  __sync_synchronize();
  for (i = 0; i < HEADER; i++) {
    t->chdr[i] = t->cmd[i];
    if (t->rsp != t->cmd)
      t->rsp[i] = 0;
  }
  __sync_synchronize();
  wr4(t->ctrl, CRB_START, 1);
  t->start1 = rd4(t->ctrl, CRB_START);
  if (t->method != SM_CRB && crb_acpi_start(t) != 0) {
    crb_note(t, "_DSM start");
    goto out;
  }
  if (until4(t->ctrl, CRB_START, 1, 0, TIMEOUT_CMD) < 0) {
    crb_note(t, "start");
    wr4(t->ctrl, CRB_CANCEL, 1);
    until4(t->ctrl, CRB_START, 1, 0, TIMEOUT_B);
    wr4(t->ctrl, CRB_CANCEL, 0);
    goto out;
  }
  for (i = 0; i < HEADER && i < max; i++)
    out[i] = t->rsp[i];
  size = (int)be32(out + 2);
  for (; i < size && i < max && (uint32_t)i < t->rsp_size; i++)
    out[i] = t->rsp[i];
  r = i;
out:
  crb_rest(t);
  return r;
}

// GARBAGE: what a power cut leaves. right tag, right size, not our own echo
static int answer_ok(struct tpm *t, const uint8_t *in, const uint8_t *out, int n) {
  uint32_t tag = (uint32_t)out[0] << 8 | out[1];

  if (n < HEADER || be32(out + 2) != (uint32_t)n || memcmp(out, in, HEADER) == 0)
    return 0;
  return t->v2 ? tag == 0x8001 || tag == 0x8002 : tag >= 0x00c4 && tag <= 0x00c6;
}

// whatever was half done is dropped, the interface starts clean
static void reset(struct tpm *t) {
  if (t->method == SM_TIS) {
    wr1(t->regs, TIS_STS, STS_READY);
    wr1(t->regs, TIS_ACCESS, ACCESS_ACTIVE);
    return;
  }
  wr4(t->ctrl, CRB_CANCEL, 1);
  until4(t->ctrl, CRB_START, 1, 0, TIMEOUT_B);
  wr4(t->ctrl, CRB_CANCEL, 0);
  if (t->idle) {
    wr4(t->ctrl, CRB_REQ, REQ_IDLE);
    until4(t->ctrl, CRB_REQ, REQ_IDLE, 0, TIMEOUT_C);
  }
  if (t->regs != 0)
    wr4(t->regs, CRB_LOC_CTRL, LOC_CTRL_RELINQUISH);
}

// no answer: clean the interface, once more
static int exchange(struct tpm *t, const uint8_t *in, int len, uint8_t *out, int max) {
  int n, try;

  for (try = 0; try < 2; try++) {
    n = t->method == SM_TIS ? tis_transmit(t, in, len, out, max)
                            : crb_transmit(t, in, len, out, max);
    if (n >= HEADER)
      memcpy(t->rhdr, out, HEADER);
    if (answer_ok(t, in, out, n))
      return n;
    t->garbage++;
    reset(t);
  }
  return -1;
}

static int transmit(const uint8_t *in, int len, uint8_t *out, int max, uint32_t *rc) {
  int n = exchange(tpm, in, len, out, max);

  if (n < HEADER)
    return -1;
  *rc = be32(out + 6);
  return n;
}

// TPM_GetRandom, TPM2_GetRandom: what came, or -1
static int getrandom(uint8_t *dst, int want, uint32_t *rc) {
  uint8_t cmd[14], out[BUFSIZE];
  int n, len, at;

  memset(cmd, 0, sizeof(cmd));
  if (tpm->v2) {
    memcpy(cmd, "\x80\x01\x00\x00\x00\x0c\x00\x00\x01\x7b", 10);
    cmd[11] = (uint8_t)want;
    len = 12;
  } else {
    memcpy(cmd, "\x00\xc1\x00\x00\x00\x0e\x00\x00\x00\x46", 10);
    cmd[13] = (uint8_t)want;
    len = 14;
  }
  if ((n = transmit(cmd, len, out, sizeof(out), rc)) < 0 || *rc != 0)
    return -1;
  if (tpm->v2) {
    if (n < 12)
      return -1;
    len = out[10] << 8 | out[11];
    at = 12;
  } else {
    if (n < 14)
      return -1;
    len = (int)be32(out + 10);
    at = 14;
  }
  if (len < 0 || len > want || at + len > n)
    return -1;
  memcpy(dst, out + at, (size_t)len);
  memset(out, 0, sizeof(out));
  return len;
}

static int command(const char *bytes, int len) {
  uint8_t out[BUFSIZE];
  uint32_t rc;

  return transmit((const uint8_t *)bytes, len, out, sizeof(out), &rc) < 0 ? -1 : (int)rc;
}

static int busy(uint32_t rc) {
  return tpm->v2 ? rc == RC2_RETRY || rc == RC2_TESTING || rc == RC2_YIELDED
                 : rc == RC_RETRY || rc == RC_DOING_SELFTEST;
}

// where a failed command stopped, or what came back
static void why(struct tpm *t) {
  const uint8_t *h = t->rhdr;

  if (t->fail != 0)
    printf("acpi: %s: CRB stopped at %s: locality 0x%x, request 0x%x, status 0x%x, start 0x%x\n",
           t->name, t->fail, t->fregs[0], t->fregs[1], t->fregs[2], t->fregs[3]);
  else
    printf("acpi: %s: answer %02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", t->name, h[0], h[1],
           h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9]);
  h = t->chdr;
  if (t->method != SM_TIS)
    printf("acpi: %s: status 0x%x before, start 0x%x after, %s buffer, command %02x%02x "
           "%02x%02x%02x%02x %02x%02x%02x%02x\n",
           t->name, t->sts0, t->start1, t->cmd == t->rsp ? "one" : "two", h[0], h[1], h[2], h[3],
           h[4], h[5], h[6], h[7], h[8], h[9]);
}

// TPM_ContinueSelfTest: it tests on in the background
static void selftest(void) { command("\x00\xc1\x00\x00\x00\x0a\x00\x00\x00\x53", 10); }

// ???
static int early(uint8_t *dst, int want) {
  uint32_t rc = 0;
  int i, n, started = 0;

  for (i = 0; i < 20; i++) {
    if ((n = getrandom(dst, want, &rc)) > 0)
      return n;
    if (!started && rc == (tpm->v2 ? RC2_INITIALIZE : RC_INVALID_POSTINIT)) {
      printf("acpi: %s: the firmware did not start the TPM; TPM%s_Startup(CLEAR)\n",
             tpm->name, tpm->v2 ? "2" : "");
      if (tpm->v2)
        command("\x80\x01\x00\x00\x00\x0c\x00\x00\x01\x44\x00\x00", 12);
      else
        command("\x00\xc1\x00\x00\x00\x0c\x00\x00\x00\x99\x00\x01", 12);
      started = 1;
    } else if (!tpm->v2 && rc == RC_NEEDS_SELFTEST)
      selftest();
    else if (busy(rc))
      delay(100000);
    else
      break;
  }
  if (!tpm->v2 && (rc == RC_DISABLED || rc == RC_DEACTIVATED))
    printf("acpi: %s: the TPM is %s in the firmware, no randomness from it\n", tpm->name,
           rc == RC_DISABLED ? "disabled" : "deactivated");
  else
    printf("acpi: %s: TPM%s_GetRandom answered 0x%x, no randomness from it\n", tpm->name,
           tpm->v2 ? "2" : "", rc);
  why(tpm);
  return -1;
}

static void harvest_kick(void *arg) { task_add(systq, &((struct tpm *)arg)->task); }

static void harvest(void *arg) {
  struct tpm *t = arg;
  uint8_t buf[HARVEST_SIZE];
  uint32_t rc = 0;
  int n = -1;

  acquiresleep(&t->lock);
  if (!t->asleep && harvest_on && (n = getrandom(buf, sizeof(buf), &rc)) < 0) {
    if (!t->v2 && rc == RC_NEEDS_SELFTEST)
      selftest();
    else if (!busy(rc) && !t->complained) {
      t->complained = 1;
      printf("%s: TPM%s_GetRandom answered 0x%x\n", t->name, t->v2 ? "2" : "", rc);
      why(t);
    }
  } else if (n > 0 && t->complained) {
    t->complained = 0;
    printf("%s: the TPM answers again\n", t->name);
  }
  releasesleep(&t->lock);
  if (n > 0) {
    random_add(buf, (uint)n);
    harvest_bytes += n;
  }
  memset(buf, 0, sizeof(buf));
  callout_reset(&t->co, HARVEST_INTERVAL * HZ, harvest_kick, t);
}

// TPM2_Shutdown, or a 1.2's TPM_SaveState for a sleep
static void tell(int sleep) {
  static const uint8_t shutdown[2][12] = {
      {0x80, 0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x01, 0x45, 0x00, 0x00},
      {0x80, 0x01, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x01, 0x45, 0x00, 0x01}};
  static const uint8_t savestate[10] = {0x00, 0xc1, 0x00, 0x00, 0x00, 0x0a,
                                        0x00, 0x00, 0x00, 0x98};
  uint8_t out[BUFSIZE];
  const char *what;
  uint32_t rc;
  int r, try;

  if (tpm == 0 || (!tpm->v2 && !sleep))
    return;
  acquiresleep(&tpm->lock);
  tpm->asleep = 1;
  what = tpm->v2 ? (sleep ? "TPM2_Shutdown(STATE)" : "TPM2_Shutdown(CLEAR)") : "TPM_SaveState";
  // silence is the wire, not the TPM: three tries
  for (try = 0; try < 3; try++) {
    r = tpm->v2 ? transmit(shutdown[sleep], sizeof(shutdown[0]), out, sizeof(out), &rc)
                : transmit(savestate, sizeof(savestate), out, sizeof(out), &rc);
    if (r >= 0)
      break;
    delay(20000);
  }
  releasesleep(&tpm->lock);
  if (r < 0)
    printf("%s: %s went unanswered\n", tpm->name, what);
  else if (rc != 0)
    printf("%s: %s answered 0x%x\n", tpm->name, what, rc);
}

void acpi_tpm_suspend(void) { tell(1); }

void acpi_tpm_shutdown(void) { tell(0); }

// the firmware did Startup(STATE) already; a 1.2 retests itself
void acpi_tpm_resume(void) {
  if (tpm == 0)
    return;
  acquiresleep(&tpm->lock);
  tpm->asleep = 0;
  if (!tpm->v2)
    selftest();
  releasesleep(&tpm->lock);
}

// up to three memory windows
static int window(int idx, union acpi_resource *crs, void *arg) {
  struct tpm *t = arg;
  struct window *w;

  (void)idx;
  if (t->nwin == WINDOWS)
    return 0;
  w = &t->win[t->nwin];
  if (AML_CRSTYPE(crs) == LR_MEM32FIXED) {
    w->base = crs->lr_m32fixed._bas;
    w->len = crs->lr_m32fixed._len;
  } else if (AML_CRSTYPE(crs) == LR_MEM32) {
    w->base = crs->lr_m32._min;
    w->len = crs->lr_m32._len;
  } else if (AML_CRSTYPE(crs) == LR_QWORD && crs->lr_qword.type == LR_TYPE_MEMORY) {
    w->base = crs->lr_qword._min;
    w->len = crs->lr_qword._len;
  } else
    return 0;
  if (w->len != 0)
    t->nwin++;
  return 0;
}

static struct window *holder(struct tpm *t, uint64_t pa) {
  int i;

  for (i = 0; i < t->nwin; i++)
    if (pa >= t->win[i].base && pa < t->win[i].base + t->win[i].len)
      return &t->win[i];
  return 0;
}

static volatile uint8_t *map_in(struct window *w, uint64_t pa) {
  if (w->va == 0 && (w->va = ioremap(w->base, w->len)) == 0)
    return 0;
  return w->va + (pa - w->base);
}

// trust the ACPI region, not the registers. the registers lie
static uint32_t fit(struct tpm *t, struct window *w, uint64_t pa, uint32_t size) {
  if (pa + size <= w->base + w->len)
    return size;
  printf("acpi: %s: the ACPI region does not cover the whole TPM buffer\n", t->name);
  return (uint32_t)(w->base + w->len - pa);
}

// never write outside the device's memory
static int crb_setup(struct tpm *t, uint64_t control) {
  struct window *w, *cw, *rw;
  uint64_t cmd, rsp;
  int r = -1;

  if ((w = holder(t, control)) != 0)
    t->ctrl = map_in(w, control);
  else
    t->ctrl = ioremap(control, 0x30);
  if (t->ctrl == 0)
    return -1;
  t->idle = t->method == SM_CRB;
  if (t->method == SM_CRB) {
    if (w != 0 && control == w->base + 0x40)
      t->regs = map_in(w, w->base);
    else
      printf("acpi: %s: the TPM's locality registers are not where the table puts "
             "its control area\n", t->name);
  }
  if (crb_wake(t) < 0) {
    printf("acpi: %s: the TPM did not come ready\n", t->name);
    return -1;
  }
  wr4(t->ctrl, CRB_INT_ENABLE, 0);
  cmd = rd4(t->ctrl, CRB_CMD_LADDR) | (uint64_t)rd4(t->ctrl, CRB_CMD_HADDR) << 32;
  rsp = rd4(t->ctrl, CRB_RSP_ADDR) | (uint64_t)rd4(t->ctrl, CRB_RSP_ADDR + 4) << 32;
  t->cmd_size = rd4(t->ctrl, CRB_CMD_SIZE);
  t->rsp_size = rd4(t->ctrl, CRB_RSP_SIZE);
  crb_rest(t);
  cw = holder(t, cmd);
  rw = holder(t, rsp);
  if (cw == 0 || rw == 0) {
    printf("acpi: %s: TPM buffers at 0x%lx and 0x%lx are outside its memory\n", t->name,
           (unsigned long)cmd, (unsigned long)rsp);
    return -1;
  }
  t->cmd_size = fit(t, cw, cmd, t->cmd_size);
  t->rsp_size = fit(t, rw, rsp, t->rsp_size);
  if (t->cmd_size < HEADER || t->rsp_size < HEADER ||
      (cmd == rsp && t->cmd_size != t->rsp_size))
    goto out;
  t->cmd = map_in(cw, cmd);
  t->rsp = rsp == cmd ? t->cmd : map_in(rw, rsp);
  if (t->cmd != 0 && t->rsp != 0)
    r = 0;
out:
  return r;
}

// /dev/tpm0: a command written, its answer read back
static int tpm_open(struct inode *ip __unused, int flags __unused) {
  int r = 0;

  acquiresleep(&tpm->lock);
  if (tpm->devopen)
    r = FNU_ERR_BUSY;
  else {
    tpm->devopen = 1;
    tpm->devlen = 0;
  }
  releasesleep(&tpm->lock);
  return r;
}

static void tpm_close(struct inode *ip __unused) {
  acquiresleep(&tpm->lock);
  tpm->devopen = 0;
  tpm->devlen = 0;
  releasesleep(&tpm->lock);
}

static int tpm_write(struct inode *ip __unused, struct uio *uio) {
  uint8_t *in;
  size_t len = uio->uio_resid;
  int n;

  if (len < HEADER || len > DEVSIZE)
    return -FNU_ERR_INVALID;
  in = malloc(len, M_TEMP, M_WAITOK);
  if (uiomove(in, len, uio) != 0) {
    free(in, M_TEMP);
    return -FNU_ERR_FAULT;
  }
  acquiresleep(&tpm->lock);
  if (tpm->asleep)
    n = -FNU_ERR_BUSY;
  else if ((n = exchange(tpm, in, (int)len, tpm->dev, DEVSIZE)) < 0) {
    why(tpm);
    n = -FNU_ERR_IO;
  } else {
    tpm->devlen = n;
    n = (int)len;
  }
  releasesleep(&tpm->lock);
  free(in, M_TEMP);
  return n;
}

static int tpm_read(struct inode *ip __unused, struct uio *uio) {
  size_t n;
  int r;

  acquiresleep(&tpm->lock);
  n = (size_t)tpm->devlen < uio->uio_resid ? (size_t)tpm->devlen : uio->uio_resid;
  r = n > 0 && uiomove(tpm->dev, n, uio) != 0 ? -FNU_ERR_FAULT : (int)n;
  tpm->devlen = 0;
  releasesleep(&tpm->lock);
  return r;
}

static void attach(struct aml_node *dev, const char *hid __unused) {
  struct tpm2_table *tbl = acpi_find_table("TPM2");
  struct aml_value res;
  struct tpm *t;
  const char *how;
  uint8_t seed[EARLY_SIZE];
  int n;

  if (tpm != 0) {
    printf("acpi: %s, a second TPM, is left alone\n", aml_nodename(dev));
    return;
  }
  t = malloc(sizeof(*t), M_ACPI, M_WAITOK | M_ZERO);
  t->node = dev;
  t->v2 = tbl != 0 && tbl->hdr.length >= sizeof(*tbl);
  t->method = t->v2 ? (int)tbl->start_method : SM_TIS;
  strlcpy(t->name, aml_nodename(dev), sizeof(t->name));
  if (aml_eval_name(dev, "_CRS", 0, 0, &res) == 0) {
    aml_parse_resource(&res, window, t);
    aml_freevalue(&res);
  }
  switch (t->method) {
  case SM_TIS:
    if (t->nwin == 0 || t->win[0].len < TIS_DID_VID + 4 ||
        (t->regs = map_in(&t->win[0], t->win[0].base)) == 0)
      goto unusable;
    wr4(t->regs, TIS_INT_ENABLE, 0);
    how = "TIS";
    break;
  case SM_CRB:
  case SM_CRB_ACPI:
  case SM_ACPI:
    if (crb_setup(t, tbl->control) < 0)
      goto unusable;
    how = t->method == SM_CRB ? "CRB" : t->method == SM_CRB_ACPI ? "CRB, ACPI start"
                                                                 : "ACPI start";
    break;
  default:
    printf("acpi: %s: TPM start method %d is not one known here\n", t->name, t->method);
    free(t, M_ACPI);
    return;
  }
  tpm = t;
  initsleeplock(&t->lock, "tpm");
  t->dev = malloc(DEVSIZE, M_ACPI, M_WAITOK);
  devsw[TPMDEV].open = tpm_open;
  devsw[TPMDEV].close = tpm_close;
  devsw[TPMDEV].read = tpm_read;
  devsw[TPMDEV].write = tpm_write;
  devfs_add("tpm0", FNU_TYPE_DEVICE, TPMDEV, 0, 0600);
  callout_init(&t->co);
  task_set(&t->task, harvest, t);
  printf("acpi: %s, TPM %s, %s at 0x%lx\n", t->name, t->v2 ? "2.0" : "1.2", how,
         (unsigned long)(t->nwin != 0 ? t->win[0].base : tbl->control));
  sysctl_add_int("kern.random.harvest.tpm", &harvest_on, FNU_CTLF_STDIO | FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("kern.random.harvest.tpm_bytes", &harvest_bytes, FNU_CTLF_STDIO, 0, 0);
  sysctl_add_int("kern.random.harvest.tpm_garbage", &t->garbage, FNU_CTLF_STDIO, 0, 0);
  // one refusal at boot is not the end of it
  if ((n = early(seed, sizeof(seed))) > 0) {
    random_add(seed, (uint)n);
    harvest_bytes += n;
  } else
    t->complained = 1;
  callout_reset(&t->co, HARVEST_INTERVAL * HZ, harvest_kick, t);
  memset(seed, 0, sizeof(seed));
  return;
unusable:
  printf("acpi: %s: no usable TPM registers\n", t->name);
  free(t, M_ACPI);
}

static const char *const hids[] = {"MSFT0101", "PNP0C31", 0};

const struct acpi_driver acpi_tpm_driver = {"tpm", hids, 0, attach};
