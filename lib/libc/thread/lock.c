// THE LOCKS libc keeps for itself, on the futex
// nothing to lock before a second thread
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>
#include <tib.h>
#include "thread_private.h"

void
_lock_take(volatile unsigned int *l)
{
	unsigned int c = 0;

	if (__atomic_compare_exchange_n(l, &c, 1, 0, __ATOMIC_ACQUIRE,
	    __ATOMIC_RELAXED))
		return;
	if (c != 2)
		c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
	while (c != 0) {
		fnu_futex((uint32_t *)l, FNU_FUTEX_WAIT | FNU_FUTEX_PRIVATE, 2,
		    FNU_FUTEX_FOREVER, 0);
		c = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
	}
}

int
_lock_try(volatile unsigned int *l)
{
	unsigned int c = 0;

	return __atomic_compare_exchange_n(l, &c, 1, 0, __ATOMIC_ACQUIRE,
	    __ATOMIC_RELAXED);
}

void
_lock_give(volatile unsigned int *l)
{
	if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2)
		fnu_futex((uint32_t *)l, FNU_FUTEX_WAKE | FNU_FUTEX_PRIVATE, 1, 0, 0);
}

// a thread_private mutex is the word its pointer's room holds
#define WORD(m) ((volatile unsigned int *)(void *)(m))

void
_thread_mutex_lock(void **m)
{
	_lock_take(WORD(m));
}

void
_thread_mutex_unlock(void **m)
{
	_lock_give(WORD(m));
}

void
_thread_mutex_destroy(void **m)
{
	*m = NULL;
}

static volatile unsigned int malloc_locks[_MALLOC_MUTEXES];
static volatile unsigned int atexit_lock, arc4_lock, atfork_lock;

void
_thread_malloc_lock(int i)
{
	_lock_take(&malloc_locks[i]);
}

void
_thread_malloc_unlock(int i)
{
	_lock_give(&malloc_locks[i]);
}

void
_thread_atexit_lock(void)
{
	_lock_take(&atexit_lock);
}

void
_thread_atexit_unlock(void)
{
	_lock_give(&atexit_lock);
}

void
_thread_arc4_lock(void)
{
	_lock_take(&arc4_lock);
}

void
_thread_arc4_unlock(void)
{
	_lock_give(&arc4_lock);
}

void
_thread_atfork_lock(void)
{
	_lock_take(&atfork_lock);
}

void
_thread_atfork_unlock(void)
{
	_lock_give(&atfork_lock);
}

// held across fork; the child starts with them free
void
_thread_fork_prepare(void)
{
	int i;

	_thread_atexit_lock();
	for (i = 0; i < _MALLOC_MUTEXES; i++)
		_thread_malloc_lock(i);
	_thread_arc4_lock();
}

void
_thread_fork_parent(void)
{
	int i;

	_thread_arc4_unlock();
	for (i = _MALLOC_MUTEXES - 1; i >= 0; i--)
		_thread_malloc_unlock(i);
	_thread_atexit_unlock();
}

// the child's arc4random is the parent's: stir it before use
volatile int _thread_arc4_forked;

void
_thread_fork_child(void)
{
	int i;

	arc4_lock = 0;
	_thread_arc4_forked = 1;
	for (i = 0; i < _MALLOC_MUTEXES; i++)
		malloc_locks[i] = 0;
	atexit_lock = 0;
	TIB_GET()->tib_tid = (pid_t)fnu_thread_id();
}

// A FILE'S LOCK: its owner may take it again
void
__rcmtx_init(struct __rcmtx *m)
{
	m->owner = NULL;
	m->depth = 0;
	m->lock = 0;
}

int
__rcmtx_enter_try(struct __rcmtx *m)
{
	void *self = TIB_GET();

	if (m->owner == self) {
		m->depth++;
		return 1;
	}
	if (!_lock_try(&m->lock))
		return 0;
	m->owner = self;
	m->depth = 1;
	return 1;
}

void
__rcmtx_enter(struct __rcmtx *m)
{
	void *self = TIB_GET();

	if (m->owner == self) {
		m->depth++;
		return;
	}
	_lock_take(&m->lock);
	m->owner = self;
	m->depth = 1;
}

void
__rcmtx_leave(struct __rcmtx *m)
{
	if (--m->depth == 0) {
		m->owner = NULL;
		_lock_give(&m->lock);
	}
}
