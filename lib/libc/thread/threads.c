// C11 THREADS
// own stack, block at %fs; join waits on a word the system clears
#include <sys/param.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <fnu/sys.h>
#include <tib.h>
#include "thread_private.h"

// stack, guard page, key count
#define STACK_SIZE (512 * 1024)
#define GUARD PAGE_SIZE
#define TSS_MAX 512

void _malloc_init(int);
void _thread_finalize(void);

struct __thrd {
	struct tib *tib;
	thrd_start_t fn;
	void *arg;
	int result;
	unsigned int alive; // its tid, cleared by the system when it is gone
	unsigned int state;
	char *stack;
	void *tss[TSS_MAX];
	struct __thrd *next; // on the list of the detached and dead
};

#define JOINABLE 0
#define DETACHED 1

static struct __thrd main_thread;
static volatile unsigned int list_lock;
static struct __thrd *dead;
static unsigned int nthreads = 1;

static struct {
	unsigned int used;
	tss_dtor_t dtor;
} keys[TSS_MAX];
static volatile unsigned int keys_lock;

static void thread_free(struct __thrd *t) {
	fnu_unmap(t->stack, STACK_SIZE);
	_libc_tib_free(t->tib);
	free(t);
}

// the detached ones that are gone give their stacks back
static void reap(void) {
	struct __thrd **pp, *t;

	_lock_take(&list_lock);
	for (pp = &dead; (t = *pp) != NULL;) {
		if (__atomic_load_n(&t->alive, __ATOMIC_ACQUIRE) == 0) {
			*pp = t->next;
			thread_free(t);
		} else
			pp = &t->next;
	}
	_lock_give(&list_lock);
}

thrd_t
thrd_current(void)
{
	struct __thrd *t = TIB_GET()->tib_thread;

	return t != NULL ? t : &main_thread;
}

static void
thread_main(void *arg)
{
	struct __thrd *t = arg;

	t->tib->tib_tid = (pid_t)t->alive;
	thrd_exit(t->fn(t->arg));
}

int
thrd_create(thrd_t *out, thrd_start_t fn, void *arg)
{
	struct fnu_thread_start st;
	struct __thrd *t;
	long r;

	if (!__isthreaded) {
		main_thread.tib = TIB_GET();
		main_thread.tib->tib_thread = &main_thread;
		_malloc_init(1);
		__isthreaded = 1;
	}
	reap();
	if ((t = calloc(1, sizeof(*t))) == NULL)
		return thrd_nomem;
	if ((t->tib = _libc_tib_new()) == NULL) {
		free(t);
		return thrd_nomem;
	}
	t->tib->tib_thread = t;
	t->fn = fn;
	t->arg = arg;
	r = fnu_map(0, STACK_SIZE, FNU_MEM_READ | FNU_MEM_WRITE,
	    FNU_MEM_PRIVATE | FNU_MEM_ANON, -1, 0);
	if (r < 0) {
		_libc_tib_free(t->tib);
		free(t);
		return thrd_nomem;
	}
	t->stack = (char *)r;
	fnu_protect(t->stack, GUARD, 0);
	st.entry = (uint64_t)(uintptr_t)thread_main;
	st.arg = (uint64_t)(uintptr_t)t;
	st.stack = (uint64_t)(uintptr_t)(t->stack + STACK_SIZE);
	st.tcb = (uint64_t)(uintptr_t)t->tib;
	st.tid = (uint64_t)(uintptr_t)&t->alive;
	__atomic_add_fetch(&nthreads, 1, __ATOMIC_RELAXED);
	if ((r = fnu_thread_start(&st)) < 0) {
		__atomic_sub_fetch(&nthreads, 1, __ATOMIC_RELAXED);
		thread_free(t);
		return r == -FNU_ERR_NOMEMORY || r == -FNU_ERR_WOULDBLOCK ? thrd_nomem : thrd_error;
	}
	*out = t;
	return thrd_success;
}

// destructors, a few rounds; they may set more keys
static void
tss_run(struct __thrd *t)
{
	int round, i, again;

	for (round = 0; round < TSS_DTOR_ITERATIONS; round++) {
		again = 0;
		for (i = 0; i < TSS_MAX; i++) {
			void *v = t->tss[i];

			if (v == NULL || !keys[i].used || keys[i].dtor == NULL)
				continue;
			t->tss[i] = NULL;
			keys[i].dtor(v);
			again = 1;
		}
		if (!again)
			break;
	}
}

// the last thread out ends the program like exit(0)
_Noreturn void
thrd_exit(int res)
{
	struct __thrd *t = thrd_current();

	_thread_finalize();
	tss_run(t);
	if (__atomic_sub_fetch(&nthreads, 1, __ATOMIC_ACQ_REL) == 0)
		exit(0);
	t->result = res;
	if (t == &main_thread)
		fnu_thread_exit(NULL);
	if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == DETACHED) {
		_lock_take(&list_lock);
		t->next = dead;
		dead = t;
		_lock_give(&list_lock);
	}
	fnu_thread_exit(&t->alive);
}

