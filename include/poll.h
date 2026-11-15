#ifndef _POLL_H_
#define _POLL_H_

// poll(2), in the shapes every UNIX gives it

#include <sys/types.h>
#include <signal.h>
#include <time.h>

typedef struct pollfd {
	int	fd;
	short	events;
	short	revents;
} pollfd_t;

typedef unsigned int	nfds_t;

#define	POLLIN		0x0001
#define	POLLPRI		0x0002
#define	POLLOUT		0x0004
#define	POLLERR		0x0008
#define	POLLHUP		0x0010
#define	POLLNVAL	0x0020
#define	POLLRDNORM	0x0040
#define	POLLNORM	POLLRDNORM
#define	POLLWRNORM	POLLOUT
#define	POLLRDBAND	0x0080
#define	POLLWRBAND	0x0100

#define	INFTIM		(-1)

__BEGIN_DECLS
int poll(struct pollfd *, nfds_t, int);
int ppoll(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
__END_DECLS

#endif
