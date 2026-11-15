#include "defs.h"
#include "devfs.h"
#include "malloc.h"
#include "inc/stat.h"
#include "file.h"
#include "buf.h"
#include "blockdev.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "sleeplock.h"
#include "inc/abi.h"
#include "mmu.h"
#include "uio.h"

#define MSGBUF_SIZE (64 * 1024)
#define MSGBUF_MAGIC 0x464e554d
#define CONSBUF_SIZE (16 * 1024)
#define DUMP_SECTORS (MSGBUF_DUMP_SECTORS)

struct msgbuf {
  uint32_t magic;
  uint32_t size;
  uint64_t written;
  char buf[MSGBUF_SIZE];
};

struct dumphdr {
  uint32_t magic;
  uint32_t len;
  uint32_t panicked;
  char pad[500];
};

static struct msgbuf early;
static struct msgbuf *mb = &early;
static char saved[MSGBUF_SIZE];
static uint saved_len;
static int kept;
const char *panicstr;

static struct {
  char *buf;
  uint64_t written;
} cons_rec;

// /dev/klog
static struct {
  int open;
  uint64_t rptr;
  int started;
  volatile int pending;
  struct callout timer;
  struct selinfo sel;
} klog;

void msgbuf_putc(int c) {
  mb->buf[mb->written % MSGBUF_SIZE] = (char)c;
  mb->written++;
  klog.pending = 1;
}

int sys_sendsyslog(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uint64_t addr;
  uint64_t len;
  int flags, i;
  char *line;

  addr = uap[0];
  len = uap[1];
  flags = (int)uap[2];
  if (len == 0)
    return FNU_ERR_INVALID;
  if (len > FNU_LOG_MAXLINE)
    len = FNU_LOG_MAXLINE;
  line = malloc(len + 1, M_TEMP, M_WAITOK);
  if (copyin(p->p_p->pgdir, line, (uintptr_t)addr, (size_t)len) < 0) {
    free(line, M_TEMP);
    return FNU_ERR_FAULT;
  }
  line[len] = 0;
  if (flags & FNU_LOG_CONSOLE)
    printf("%s\n", line);
  else {
    int locking = log_enter();

    for (i = 0; i < (int)len; i++)
      msgbuf_putc(line[i]);
    msgbuf_putc('\n');
    log_leave(locking);
  }
  free(line, M_TEMP);
  return 0;
}

int msgbuf_read(char *dst, uint64_t off, int n) {
  int locking = log_enter();
  uint64_t start = mb->written > MSGBUF_SIZE ? mb->written - MSGBUF_SIZE : 0;
  uint64_t len = saved_len + (mb->written - start);
  int i;

  if (n < 0 || off >= len)
    n = 0;
  else if ((uint64_t)n > len - off)
    n = (int)(len - off);
  for (i = 0; i < n; i++) {
    uint64_t at = off + (uint64_t)i;
    dst[i] = at < saved_len ? saved[at] : mb->buf[(start + at - saved_len) % MSGBUF_SIZE];
  }
  log_leave(locking);
  return n;
}

// tty_lock held
void consbuf_put(const char *s, int n) {
  int i;

  if (cons_rec.buf == 0)
    return;
  for (i = 0; i < n; i++)
    if (s[i] != 0 && s[i] != '\r' && s[i] != 0177)
      cons_rec.buf[cons_rec.written++ % CONSBUF_SIZE] = s[i];
}

// THE LOG AS SYSCTL NODES

static uint64_t msgbuf_length(void *arg __unused) {
  int locking = log_enter();
  uint64_t len = saved_len + (mb->written > MSGBUF_SIZE ? MSGBUF_SIZE : mb->written);

  log_leave(locking);
  return len;
}

static int msgbuf_readat(void *arg __unused, char *dst, uint64_t off, int n) {
  return msgbuf_read(dst, off, n);
}

static int64_t msgbuf_room(void *arg __unused) { return (int64_t)saved_len + MSGBUF_SIZE; }

static uint64_t consbuf_length(void *arg __unused) {
  int locking = log_enter();
  uint64_t len = cons_rec.written > CONSBUF_SIZE ? CONSBUF_SIZE : cons_rec.written;

  log_leave(locking);
  return len;
}

