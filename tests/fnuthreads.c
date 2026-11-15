// THREADS
#include "inc/fnu.h"

#define NTHREAD 8
#define ROUNDS 20000
#define STACK (64 * 1024)

static int bad;

static uint64 len(const char *s) {
  uint64 n = 0;

  while (s[n] != 0)
    n++;
  return n;
}

static long get(int fd, void *buf, uint64 n) {
  struct fnu_segment seg;

  seg.base = buf;
  seg.len = n;
  return fnu_read(fd, &seg, 1, FNU_IO_HERE);
}

static long put(int fd, const void *buf, uint64 n) {
  struct fnu_segment seg;

  seg.base = (void *)buf;
  seg.len = n;
  return fnu_write(fd, &seg, 1, FNU_IO_HERE);
}

static void say(const char *s) { put(1, s, len(s)); }

static void sayn(long v) {
  char buf[24];
  int i = (int)sizeof(buf);
  int neg = v < 0;

  if (neg)
    v = -v;
  buf[--i] = 0;
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0 && i > 1);
  if (neg)
    buf[--i] = '-';
  say(buf + i);
}

static void check(int ok, const char *what) {
  say("fnuthreads: ");
  say(what);
  say(ok ? " ok\n" : " FAILED\n");
  if (!ok)
    bad++;
}

// THE LOCK
static uint32_t lock;

static void lock_take(uint32_t *l) {
  uint32_t c = 0;

  if (__atomic_compare_exchange_n(l, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return;
  if (c != 2)
    c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    fnu_futex(l, FNU_FUTEX_WAIT | FNU_FUTEX_PRIVATE, 2, FNU_FUTEX_FOREVER, 0);
    c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
  }
}

static void lock_give(uint32_t *l) {
  if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2)
    fnu_futex(l, FNU_FUTEX_WAKE | FNU_FUTEX_PRIVATE, 1, 0, 0);
}

// A THREAD
struct thread {
  struct thread *self;
  int index;
  uint32_t alive;
  int tls_ok;
  long counted;
  char *stack;
};

static struct thread threads[NTHREAD];
static long counter;

static struct thread *current(void) {
  struct thread *t;

  __asm __volatile("movq %%fs:0, %0" : "=r"(t));
  return t;
}

static void worker(void *arg) {
  struct thread *t = arg;
  int i;

  t->tls_ok = current() == t && t->self == t;
  for (i = 0; i < ROUNDS; i++) {
    lock_take(&lock);
    counter++;
    t->counted++;
    lock_give(&lock);
  }
  fnu_thread_exit(&t->alive);
}

static long start(struct thread *t, void (*fn)(void *)) {
  struct fnu_thread_start st;
  long r;

  t->self = t;
  t->stack = (char *)fnu_map(0, STACK, FNU_MEM_READ | FNU_MEM_WRITE,
                             FNU_MEM_PRIVATE | FNU_MEM_ANON, -1, 0);
  if ((long)t->stack < 0)
    return (long)t->stack;
  st.entry = (uint64_t)(uintptr_t)fn;
  st.arg = (uint64_t)(uintptr_t)t;
  st.stack = (uint64_t)(uintptr_t)(t->stack + STACK);
  st.tcb = (uint64_t)(uintptr_t)t;
  st.tid = (uint64_t)(uintptr_t)&t->alive;
  r = fnu_thread_start(&st);
  return r;
}

static void join(struct thread *t) {
  uint32_t v;

  while ((v = __atomic_load_n(&t->alive, __ATOMIC_ACQUIRE)) != 0)
    fnu_futex(&t->alive, FNU_FUTEX_WAIT, v, FNU_FUTEX_FOREVER, 0);
}

static uint32_t never;

static void sleeper(void *arg) {
  (void)arg;
  for (;;)
    fnu_futex(&never, FNU_FUTEX_WAIT | FNU_FUTEX_PRIVATE, 0, FNU_FUTEX_FOREVER, 0);
}

static void spinner(void *arg) {
  volatile long n = 0;

  (void)arg;
  for (;;)
    n++;
}

static void leaver(void *arg) {
  (void)arg;
  fnu_exit(42);
}

static void execer(void *arg) {
  static char *argv[] = {"fnuonly", 0};
  static char *envp[] = {0};

  (void)arg;
  fnu_exec("/usr/tests/fnuonly", argv, envp);
  fnu_exit(99);
}

static struct thread extra[4];
static int forked_ok;

static void forker(void *arg) {
  struct thread *t = arg;
  struct fnu_exitinfo ei;
  pid_t pid;

  if ((pid = fnu_fork()) == 0)
    fnu_exit(fnu_thread_id() > 0 ? 7 : 8);
  forked_ok = fnu_wait(pid, &ei, 0) == pid && ei.reason == FNU_EXIT_NORMAL &&
              ei.status == 7;
  fnu_thread_exit(&t->alive);
}

// THE DESCRIPTOR TABLE
static int pipefd[2];
static volatile int reader_got;

static void reader(void *arg) {
  struct thread *t = arg;
  char c;

  reader_got = (int)get(pipefd[0], &c, 1);
  fnu_thread_exit(&t->alive);
}

#define CHURN 2000

