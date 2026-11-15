#ifndef _SYS_SELECT_H_
#define _SYS_SELECT_H_

#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

struct timeval;
struct timespec;

#define FD_SETSIZE 1024
typedef uint32_t __fd_mask;
#define NFDBITS 32
#define __howmany(x, y) (((x) + ((y) - 1)) / (y))
#define NBBY 8
typedef __fd_mask fd_mask;
#ifndef howmany
#define howmany(x, y) __howmany(x, y)
#endif

typedef struct fd_set {
  __fd_mask fds_bits[__howmany(FD_SETSIZE, NFDBITS)];
} fd_set;

#define FD_SET(n, p) ((p)->fds_bits[(n) / NFDBITS] |= (1u << ((n) % NFDBITS)))
#define FD_CLR(n, p) ((p)->fds_bits[(n) / NFDBITS] &= ~(1u << ((n) % NFDBITS)))
#define FD_ISSET(n, p) (((p)->fds_bits[(n) / NFDBITS] & (1u << ((n) % NFDBITS))) != 0)
#define FD_ZERO(p) memset((p), 0, sizeof(*(p)))
#define FD_COPY(f, t) memcpy((t), (f), sizeof(*(f)))

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask);

#endif