static int consbuf_readat(void *arg __unused, char *dst, uint64_t off, int n) {
  int locking = log_enter();
  uint64_t w = cons_rec.written;
  uint64_t start = w > CONSBUF_SIZE ? w - CONSBUF_SIZE : 0;
  int i;

  if (n < 0 || off >= w - start)
    n = 0;
  else if ((uint64_t)n > w - start - off)
    n = (int)(w - start - off);
  for (i = 0; i < n; i++)
    dst[i] = cons_rec.buf[(start + off + (uint64_t)i) % CONSBUF_SIZE];
  log_leave(locking);
  return n;
}

static int64_t consbuf_room(void *arg __unused) { return CONSBUF_SIZE; }


#define KLOG_WAKEUPS 5

static void klog_timeout(void *arg __unused) {
  if (klog.pending) {
    klog.pending = 0;
    wakeup(&klog);
    selwakeup(&klog.sel);
  }
  if (klog.open)
    callout_reset(&klog.timer, HZ / KLOG_WAKEUPS, klog_timeout, 0);
}

static int klog_open(struct inode *ip __unused, int omode __unused) {
  int locking = log_enter();

  if (klog.open) {
    log_leave(locking);
    return FNU_ERR_BUSY;
  }
  klog.open = 1;
  if (!klog.started) {
    klog.started = 1;
    klog.rptr = mb->written > MSGBUF_SIZE ? mb->written - MSGBUF_SIZE : 0;
  }
  log_leave(locking);
  callout_reset(&klog.timer, HZ / KLOG_WAKEUPS, klog_timeout, 0);
  return 0;
}

static void klog_close(struct inode *ip __unused) {
  klog.open = 0;
  callout_stop(&klog.timer);
}

static int klog_poll(struct inode *ip __unused, int want) {
  if (!(want & FNU_SEL_READ))
    return 0;
  if (klog.rptr != mb->written)
    return FNU_SEL_READ;
  selrecord(&klog.sel);
  return klog.rptr != mb->written ? FNU_SEL_READ : 0;
}

static int klog_read(struct inode *ip __unused, struct uio *uio) {
  char *buf;
  int locking, n, error;
  uint64_t oldest;

  while (klog.rptr == mb->written)
    if ((error = tsleep(&klog, PSOCK | PCATCH, "klog", 0)) != 0)
      return -error;
  if ((buf = page_alloc()) == 0)
    return -FNU_ERR_NOMEMORY;
  locking = log_enter();
  oldest = mb->written > MSGBUF_SIZE ? mb->written - MSGBUF_SIZE : 0;
  if (klog.rptr < oldest)
    klog.rptr = oldest;
  n = (int)(mb->written - klog.rptr);
  if (n > PGSIZE)
    n = PGSIZE;
  if ((size_t)n > uio->uio_resid)
    n = (int)uio->uio_resid;
  for (error = 0; error < n; error++)
    buf[error] = mb->buf[(klog.rptr + (uint64_t)error) % MSGBUF_SIZE];
  klog.rptr += (uint64_t)n;
  log_leave(locking);
  error = uiomove(buf, (size_t)n, uio);
  page_free(buf);
  return error != 0 ? -FNU_ERR_FAULT : n;
}

uint64_t msgbuf_bytes(void) { return sizeof(struct msgbuf); }

void msgbuf_attach(uint64_t phys) {
  struct msgbuf *m = (struct msgbuf *)P2V((uintptr_t)phys);
  uint64_t i, start;

  if (phys == 0)
    return;
  if (m->magic != MSGBUF_MAGIC || m->size != MSGBUF_SIZE ||
      m->written > (1ULL << 40)) {
    m->magic = MSGBUF_MAGIC;
    m->size = MSGBUF_SIZE;
    m->written = 0;
  } else {
    kept = m->written != 0;
    printf("MSGBUF: %u bytes kept from the boot before\n",
            (uint)(m->written > MSGBUF_SIZE ? MSGBUF_SIZE : m->written));
  }
  start = early.written > MSGBUF_SIZE ? early.written - MSGBUF_SIZE : 0;
  for (i = start; i < early.written; i++)
    m->buf[m->written++ % MSGBUF_SIZE] = early.buf[i % MSGBUF_SIZE];
  mb = m;
}

