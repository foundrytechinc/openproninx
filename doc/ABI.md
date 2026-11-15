# OpenProninx system calls

System calls use `int $0x30`. The number goes in `%rax` and the arguments in
`%rdi %rsi %rdx %rcx %r8 %r9`. On success the carry flag is clear and `%rax`
holds the result. On failure the carry flag is set and `%rax` holds an
`FNU_ERR_*` code from [`inc/error.h`](../inc/error.h). The `libfnu` stubs
return the result or the negative error. There is no `errno`.

Numbers are in [`inc/syscall.h`](../inc/syscall.h). Structures and constants
are in [`inc/abi.h`](../inc/abi.h), [`inc/open.h`](../inc/open.h),
[`inc/stat.h`](../inc/stat.h), [`inc/signal.h`](../inc/signal.h) and
[`inc/dirent.h`](../inc/dirent.h). The signatures below are from
[`inc/fnu.h`](../inc/fnu.h).

POSIX lives in `libc` on top of these calls. `tests/fnuonly.c` links with
`libfnu` alone to prove nothing else is needed.

Calls that take a path take it relative to a directory descriptor, or
`FNU_FD_CWD`. A null path means the descriptor itself, so
`fnu_getattr(fd, 0, 0, &st)` is `fstat`.

Under `pledge`, each call needs the promises listed for it in
[`kern/pledge.c`](../kern/pledge.c).

## Processes

| # | Signature | Notes |
|---|-----------|-------|
| 0 | `void fnu_exit(int status) __attribute__((noreturn))` | the low eight bits are what `fnu_wait` reports; every thread ends |
| 1 | `int fnu_fork(void)` | 0 in the child, the child's pid in the parent |
| 2 | `int fnu_exec(const char *path, char *const argv[], char *const envp[])` | the image of a static ELF; descriptors marked `FNU_FD_CLOSEEXEC` are closed |
| 3 | `pid_t fnu_wait(pid_t pid, struct fnu_exitinfo *out, int flags)` | pid -1 any child, 0 the caller's group, below -1 that group; `FNU_WAIT_NOHANG`, `_STOPPED`, `_CONTINUED`; `out` says why (`FNU_EXIT_*`) and carries the child's usage |
| 4 | `int fnu_signal(pid_t pid, int sig)` | pid 0 the caller's group, -1 every process it may signal, below -1 a group; signal 0 only asks |
| 5 | `int fnu_procids(pid_t pid, struct fnu_procids *out)` | pid, parent, group and session of a process, pid 0 the caller |
| 6 | `pid_t fnu_session_new(void)` | new session and group; fails for a group leader |
| 7 | `int fnu_pgroup_set(pid_t pid, pid_t pgid)` | into a group of the same session, or a new one; pid 0 the caller, pgid 0 its pid |
| 8 | `int fnu_ident_get(struct fnu_ident *out)` | real, effective and saved ids, the groups, the session's login name, and whether the ids changed since the last exec |
| 9 | `int fnu_ident_set(const struct fnu_ident *in, int what)` | `FNU_IDENT_UID` and `_GID` work like setuid(2). `_RUID`, `_EUID`, `_SVUID`, `_RGID`, `_EGID`, `_SVGID` set one id each. `_GROUPS` and `_LOGIN` are root only. All or nothing |
| 10 | `int fnu_niceness(int whom, int id, int how, int *value)` | `FNU_NICE_PROCESS`, `_GROUP`, `_USER`; `FNU_NICE_GET`, `_SET`, `_ADD`. Only root can lower it |
| 11 | `uint32_t fnu_createmask(uint32_t mask)` | umask; returns the old one |
| 12 | `int fnu_limit(int which, const struct fnu_limit *in, struct fnu_limit *out)` | `FNU_LIMIT_*`; `in` and `out` may each be null |
| 13 | `int fnu_usage(int who, struct fnu_usage *out)` | `FNU_USAGE_SELF` or `FNU_USAGE_CHILDREN` |
| 14 | `int fnu_timer(int which, const struct fnu_timer *in, struct fnu_timer *out)` | `FNU_TIMER_REAL`, `_VIRTUAL`, `_PROFILE`; as `fnu_limit` |
| 15 | `int fnu_sleep(const struct fnu_timespec *want, struct fnu_timespec *left)` | a signal interrupts it and `left` has the rest |
| 16 | `int fnu_halt(int howto)` | `FNU_HALT_*`; init's alone unless `FNU_HALT_FORCE`; root only |
| 17 | `int fnu_reboot(void)` | asks init; root only |
| 18 | `int fnu_poweroff(void)` | asks init; root only |
| 19 | `long fnu_ptrace(int request, pid_t pid, uintptr_t addr, int64_t data)` | the 4.3BSD requests, `PT_*` in abi.h |

