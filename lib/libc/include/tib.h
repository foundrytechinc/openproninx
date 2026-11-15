// THE THREAD BLOCK: %fs:0 points at itself, tls data just under it
#ifndef _LIBC_TIB_H_
#define _LIBC_TIB_H_

#include <sys/types.h>

struct tib {
	struct tib *__tib_self;
	void	*tib_dtv;
	void	*tib_thread;
	void	*tib_locale;
	void	*tib_atexit;
	int	tib_thread_flags;
	pid_t	tib_tid;
	int	tib_cantcancel;
	int	tib_cancel_point;
	int	tib_canceled;
	int	tib_errno;
};

static inline struct tib *
__tib_get(void)
{
	struct tib *t;

	__asm__ volatile("movq %%fs:0, %0" : "=r"(t));
	return t;
}

#define TIB_GET()	__tib_get()
#define TIB_INIT(t, tcb, thread) do {					\
	(t)->__tib_self = (t);						\
	(t)->tib_thread = (thread);					\
} while (0)

struct tib *_libc_tib_new(void);
void	_libc_tib_free(struct tib *);
void	_libc_tib_init(void);

#endif /* _LIBC_TIB_H_ */
