#include <sys/select.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

// select and poll: one question, one call

uint64_t __set_to_native(sigset_t set);

// mask in the system's numbering
static int ready_wait(struct fnu_ready *list, int n, const struct fnu_timespec *ts,
    const sigset_t *mask)
{
	uint64_t native;

	if (mask != 0)
		native = __set_to_native(*mask);
	return (int)__posix(fnu_ready(list, n, ts, mask != 0 ? &native : 0));
}

// short lists live on the stack, long ones are made
static struct fnu_ready *
ready_list(struct fnu_ready *onstack, size_t n)
{
	if (n <= FNU_READY_MAX)
		return (onstack);
	return (malloc(n * sizeof(struct fnu_ready)));
}

// the kernel sets the mask for the wait, no signal slips in
static int
poll_over_ready(struct pollfd *fds, nfds_t nfds, const struct fnu_timespec *ts,
    const sigset_t *mask)
{
	struct fnu_ready stack[FNU_READY_MAX], *list;
	nfds_t i;
	int n;

	if (nfds > FNU_READY_LIMIT) {
		errno = EINVAL;
		return (-1);
	}
	if ((list = ready_list(stack, nfds)) == 0) {
		errno = ENOMEM;
		return (-1);
	}
	for (i = 0; i < nfds; i++) {
		list[i].fd = fds[i].fd;
		list[i].want = 0;
		list[i].got = 0;
		if (fds[i].events & (POLLIN | POLLRDNORM))
			list[i].want |= FNU_READY_IN;
		if (fds[i].events & (POLLOUT | POLLWRNORM))
			list[i].want |= FNU_READY_OUT;
		if (fds[i].events & (POLLPRI | POLLRDBAND))
			list[i].want |= FNU_READY_PRI;
	}
	if ((n = ready_wait(list, (int)nfds, ts, mask)) < 0) {
		if (list != stack)
			free(list);
		return (-1);
	}
	for (i = 0; i < nfds; i++) {
		fds[i].revents = 0;
		if (list[i].got & FNU_READY_IN)
			fds[i].revents |= POLLIN | POLLRDNORM;
		if (list[i].got & FNU_READY_OUT)
			fds[i].revents |= POLLOUT | POLLWRNORM;
		if (list[i].got & FNU_READY_PRI)
			fds[i].revents |= POLLPRI;
		if (list[i].got & FNU_READY_HUP)
			fds[i].revents |= POLLHUP;
		if (list[i].got & FNU_READY_ERR)
			fds[i].revents |= POLLERR;
		if (list[i].got & FNU_READY_BAD)
			fds[i].revents |= POLLNVAL;
	}
	if (list != stack)
		free(list);
	return (n);
}

int
poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	struct fnu_timespec ts;

	if (timeout >= 0) {
		ts.sec = timeout / 1000;
		ts.nsec = (timeout % 1000) * 1000000;
		ts.pad = 0;
	}
	return (poll_over_ready(fds, nfds, timeout >= 0 ? &ts : 0, 0));
}

int
ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
    const sigset_t *sigmask)
{
	struct fnu_timespec ts;

	if (timeout != 0) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
		    timeout->tv_nsec >= 1000000000) {
			errno = EINVAL;
			return (-1);
		}
		ts.sec = timeout->tv_sec;
		ts.nsec = (int32_t)timeout->tv_nsec;
		ts.pad = 0;
	}
	return (poll_over_ready(fds, nfds, timeout != 0 ? &ts : 0, sigmask));
}

// an fd set is a poll list with the bits spread out
static int
select_over_ready(int nfds, fd_set *rd, fd_set *wr, fd_set *ex,
    const struct fnu_timespec *ts, const sigset_t *mask)
{
	struct fnu_ready stack[FNU_READY_MAX], *list;
	int i, n = 0, count, result;

	for (i = 0; i < nfds; i++)
		if ((rd != 0 && FD_ISSET(i, rd)) || (wr != 0 && FD_ISSET(i, wr)) ||
		    (ex != 0 && FD_ISSET(i, ex)))
			n++;
	if ((list = ready_list(stack, (size_t)n)) == 0) {
		errno = ENOMEM;
		return (-1);
	}
	n = 0;
	for (i = 0; i < nfds; i++) {
		int want = 0;

		if (rd != 0 && FD_ISSET(i, rd))
			want |= FNU_READY_IN;
		if (wr != 0 && FD_ISSET(i, wr))
			want |= FNU_READY_OUT;
		if (ex != 0 && FD_ISSET(i, ex))
			want |= FNU_READY_PRI;
		if (want == 0)
			continue;
		list[n].fd = i;
		list[n].want = (unsigned short)want;
		list[n].got = 0;
		n++;
	}
	if ((count = ready_wait(list, n, ts, mask)) < 0) {
		if (list != stack)
			free(list);
		return (-1);
	}
	result = 0;
	if (rd != 0)
		FD_ZERO(rd);
	if (wr != 0)
		FD_ZERO(wr);
	if (ex != 0)
		FD_ZERO(ex);
	count = 0;
	for (i = 0; i < n; i++) {
		// select folds a hangup and an error into readability
		if (rd != 0 && (list[i].want & FNU_READY_IN) &&
		    (list[i].got & (FNU_READY_IN | FNU_READY_HUP | FNU_READY_ERR))) {
			FD_SET(list[i].fd, rd);
			count++;
		}
		if (wr != 0 && (list[i].want & FNU_READY_OUT) &&
		    (list[i].got & (FNU_READY_OUT | FNU_READY_ERR))) {
			FD_SET(list[i].fd, wr);
			count++;
		}
		if (ex != 0 && (list[i].got & FNU_READY_PRI)) {
			FD_SET(list[i].fd, ex);
			count++;
		}
		if (list[i].got & FNU_READY_BAD) {
			result = -1;
			break;
		}
	}
	if (list != stack)
		free(list);
	if (result < 0) {
		errno = EBADF;
		return (-1);
	}
	return (count);
}

int
select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
	struct fnu_timespec ts;

	if (nfds < 0 || nfds > FD_SETSIZE) {
		errno = EINVAL;
		return (-1);
	}
	if (tv != 0) {
		if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
			errno = EINVAL;
			return (-1);
		}
		ts.sec = tv->tv_sec;
		ts.nsec = (int32_t)(tv->tv_usec * 1000);
		ts.pad = 0;
	}
	return (select_over_ready(nfds, rd, wr, ex, tv != 0 ? &ts : 0, 0));
}

int
pselect(int nfds, fd_set *rd, fd_set *wr, fd_set *ex,
    const struct timespec *timeout, const sigset_t *sigmask)
{
	struct fnu_timespec ts;

	if (nfds < 0 || nfds > FD_SETSIZE) {
		errno = EINVAL;
		return (-1);
	}
	if (timeout != 0) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
		    timeout->tv_nsec >= 1000000000) {
			errno = EINVAL;
			return (-1);
		}
		ts.sec = timeout->tv_sec;
		ts.nsec = (int32_t)timeout->tv_nsec;
		ts.pad = 0;
	}
	return (select_over_ready(nfds, rd, wr, ex, timeout != 0 ? &ts : 0, sigmask));
}
