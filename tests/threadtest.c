// C11 THREADS
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define NWORK 8
#define ROUNDS 20000

static int bad;

static void check(int ok, const char *what) {
  printf("threadtest: %-46s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    bad++;
}

static mtx_t lock;
static long counter;
static _Thread_local int mine = 5;
static _Thread_local char scratch[64];
static tss_t key;
static int destroyed;
static mtx_t dlock;

static void gone(void *v) {
  mtx_lock(&dlock);
  destroyed++;
  mtx_unlock(&dlock);
  free(v);
}

static int worker(void *arg) {
  int i = (int)(long)arg, k, ok = 1;
  void *blocks[16];

  if (mine != 5)
    ok = 0;
  mine = 100 + i;
  snprintf(scratch, sizeof(scratch), "thread %d", i);
  tss_set(key, malloc(32));
  errno = 1000 + i;
  for (k = 0; k < ROUNDS; k++) {
    mtx_lock(&lock);
    counter++;
    mtx_unlock(&lock);
    if (k % 1000 == 0) {
      int j;

      for (j = 0; j < 16; j++)
        if ((blocks[j] = malloc((size_t)(16 + (k + j * 37) % 3000))) != NULL)
          memset(blocks[j], i, 16);
      for (j = 0; j < 16; j++)
        free(blocks[j]);
    }
  }
  if (mine != 100 + i || errno != 1000 + i)
    ok = 0;
  {
    char want[64];

    snprintf(want, sizeof(want), "thread %d", i);
    if (strcmp(scratch, want) != 0)
      ok = 0;
  }
  return ok ? i : -1;
}

static mtx_t qlock;
static cnd_t qcond;
static int slot, full, done;

static int producer(void *arg) {
  int n;

  (void)arg;
  for (n = 1; n <= 1000; n++) {
    mtx_lock(&qlock);
    while (full)
      cnd_wait(&qcond, &qlock);
    slot = n;
    full = 1;
    cnd_broadcast(&qcond);
    mtx_unlock(&qlock);
  }
  mtx_lock(&qlock);
  while (full)
    cnd_wait(&qcond, &qlock);
  done = 1;
  cnd_broadcast(&qcond);
  mtx_unlock(&qlock);
  return 0;
}

static int consumer(void *arg) {
  long sum = 0;

  (void)arg;
  mtx_lock(&qlock);
  for (;;) {
    while (!full && !done)
      cnd_wait(&qcond, &qlock);
    if (full) {
      sum += slot;
      full = 0;
      cnd_broadcast(&qcond);
    } else if (done) {
      break;
    }
  }
  mtx_unlock(&qlock);
  return sum == 500500 ? 1 : 0;
}

static int mapper(void *arg) {
  int i, k, ok = 1;

  (void)arg;
  for (i = 0; i < 500; i++) {
    char *m = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);

    if (m == MAP_FAILED)
      return 0;
    for (k = 0; k < 8; k++)
      m[k * 4096] = (char)(i + k);
    for (k = 0; k < 8; k++)
      if (m[k * 4096] != (char)(i + k))
        ok = 0;
    munmap(m, 8 * 4096);
  }
  return ok;
}

static once_flag once = ONCE_FLAG_INIT;
static int once_runs;

static void run_once(void) { once_runs++; }

static int oncer(void *arg) {
  (void)arg;
  call_once(&once, run_once);
  return 0;
}

static int holder(void *arg) {
  mtx_t *m = arg;
  struct timespec t = {0, 300 * 1000 * 1000};

  mtx_lock(m);
  thrd_sleep(&t, NULL);
  mtx_unlock(m);
  return 0;
}

int main(void) {
  thrd_t t[NWORK], p, c;
  int i, r, all = 1;

  mtx_init(&lock, mtx_plain);
  mtx_init(&dlock, mtx_plain);
  tss_create(&key, gone);
  for (i = 0; i < NWORK; i++)
    if (thrd_create(&t[i], worker, (void *)(long)i) != thrd_success)
      all = 0;
  for (i = 0; i < NWORK; i++) {
    if (thrd_join(t[i], &r) != thrd_success || r != i)
      all = 0;
  }
  check(all && counter == (long)NWORK * ROUNDS,
        "eight threads count under one mutex");
  check(all, "_Thread_local, errno and malloc are each one's");
  check(destroyed == NWORK, "each key's value went to its destructor");
  check(mine == 5, "the first thread kept its own copy");

  mtx_init(&qlock, mtx_plain);
  cnd_init(&qcond);
  thrd_create(&p, producer, NULL);
  thrd_create(&c, consumer, NULL);
  thrd_join(p, NULL);
  thrd_join(c, &r);
  check(r == 1, "a condition carries a thousand items across");

  all = 1;
  for (i = 0; i < NWORK; i++)
    thrd_create(&t[i], mapper, NULL);
  for (i = 0; i < NWORK; i++)
    if (thrd_join(t[i], &r) != thrd_success || r != 1)
      all = 0;
  check(all, "eight threads map and unmap at once");

  for (i = 0; i < NWORK; i++)
    thrd_create(&t[i], oncer, NULL);
  for (i = 0; i < NWORK; i++)
    thrd_join(t[i], NULL);
  check(once_runs == 1, "call_once runs once among eight");

  {
    mtx_t m;
    struct timespec at;

    mtx_init(&m, mtx_timed);
    thrd_create(&t[0], holder, &m);
    thrd_sleep(&(struct timespec){0, 50 * 1000 * 1000}, NULL);
    timespec_get(&at, TIME_UTC);
    at.tv_nsec += 50 * 1000 * 1000;
    if (at.tv_nsec >= 1000000000) {
      at.tv_sec++;
      at.tv_nsec -= 1000000000;
    }
    check(mtx_timedlock(&m, &at) == thrd_timedout, "a timed lock gives up");
    thrd_join(t[0], NULL);
    check(mtx_trylock(&m) == thrd_success, "and the lock is free after");
    mtx_unlock(&m);
  }
  {
    mtx_t m;

    mtx_init(&m, mtx_plain | mtx_recursive);
    mtx_lock(&m);
    check(mtx_trylock(&m) == thrd_success && mtx_unlock(&m) == thrd_success &&
              mtx_unlock(&m) == thrd_success,
          "a recursive mutex is taken twice");
  }

  thrd_create(&t[0], worker, (void *)0L);
  {
    pid_t pid = fork();
    int status;

    if (pid == 0) {
      char *q = malloc(100000);

      _exit(q != NULL ? 0 : 1);
    }
    thrd_join(t[0], NULL);
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "fork from a process with threads");
  }

  printf(bad == 0 ? "threadtest: ok\n" : "threadtest: %d failed\n", bad);
  return bad != 0;
}
