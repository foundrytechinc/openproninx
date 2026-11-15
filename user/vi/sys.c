/* the system's own calls, as vi uses them */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>
#include "vi.h"

static struct fnu_ttymode saved;
static int saved_ok;
static char **envp;

void sys_init(char **env)
{
	envp = env;
}

static void sys_errno(long r)
{
	if (r < 0)
		errno = (int)-r;
}

int sys_open(char *path, int write, int mode)
{
	int fd = fnu_open(FNU_FD_CWD, path,
		write ? FNU_OPEN_WRITE | FNU_OPEN_CREATE : FNU_OPEN_READ, mode);
	sys_errno(fd);
	return fd < 0 ? -1 : fd;
}

int sys_truncate(int fd, long len)
{
	struct fnu_setattr a;
	int r;
	memset(&a, 0, sizeof(a));
	a.mask = FNU_SET_SIZE;
	a.size = (uint64_t) len;
	r = fnu_setattr(fd, NULL, 0, &a);
	sys_errno(r);
	return r < 0 ? -1 : 0;
}

/* one segment, at the file's own offset */
static long rw(int fd, void *buf, long n, int write)
{
	struct fnu_segment seg;
	seg.base = buf;
	seg.len = (size_t) n;
	return write ? fnu_write(fd, &seg, 1, FNU_IO_HERE) :
		fnu_read(fd, &seg, 1, FNU_IO_HERE);
}

long sys_read(int fd, void *buf, long n)
{
	long r;
	while ((r = rw(fd, buf, n, 0)) == -FNU_ERR_INTERRUPTED)
		;
	sys_errno(r);
	return r < 0 ? -1 : r;
}

long sys_write(int fd, void *buf, long n)
{
	long done = 0, r;
	while (done < n) {
		r = rw(fd, (char *) buf + done, n - done, 1);
		if (r == -FNU_ERR_INTERRUPTED)
			continue;
		if (r <= 0) {
			sys_errno(r);
			return -1;
		}
		done += r;
	}
	return done;
}

long sys_write_some(int fd, void *buf, long n)
{
	long r = rw(fd, buf, n, 1);
	sys_errno(r);
	return r < 0 ? -1 : r;
}

int sys_close(int fd)
{
	if (fd < 0)
		return 0;
	return fnu_close(fd) < 0 ? -1 : 0;
}

int sys_readable(char *path)
{
	return fnu_access(FNU_FD_CWD, path, FNU_ACCESS_READ, 0) == 0;
}

long sys_mtime(char *path)
{
	struct fnu_stat st;
	if (fnu_getattr(FNU_FD_CWD, path, 0, &st) < 0)
		return -1;
	return (long) st.mtime.sec;
}

int sys_cwd(char *buf, int len)
{
	return fnu_pathof(FNU_FD_CWD, NULL, buf, len) < 0 ? -1 : 0;
}

int sys_isterm(int fd)
{
	struct fnu_ttymode t;
	return fnu_ioctl(fd, FNU_TTYGETMODE, (uint64_t) (uintptr_t) &t) == 0;
}

void sys_termraw(void)
{
	struct fnu_ttymode t;
	if (fnu_ioctl(0, FNU_TTYGETMODE, (uint64_t) (uintptr_t) &saved) < 0)
		return;
	saved_ok = 1;
	t = saved;
	t.edit &= ~(FNU_TTYEDIT_LINES | FNU_TTYEDIT_SIGNALS | FNU_TTYEDIT_ECHO);
	fnu_ioctl(0, FNU_TTYSETMODE_FLUSH, (uint64_t) (uintptr_t) &t);
}

void sys_termrestore(void)
{
	if (saved_ok)
		fnu_ioctl(0, FNU_TTYSETMODE, (uint64_t) (uintptr_t) &saved);
}

void sys_termlines(int on)
{
	struct fnu_ttymode t;
	if (fnu_ioctl(0, FNU_TTYGETMODE, (uint64_t) (uintptr_t) &t) < 0)
		return;
	if (on)
		t.edit |= FNU_TTYEDIT_LINES | FNU_TTYEDIT_ECHO;
	else
		t.edit &= ~(FNU_TTYEDIT_LINES | FNU_TTYEDIT_ECHO);
	fnu_ioctl(0, FNU_TTYSETMODE_FLUSH, (uint64_t) (uintptr_t) &t);
}

int sys_termsize(int *rows, int *cols)
{
	struct fnu_ttysize w;
	if (fnu_ioctl(0, FNU_TTYGETSIZE, (uint64_t) (uintptr_t) &w) < 0)
		return -1;
	*rows = w.rows;
	*cols = w.cols;
	return 0;
}

