#ifndef _THREADS_H_
#define _THREADS_H_

// C11 threads, over the system's own threads and futex
#include <time.h>

#define thread_local _Thread_local
#define ONCE_FLAG_INIT { 0 }
#define TSS_DTOR_ITERATIONS 4

typedef struct __thrd *thrd_t;
typedef int (*thrd_start_t)(void *);
typedef unsigned int tss_t;
typedef void (*tss_dtor_t)(void *);
typedef struct {
  unsigned int state;
} once_flag;
typedef struct {
  unsigned int lock;
  int type;
  thrd_t owner;
  unsigned int depth;
} mtx_t;
typedef struct {
  unsigned int seq;
} cnd_t;

enum { thrd_success, thrd_busy, thrd_error, thrd_nomem, thrd_timedout };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

int thrd_create(thrd_t *, thrd_start_t, void *);
thrd_t thrd_current(void);
int thrd_detach(thrd_t);
int thrd_equal(thrd_t, thrd_t);
_Noreturn void thrd_exit(int);
int thrd_join(thrd_t, int *);
int thrd_sleep(const struct timespec *, struct timespec *);
void thrd_yield(void);

int mtx_init(mtx_t *, int);
void mtx_destroy(mtx_t *);
int mtx_lock(mtx_t *);
int mtx_timedlock(mtx_t *, const struct timespec *);
int mtx_trylock(mtx_t *);
int mtx_unlock(mtx_t *);

int cnd_init(cnd_t *);
void cnd_destroy(cnd_t *);
int cnd_signal(cnd_t *);
int cnd_broadcast(cnd_t *);
int cnd_wait(cnd_t *, mtx_t *);
int cnd_timedwait(cnd_t *, mtx_t *, const struct timespec *);

int tss_create(tss_t *, tss_dtor_t);
void tss_delete(tss_t);
void *tss_get(tss_t);
int tss_set(tss_t, void *);

void call_once(once_flag *, void (*)(void));

#endif