// THE DUMP

static uint dump_dev(uint64_t *first) {
  uint dev;
  uint64_t size;

  if ((dev = storage_swap_device()) == 0)
    return 0;
  size = blockdev_device_size(dev);
  if (size < DUMP_SECTORS * 4)
    return 0;
  *first = size - DUMP_SECTORS;
  return dev;
}

static int raw_io(uint dev, uint64_t sector, void *data, uint bytes, int write) {
  static struct buf b;

  memset(&b, 0, sizeof(b));
  initsleeplock(&b.lock, "msgbuf");
  b.lock.locked = 1;
  b.lock.pid = curproc->p_p->pid;
  b.dev = dev;
  b.blockno = sector;
  b.addr = data;
  b.bcount = bytes;
  b.flags = write ? B_DIRTY : 0;
  blockdev_rw(&b);
  return (b.flags & B_ERROR) ? -1 : 0;
}

void msgbuf_dump(int panicked) {
  static struct dumphdr hdr __aligned(512);
  static char text[MSGBUF_SIZE] __aligned(512);
  uint64_t first;
  uint dev, off;
  int n;

  if (curproc == 0 || (dev = dump_dev(&first)) == 0)
    return;
  n = msgbuf_read(text, saved_len, MSGBUF_SIZE);
  if (n <= 0)
    return;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = MSGBUF_MAGIC;
  hdr.len = (uint32_t)n;
  hdr.panicked = (uint32_t)panicked;
  for (off = 0; off < (uint)n; off += MAXPHYS) {
    uint chunk = (uint)n - off > MAXPHYS ? MAXPHYS : (uint)n - off;
    chunk = (chunk + BSIZE - 1) / BSIZE * BSIZE;
    if (raw_io(dev, first + 1 + off / BSIZE, text + off, chunk, 1) < 0)
      return;
  }
  raw_io(dev, first, &hdr, sizeof(hdr), 1);
}

void msgbuf_restore(void) {
  static struct dumphdr hdr __aligned(512);
  uint64_t first;
  uint dev, off;
  int locking;

  if (kept || saved_len != 0 || curproc == 0 || (dev = dump_dev(&first)) == 0)
    return;
  if (raw_io(dev, first, &hdr, sizeof(hdr), 0) < 0 || hdr.magic != MSGBUF_MAGIC ||
      hdr.len == 0 || hdr.len > MSGBUF_SIZE)
    return;
  for (off = 0; off < hdr.len; off += MAXPHYS) {
    uint chunk = hdr.len - off > MAXPHYS ? MAXPHYS : hdr.len - off;
    chunk = (chunk + BSIZE - 1) / BSIZE * BSIZE;
    if (raw_io(dev, first + 1 + off / BSIZE, saved + off, chunk, 0) < 0)
      return;
  }
  locking = log_enter();
  saved_len = hdr.len;
  log_leave(locking);
  hdr.magic = 0;
  raw_io(dev, first, &hdr, sizeof(hdr), 1);
  printf("MSGBUF: %u bytes of the boot before read from fnu-swap%s\n", saved_len,
          hdr.panicked ? ", which ended in a panic" : "");
}

void msgbuf_init(void) {
  callout_init(&klog.timer);
  devsw[KLOG].open = klog_open;
  devsw[KLOG].close = klog_close;
  devsw[KLOG].read = klog_read;
  devsw[KLOG].poll = klog_poll;
  devfs_add("klog", FNU_TYPE_DEVICE, KLOG, 0, 0600);
  cons_rec.buf = malloc(CONSBUF_SIZE, M_DEVBUF, M_WAITOK | M_ZERO);
  sysctl_add_opaque("kern.msgbuf", msgbuf_length, msgbuf_readat, 0, 0, FNU_PLEDGE_VMINFO);
  sysctl_add_quad_fn("kern.msgbufsize", msgbuf_room, 0);
  sysctl_add_opaque("kern.consbuf", consbuf_length, consbuf_readat, 0, FNU_CTLF_PRIVATE,
                    FNU_PLEDGE_VMINFO);
  sysctl_add_quad_fn("kern.consbufsize", consbuf_room, 0);
}