/* ms < 0 waits for ever */
int sys_wait(struct sysfd *fds, int n, int ms)
{
	struct fnu_ready list[8];
	struct fnu_timespec ts;
	int i, k = 0, r;
	for (i = 0; i < n; i++) {
		fds[i].got = 0;
		if (fds[i].fd < 0 || k == LEN(list))
			continue;
		list[k].fd = fds[i].fd;
		list[k].want = (fds[i].want & SYS_IN ? FNU_READY_IN : 0) |
			(fds[i].want & SYS_OUT ? FNU_READY_OUT : 0);
		list[k].got = 0;
		k++;
	}
	ts.sec = ms / 1000;
	ts.nsec = (ms % 1000) * 1000000L;
	while ((r = fnu_ready(list, k, ms < 0 ? NULL : &ts, NULL)) == -FNU_ERR_INTERRUPTED)
		;
	if (r < 0)
		return -1;
	for (i = 0, k = 0; i < n; i++) {
		if (fds[i].fd < 0 || k == LEN(list))
			continue;
		if (list[k].got & FNU_READY_IN)
			fds[i].got |= SYS_IN;
		if (list[k].got & FNU_READY_OUT)
			fds[i].got |= SYS_OUT;
		if (list[k].got & (FNU_READY_HUP | FNU_READY_ERR | FNU_READY_BAD))
			fds[i].got |= SYS_GONE;
		k++;
	}
	return r;
}

static void sys_exec(char **argv)
{
	char *path = getenv("PATH");
	char full[1024];
	if (strchr(argv[0], '/') || !path) {
		fnu_exec(argv[0], argv, envp);
		return;
	}
	while (*path) {
		char *end = strchr(path, ':');
		int len = end ? end - path : (int) strlen(path);
		snprintf(full, sizeof(full), "%.*s%s%s", len, path,
			len ? "/" : "", argv[0]);
		fnu_exec(full, argv, envp);
		path += len + (end != NULL);
	}
}

/* stdin from *ifd, stdout into *ofd, stderr into errfd when >= 0 */
int sys_spawn(char **argv, int *ifd, int *ofd, int errfd, int group)
{
	int in[2] = {-1, -1}, out[2] = {-1, -1};
	int pid;
	if (ifd && fnu_pipe(in, 0) < 0)
		return -1;
	if (ofd && fnu_pipe(out, 0) < 0) {
		sys_close(in[0]);
		sys_close(in[1]);
		return -1;
	}
	if ((pid = fnu_fork()) == 0) {
		if (group)
			fnu_pgroup_set(0, 0);
		if (ifd) {
			fnu_dup(in[0], 0, FNU_DUP_EXACT);
			fnu_close(in[0]);
			fnu_close(in[1]);
		}
		if (ofd) {
			fnu_dup(out[1], 1, FNU_DUP_EXACT);
			if (errfd < 0)
				fnu_dup(out[1], 2, FNU_DUP_EXACT);
			fnu_close(out[0]);
			fnu_close(out[1]);
		}
		if (errfd >= 0) {
			fnu_dup(errfd, 2, FNU_DUP_EXACT);
			fnu_close(errfd);
		}
		sys_exec(argv);
		fnu_exit(1);
	}
	sys_close(in[0]);
	sys_close(out[1]);
	if (pid < 0) {
		sys_close(in[1]);
		sys_close(out[0]);
		return -1;
	}
	if (ifd)
		*ifd = in[1];
	if (ofd)
		*ofd = out[0];
	return pid;
}

int sys_reap(int pid, int hang)
{
	struct fnu_exitinfo ei;
	return fnu_wait(pid, &ei, hang ? 0 : FNU_WAIT_NOHANG) == pid;
}

void sys_interrupt(int pgrp)
{
	fnu_signal(-pgrp, FNU_SIG_INTERRUPT);
}

void sys_end(int pid, int hard)
{
	fnu_signal(pid, hard ? FNU_SIG_KILL : FNU_SIG_TERMINATE);
}

void sys_stop(void)
{
	fnu_signal(0, FNU_SIG_STOP);
}

void sys_pause(int ms)
{
	struct fnu_timespec ts;
	ts.sec = ms / 1000;
	ts.nsec = (ms % 1000) * 1000000L;
	fnu_sleep(&ts, NULL);
}

void sys_onresize(void (*fn)(int))
{
	struct fnu_sigact sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = (uint64_t) (uintptr_t) fn;
	sa.flags = FNU_SIGACT_RESTART;
	fnu_sigact(FNU_SIG_WINDOW, &sa, NULL);
}

void sys_nopipe(void)
{
	struct fnu_sigact sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = (uint64_t) (uintptr_t) FNU_SIGACT_IGNORE;
	fnu_sigact(FNU_SIG_BROKENPIPE, &sa, NULL);
}

void sys_private(int fd)
{
	struct fnu_fdstate st;
	st.fdflags = FNU_FD_CLOSEEXEC;
	st.fileflags = FNU_OPEN_NOWAIT;
	fnu_fdstate_set(fd, &st);
}