## Signals

| # | Signature | Notes |
|---|-----------|-------|
| 20 | `int fnu_sigact(int sig, const struct fnu_sigact *act, struct fnu_sigact *old)` | handler `FNU_SIGACT_DEFAULT`, `_IGNORE` or an address; a 64-bit mask; `FNU_SIGACT_ONCE`, `_REENTER`, `_RESTART`, `_ALTSTACK` |
| 21 | `uint64_t fnu_sigmask(uint64_t block, uint64_t unblock)` | blocks, then unblocks; the mask before comes back. `FNU_SIG_KILL` and `FNU_SIG_STOP` are never blocked |
| 22 | `uint64_t fnu_sigraised(void)` | raised and not yet acted on |
| 23 | `int fnu_sigtake(uint64_t set, struct fnu_siginfo *out, int block)` | one signal out of `set`, waiting for it when `block` is set |
| 24 | `int fnu_sigpause(uint64_t mask)` | wait with `mask` until a handler runs; returns `FNU_ERR_INTERRUPTED` |
| 25 | `fnu_sigreturn` | used by the signal trampoline only |
| 26 | `int fnu_altstack(const struct fnu_altstack *in, struct fnu_altstack *out)` | `FNU_SIGSTACK_*` |

## Threads

| # | Signature | Notes |
|---|-----------|-------|
| 30 | `long fnu_thread_start(const struct fnu_thread_start *start)` | a thread at `entry(arg)` on `stack` with `tcb` as its thread pointer; its id comes back |
| 31 | `void fnu_thread_exit(uint32_t *cleared) __attribute__((noreturn))` | `*cleared` becomes 0 and its futex waiters are woken; the last thread ends the process |
| 32 | `long fnu_thread_tcb(uint64_t tcb)` | sets `%fs`; a non-canonical address is `FNU_ERR_INVALID` |
| 33 | `long fnu_thread_id(void)` | never a pid |
| 34 | `void fnu_thread_yield(void)` |  |
| 35 | `long fnu_futex(uint32_t *word, int op, uint32_t val, uint64_t nsec, uint32_t *to)` | `FNU_FUTEX_WAIT` while `*word == val`, `_WAKE` up to `val`, `_REQUEUE`; `FNU_FUTEX_PRIVATE` keys the word by process and address |

## Descriptors

| # | Signature | Notes |
|---|-----------|-------|
| 40 | `long fnu_read(int fd, const struct fnu_segment *seg, int nseg, int64_t at)` | the segments in order; at `at`, or with `FNU_IO_HERE` at and past the descriptor's own offset |
| 41 | `long fnu_write(int fd, const struct fnu_segment *seg, int nseg, int64_t at)` | as `fnu_read` |
| 42 | `long fnu_seek(int fd, int64_t offset, int whence)` | `FNU_SEEK_START`, `_HERE`, `_TAIL`; on a directory the offset is the filesystem's cookie |
| 43 | `int fnu_close(int fd)` |  |
| 44 | `int fnu_dup(int fd, int to, int flags)` | the lowest free descriptor at or above `to`; with `FNU_DUP_EXACT` `to` itself, closing what was there. `FNU_FD_CLOSEEXEC` in `flags` |
| 45 | `int fnu_pipe(int fds[2], int flags)` | `FNU_FD_CLOSEEXEC`, `FNU_FD_NOWAIT`; writes of `FNU_PIPE_ATOMIC` bytes or less are not interleaved |
| 46 | `int fnu_ioctl(int fd, uint64_t request, uint64_t arg)` | encoded like `sys/ioccom.h`; `FNU_TTY*`, `FNU_KB*`, `FNU_FB*`, mouse, disk and audio requests in abi.h |
| 47 | `int fnu_fdstate_get(int fd, struct fnu_fdstate *out)` | the descriptor's flags and its file's |
| 48 | `int fnu_fdstate_set(int fd, const struct fnu_fdstate *in)` |  |
| 49 | `int fnu_ready(struct fnu_ready *list, int n, const struct fnu_timespec *timeout, const uint32 *mask)` | what each descriptor can do now (`FNU_READY_*`), waiting up to `timeout`; with `mask` the signal mask for the wait alone |
| 50 | `int fnu_flush(int fd)` | write the file's volume to disk and flush the drive cache; `FNU_FLUSH_ALL` for all volumes |
| 51 | `long fnu_readdir(int fd, void *buf, int size, int64_t *basep)` | `struct fnu_dirent` records, each padded to eight bytes; `basep` takes the offset the read began at |
| 52 | `int fnu_eventq(int flags)` | new event queue (like kqueue); not inherited by fork |
| 53 | `int fnu_events(int fd, const struct fnu_event *changes, int nchanges, struct fnu_event *out, int room, const struct fnu_timespec *timeout)` | like kevent. Filters `FNU_EVF_READ`, `_WRITE`, `_FILE`, `_PROC`, `_SIGNAL`, `_TIMER`. Errors come back as `FNU_EV_ERROR` |