int
thrd_join(thrd_t t, int *res)
{
	unsigned int v;

	if (t == thrd_current() || t == &main_thread || t->state == DETACHED)
		return thrd_error;
	while ((v = __atomic_load_n(&t->alive, __ATOMIC_ACQUIRE)) != 0)
		fnu_futex(&t->alive, FNU_FUTEX_WAIT, v, FNU_FUTEX_FOREVER, 0);
	if (res != NULL)
		*res = t->result;
	thread_free(t);
	return thrd_success;
}

// a dead thread goes on the list now; the next create frees it
int
thrd_detach(thrd_t t)
{
	if (t == &main_thread)
		return thrd_error;
	if (__atomic_exchange_n(&t->state, DETACHED, __ATOMIC_ACQ_REL) == DETACHED)
		return thrd_error;
	if (__atomic_load_n(&t->alive, __ATOMIC_ACQUIRE) == 0) {
		_lock_take(&list_lock);
		t->next = dead;
		dead = t;
		_lock_give(&list_lock);
	}
	return thrd_success;
}

int
thrd_equal(thrd_t a, thrd_t b)
{
	return a == b;
}

int
thrd_sleep(const struct timespec *want, struct timespec *left)
{
	struct fnu_timespec w, l;
	int r;

	w.sec = want->tv_sec;
	w.nsec = (int32_t)want->tv_nsec;
	w.pad = 0;
	r = fnu_sleep(&w, &l);
	if (left != NULL) {
		left->tv_sec = l.sec;
		left->tv_nsec = l.nsec;
	}
	return r == 0 ? 0 : r == -FNU_ERR_INTERRUPTED ? -1 : -2;
}

void
thrd_yield(void)
{
	fnu_thread_yield();
}

// MUTEXES
int
mtx_init(mtx_t *m, int type)
{
	if ((type & ~(mtx_recursive | mtx_timed)) != 0)
		return thrd_error;
	m->lock = 0;
	m->type = type;
	m->owner = NULL;
	m->depth = 0;
	return thrd_success;
}

void
mtx_destroy(mtx_t *m)
{
	m->owner = NULL;
}

// ???
static uint64_t
until(const struct timespec *at)
{
	struct timespec now;
	int64_t ns;

	timespec_get(&now, TIME_UTC);
	ns = (at->tv_sec - now.tv_sec) * 1000000000LL + (at->tv_nsec - now.tv_nsec);
	return ns > 0 ? (uint64_t)ns : 0;
}

static int
mtx_take(mtx_t *m, const struct timespec *at)
{
	thrd_t self = thrd_current();
	unsigned int c = 0;

	if ((m->type & mtx_recursive) && m->owner == self) {
		m->depth++;
		return thrd_success;
	}
	if (!__atomic_compare_exchange_n(&m->lock, &c, 1, 0, __ATOMIC_ACQUIRE,
	    __ATOMIC_RELAXED)) {
		if (c != 2)
			c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
		while (c != 0) {
			uint64_t ns = FNU_FUTEX_FOREVER;

			if (at != NULL && (ns = until(at)) == 0)
				return thrd_timedout;
			fnu_futex(&m->lock, FNU_FUTEX_WAIT | FNU_FUTEX_PRIVATE, 2, ns, 0);
			c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
		}
	}
	m->owner = self;
	m->depth = 1;
	return thrd_success;
}

int
mtx_lock(mtx_t *m)
{
	return mtx_take(m, NULL);
}

int
mtx_timedlock(mtx_t *m, const struct timespec *at)
{
	if (!(m->type & mtx_timed))
		return thrd_error;
	return mtx_take(m, at);
}

int
mtx_trylock(mtx_t *m)
{
	thrd_t self = thrd_current();

	if ((m->type & mtx_recursive) && m->owner == self) {
		m->depth++;
		return thrd_success;
	}
	if (!_lock_try(&m->lock))
		return thrd_busy;
	m->owner = self;
	m->depth = 1;
	return thrd_success;
}

int
mtx_unlock(mtx_t *m)
{
	if (m->owner != thrd_current())
		return thrd_error;
	if (--m->depth > 0)
		return thrd_success;
	m->owner = NULL;
	_lock_give(&m->lock);
	return thrd_success;
}

// CONDITIONS
// a sequence moves on every signal; sleep while it is unchanged
int
cnd_init(cnd_t *c)
{
	c->seq = 0;
	return thrd_success;
}

void
cnd_destroy(cnd_t *c)
{
	(void)c;
}

int
cnd_signal(cnd_t *c)
{
	__atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
	fnu_futex(&c->seq, FNU_FUTEX_WAKE | FNU_FUTEX_PRIVATE, 1, 0, 0);
	return thrd_success;
}

int
cnd_broadcast(cnd_t *c)
{
	__atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
	fnu_futex(&c->seq, FNU_FUTEX_WAKE | FNU_FUTEX_PRIVATE, ~0u, 0, 0);
	return thrd_success;
}