static void churner(void *arg) {
  struct thread *t = arg;
  struct fnu_stat st;
  int i, fd;

  for (i = 0; i < CHURN; i++) {
    if ((fd = fnu_open(FNU_FD_CWD, t->index & 1 ? "/dev/null" : ".", FNU_OPEN_READ,
                       0)) < 0) {
      t->tls_ok = 0;
      break;
    }
    if (fnu_getattr(fd, 0, 0, &st) < 0 ||
        (t->index & 1 ? st.type != FNU_TYPE_DEVICE : st.type != FNU_TYPE_DIR))
      t->tls_ok = 0;
    fnu_close(fd);
  }
  fnu_thread_exit(&t->alive);
}

static void chdirer(void *arg) {
  struct thread *t = arg;
  int i;

  for (i = 0; i < CHURN; i++)
    fnu_chdir(FNU_FD_CWD, i & 1 ? "/" : "/dev");
  fnu_thread_exit(&t->alive);
}

static void descriptors(void) {
  struct fnu_timespec ts = {0, 50 * 1000 * 1000, 0};
  int i, ok = 1, many[300], n;

  check(fnu_pipe(pipefd, 0) == 0, "a pipe");
  reader_got = -1000;
  start(&extra[0], reader);
  fnu_sleep(&ts, 0);
  check(fnu_close(pipefd[0]) == 0, "a descriptor closed under a read");
  put(pipefd[1], "x", 1);
  join(&extra[0]);
  check(reader_got == 1, "the read it was in still ends with its data");
  check(fnu_close(pipefd[0]) < 0, "and the descriptor is gone");
  fnu_close(pipefd[1]);

  for (i = 0; i < NTHREAD; i++) {
    threads[i].alive = 0;
    threads[i].tls_ok = 1;
    start(&threads[i], i == NTHREAD - 1 ? chdirer : churner);
  }
  for (n = 0; n < (int)(sizeof(many) / sizeof(many[0])); n++)
    if ((many[n] = fnu_open(FNU_FD_CWD, "/dev/null", FNU_OPEN_READ, 0)) < 0)
      break;
  for (i = 0; i < NTHREAD; i++)
    join(&threads[i]);
  for (i = 0; i < NTHREAD - 1; i++)
    ok &= threads[i].tls_ok;
  for (i = 0; i < n; i++)
    fnu_close(many[i]);
  fnu_chdir(FNU_FD_CWD, "/");
  check(ok && n == (int)(sizeof(many) / sizeof(many[0])),
        "every descriptor taken is the file it was opened as");
}

static int child_status(pid_t pid, int flags, struct fnu_exitinfo *ei) {
  return fnu_wait(pid, ei, flags) == pid;
}

void fnu_main(void) {
  struct fnu_exitinfo ei;
  long tids[NTHREAD];
  int i, distinct = 1, tls = 1;
  long total = 0;
  pid_t pid;

  for (i = 0; i < NTHREAD; i++) {
    threads[i].index = i;
    tids[i] = start(&threads[i], worker);
    if (tids[i] < 0) {
      check(0, "a thread starts");
      fnu_exit(1);
    }
  }
  for (i = 0; i < NTHREAD; i++)
    join(&threads[i]);
  for (i = 0; i < NTHREAD; i++) {
    total += threads[i].counted;
    tls &= threads[i].tls_ok;
    if (i > 0 && tids[i] == tids[i - 1])
      distinct = 0;
  }
  say("fnuthreads: counted ");
  sayn(counter);
  say("\n");
  check(counter == (long)NTHREAD * ROUNDS && total == counter,
        "eight threads count under one futex lock");
  check(tls, "each finds its own block at %fs");
  check(distinct && tids[0] > 0 && fnu_thread_id() > 0 &&
            fnu_thread_id() != tids[0],
        "thread ids are their own");

  if ((pid = fnu_fork()) == 0) {
    start(&extra[0], sleeper);
    start(&extra[1], spinner);
    start(&extra[2], leaver);
    sleeper(0);
  }
  check(child_status(pid, 0, &ei) && ei.reason == FNU_EXIT_NORMAL &&
            ei.status == 42,
        "one thread's exit is the process's");

  // a kill takes every thread
  if ((pid = fnu_fork()) == 0) {
    start(&extra[0], spinner);
    start(&extra[1], spinner);
    start(&extra[2], sleeper);
    sleeper(0);
  }
  {
    struct fnu_timespec ts = {0, 100 * 1000 * 1000, 0};

    fnu_sleep(&ts, 0);
  }
  fnu_signal(pid, FNU_SIG_STOP);
  check(child_status(pid, FNU_WAIT_STOPPED, &ei) && ei.reason == FNU_EXIT_STOPPED,
        "a stop stops the process");
  fnu_signal(pid, FNU_SIG_CONTINUE);
  fnu_signal(pid, FNU_SIG_KILL);
  check(child_status(pid, 0, &ei) && ei.reason == FNU_EXIT_KILLED &&
            ei.status == FNU_SIG_KILL,
        "a kill ends every thread");

  if ((pid = fnu_fork()) == 0) {
    start(&extra[0], spinner);
    start(&extra[1], sleeper);
    start(&extra[2], execer);
    sleeper(0);
  }
  check(child_status(pid, 0, &ei) && ei.reason == FNU_EXIT_NORMAL &&
            ei.status == 0,
        "exec from a thread runs the program alone");

  start(&extra[3], forker);
  join(&extra[3]);
  check(forked_ok, "fork from a thread");

  descriptors();

  say(bad == 0 ? "fnuthreads: ok\n" : "fnuthreads: FAILED\n");
  fnu_exit(bad != 0);
}