## Names

| # | Signature | Notes |
|---|-----------|-------|
| 60 | `int fnu_open(int dirfd, const char *path, int flags, int mode)` | `FNU_OPEN_*`; `mode` only for a file it makes |
| 61 | `int fnu_getattr(int dirfd, const char *path, int flags, struct fnu_stat *st)` | with no path, whatever `dirfd` is open on; `FNU_PATH_NOFOLLOW` asks about a final link itself |
| 62 | `int fnu_setattr(int dirfd, const char *path, int flags, const struct fnu_setattr *attr)` | `FNU_SET_SIZE`, `_MODE`, `_UID`, `_GID`, `_FLAGS`, `_ATIME`, `_MTIME`. `FNU_TIME_NOW` and `FNU_TIME_KEEP` go in `nsec` |
| 63 | `int fnu_make(int dirfd, const char *path, int type, int mode, const char *target)` | `FNU_TYPE_DIR`, `FNU_TYPE_FIFO` or `FNU_TYPE_LINK` to `target` |
| 64 | `int fnu_remove(int dirfd, const char *path, int flags)` | a name; a directory, empty, only with `FNU_REMOVE_DIR` |
| 65 | `int fnu_link(int fromfd, const char *from, int tofd, const char *to, int flags)` | a second name; a final link of `from` is followed only with `FNU_PATH_FOLLOW`; not on FAT |
| 66 | `int fnu_rename(int fromfd, const char *from, int tofd, const char *to)` | within one volume |
| 67 | `long fnu_readlink(int dirfd, const char *path, char *buf, int size)` | the length of what the link says; no terminator |
| 68 | `int fnu_chdir(int dirfd, const char *path)` | with no path, the directory `dirfd` is open on |
| 69 | `int fnu_access(int dirfd, const char *path, int how, int flags)` | `FNU_ACCESS_READ`, `_WRITE`, `_EXEC` for the effective user, or with `FNU_ACCESS_REAL` the real one |
| 70 | `int64_t fnu_pathconf(int dirfd, const char *path, int flags, int name)` | `FNU_PC_*`: what the filesystem under the name can do |
| 71 | `int fnu_pathof(int dirfd, const char *path, char *buf, int size)` | the absolute name, every link in it followed; with no path that of `dirfd` |
| 72 | `int fnu_mount(const struct fnu_mount_req *request)` | root only; type `ufs2`, `fat32`, or empty to read the device |
| 73 | `int fnu_unmount(const char *on, unsigned int flags)` | `FNU_MNT_FORCE` |
| 74 | `int fnu_fsstat(struct fnu_fsstat *buf, int capacity)` | one entry a mounted volume; capacity 0 returns the count |
| 75 | `int fnu_swapon(const char *device)` | a block device; root only |
| 76 | `int fnu_swapoff(void)` | root only |
| 77 | `int fnu_unveil(const char *path, const char *permissions)` | OpenBSD's unveil: `r`, `w`, `x`, `c`; both null locks it |

## Memory

| # | Signature | Notes |
|---|-----------|-------|
| 80 | `long fnu_map(void *addr, size_t len, int prot, int flags, int fd, int64_t offset)` | `FNU_MEM_READ`, `_WRITE`, `_EXEC`; `FNU_MEM_SHARED` or `_PRIVATE`, `_ANON`, `_FIXED`; the address comes back |
| 81 | `int fnu_unmap(void *addr, size_t len)` |  |
| 82 | `int fnu_protect(void *addr, size_t len, int prot)` | W^X |
| 83 | `int fnu_memsync(void *addr, size_t len, int flags)` | msync; `FNU_MEMSYNC_WAIT` or `_START` |
| 84 | `int fnu_memuse(void *addr, size_t len, int advice)` | madvise; `FNU_MEMUSE_*` |

## Sockets