static int
cnd_sleep(cnd_t *c, mtx_t *m, const struct timespec *at)
{
	unsigned int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
	unsigned int depth = m->depth;
	uint64_t ns = FNU_FUTEX_FOREVER;
	long r = 0;

	if (m->owner != thrd_current())
		return thrd_error;
	m->depth = 1;
	mtx_unlock(m);
	if (at == NULL || (ns = until(at)) != 0)
		r = fnu_futex(&c->seq, FNU_FUTEX_WAIT | FNU_FUTEX_PRIVATE, seq, ns, 0);
	mtx_take(m, NULL);
	m->depth = depth;
	if (at != NULL && (ns == 0 || r == -FNU_ERR_TIMEDOUT))
		return thrd_timedout;
	return thrd_success;
}

int
cnd_wait(cnd_t *c, mtx_t *m)
{
	return cnd_sleep(c, m, NULL);
}

int
cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *at)
{
	return cnd_sleep(c, m, at);
}

// THREAD-SPECIFIC STORAGE
int
tss_create(tss_t *key, tss_dtor_t dtor)
{
	int i;

	_lock_take(&keys_lock);
	for (i = 0; i < TSS_MAX; i++)
		if (!keys[i].used) {
			keys[i].used = 1;
			keys[i].dtor = dtor;
			_lock_give(&keys_lock);
			*key = (tss_t)i;
			return thrd_success;
		}
	_lock_give(&keys_lock);
	return thrd_error;
}

void
tss_delete(tss_t key)
{
	if (key >= TSS_MAX)
		return;
	_lock_take(&keys_lock);
	keys[key].used = 0;
	keys[key].dtor = NULL;
	_lock_give(&keys_lock);
}

void *
tss_get(tss_t key)
{
	return key < TSS_MAX && keys[key].used ? thrd_current()->tss[key] : NULL;
}

int
tss_set(tss_t key, void *v)
{
	if (key >= TSS_MAX || !keys[key].used)
		return thrd_error;
	thrd_current()->tss[key] = v;
	return thrd_success;
}

// ONCE: 0 нет, 1 идёт, 2 готово. опоздавшие ждут
void
call_once(once_flag *f, void (*fn)(void))
{
	unsigned int c = 0;

	if (__atomic_load_n(&f->state, __ATOMIC_ACQUIRE) == 2)
		return;
	if (__atomic_compare_exchange_n(&f->state, &c, 1, 0, __ATOMIC_ACQ_REL,
	    __ATOMIC_ACQUIRE)) {
		fn();
		__atomic_store_n(&f->state, 2, __ATOMIC_RELEASE);
		fnu_futex(&f->state, FNU_FUTEX_WAKE | FNU_FUTEX_PRIVATE, ~0u, 0, 0);
		return;
	}
	while ((c = __atomic_load_n(&f->state, __ATOMIC_ACQUIRE)) != 2)
		fnu_futex(&f->state, FNU_FUTEX_WAIT | FNU_FUTEX_PRIVATE, c,
		    FNU_FUTEX_FOREVER, 0);
}

// THE TAGS: a lock and a key, made on first use
// the first thread keeps the static copy
struct tag {
	volatile unsigned int lock;
	int keyed;
	tss_t key;
};

static volatile unsigned int tags_lock;

static struct tag *
tag_of(void **tag)
{
	struct tag *t;

	if ((t = __atomic_load_n((struct tag **)tag, __ATOMIC_ACQUIRE)) != NULL)
		return t;
	_lock_take(&tags_lock);
	if ((t = *tag) == NULL && (t = calloc(1, sizeof(*t))) != NULL)
		__atomic_store_n((struct tag **)tag, t, __ATOMIC_RELEASE);
	_lock_give(&tags_lock);
	return t;
}

void
_thread_tag_lock(void **tag)
{
	struct tag *t;

	if (__isthreaded && (t = tag_of(tag)) != NULL)
		_lock_take(&t->lock);
}

void
_thread_tag_unlock(void **tag)
{
	struct tag *t;

	if (__isthreaded && (t = tag_of(tag)) != NULL)
		_lock_give(&t->lock);
}

void *
_thread_tag_storage(void **tag, void *storage, size_t sz, void *err)
{
	struct tag *t;
	void *v;

	if (!__isthreaded || thrd_current() == &main_thread)
		return storage;
	if ((t = tag_of(tag)) == NULL)
		return err;
	_lock_take(&t->lock);
	if (!t->keyed) {
		if (tss_create(&t->key, free) != thrd_success) {
			_lock_give(&t->lock);
			return err;
		}
		t->keyed = 1;
	}
	_lock_give(&t->lock);
	if ((v = tss_get(t->key)) == NULL) {
		if ((v = calloc(1, sz)) == NULL)
			return err;
		memcpy(v, storage, sz);
		tss_set(t->key, v);
	}
	return v;
}