| # | Signature | Notes |
|---|-----------|-------|
| 90 | `int fnu_socket(int domain, int type, int protocol)` | `FNU_NET_INET` or `FNU_NET_LOCAL`; `FNU_SOCKET_*` |
| 91 | `int fnu_socketpair(int domain, int type, int protocol, int fds[2])` | a connected pair of local sockets |
| 92 | `int fnu_bind(int fd, const void *addr, int len)` | port 0 picks one |
| 93 | `int fnu_listen(int fd, int backlog)` |  |
| 94 | `int fnu_accept(int fd, void *from, int *len)` | `from` may be null |
| 95 | `int fnu_connect(int fd, const void *addr, int len)` | on a datagram socket sets the default peer |
| 96 | `long fnu_send(int fd, const struct fnu_msg *msg, int flags)` | like sendmsg; descriptors only over local sockets; `FNU_SEND_*` |
| 97 | `long fnu_recv(int fd, struct fnu_msg *msg, int flags)` | like recvmsg; `FNU_RECV_*`; 0 is end of stream |
| 98 | `int fnu_shutdown(int fd, int how)` | `FNU_SHUTDOWN_READ`, `_WRITE`, `_BOTH` |
| 99 | `int fnu_sockaddr(int fd, int which, void *out, int *len)` | `FNU_SOCKADDR_LOCAL` or `_PEER` |
| 100 | `int fnu_sockopt_set(int fd, int opt, const void *val, uint32_t len)` | `FNU_SOCKOPT_*` |
| 101 | `int fnu_sockopt_get(int fd, int opt, void *val, uint32_t *len)` |  |

## The machine

| # | Signature | Notes |
|---|-----------|-------|
| 110 | `int fnu_info(struct fnu_info *inf)` | uptime, memory, swap, process count, load average |
| 111 | `int fnu_procinfo(struct fnu_procinfo *pi, int capacity)` | every process; capacity 0 returns the count |
| 112 | `int fnu_hwinfo(struct fnu_hwinfo *hw)` |  |
| 113 | `int fnu_diskinfo(struct fnu_diskinfo *buf, int capacity)` | every disk; capacity 0 returns the count |
| 114 | `int fnu_netinfo(struct fnu_ifinfo *buf, int capacity)` | every interface; capacity 0 returns the count |
| 115 | `int fnu_netconfig(struct fnu_ifconfig *config)` | an address, or a lease asked for; root only |
| 118 | `int fnu_rtctime(struct fnu_rtctime *t)` | broken down, from the battery clock |
| 119 | `int fnu_gettime(struct fnu_timespec *t, int which)` | `FNU_CLOCK_WALL` or `_UPTIME`; with `FNU_CLOCK_GRAIN` the clock's step |
| 120 | `int fnu_random(void *buf, size_t len)` | at most 256 bytes |
| 121 | `int fnu_sysctl(const char *name, void *old, uint64_t *oldlen, const void *new, uint64_t newlen)` | by dotted name; writing needs root and no pledge |
| 122 | `int fnu_sysctlnode(int index, struct fnu_sysctl_info *info)` | walk all nodes by index |
| 123 | `int fnu_pledge(const char *promises, const char *execpromises)` | OpenBSD's pledge |
| 124 | `int fnu_sendsyslog(const char *buf, size_t len, int flags)` | to syslogd; `FNU_LOG_CONSOLE` falls back to the console |
| 125 | `int fnu_settime(const struct fnu_timespec *now)` | sets the clock and the RTC; root only |
| 126 | `int fnu_slewtime(const struct fnu_timespec *delta, struct fnu_timespec *left)` | adjtime at 5000 ppm |

## Numbers

```
0   exit           1   fork           2   exec           3   wait
4   signal         5   procids        6   session_new    7   pgroup_set
8   ident_get      9   ident_set      10  niceness       11  createmask
12  limit          13  usage          14  timer          15  sleep
16  halt           17  reboot         18  poweroff       19  ptrace
20  sigact         21  sigmask        22  sigraised      23  sigtake
24  sigpause       25  sigreturn      26  altstack       30  thread_start
31  thread_exit    32  thread_tcb     33  thread_id      34  thread_yield
35  futex          40  read           41  write          42  seek
43  close          44  dup            45  pipe           46  ioctl
47  fdstate_get    48  fdstate_set    49  ready          50  flush
51  readdir        52  eventq         53  events         60  open
61  getattr        62  setattr        63  make           64  remove
65  link           66  rename         67  readlink       68  chdir
69  access         70  pathconf       71  pathof         72  mount
73  unmount        74  fsstat         75  swapon         76  swapoff
77  unveil         80  map            81  unmap          82  protect
83  memsync        84  memuse         90  socket         91  socketpair
92  bind           93  listen         94  accept         95  connect
96  send           97  recv           98  shutdown       99  sockaddr
100 sockopt_set    101 sockopt_get    110 info           111 procinfo
112 hwinfo         113 diskinfo       114 netinfo        115 netconfig
118 rtctime        119 gettime        120 random         121 sysctl
122 sysctlnode     123 pledge         124 sendsyslog     125 settime
126 slewtime
```

There are 128 slots. Unlisted numbers are not used.
