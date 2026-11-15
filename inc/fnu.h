#ifndef PRONINX_FNU_H
#define PRONINX_FNU_H

#include "inc/types.h"
#include "inc/error.h"
#include "inc/open.h"
#include "inc/abi.h"
#include "inc/dirent.h"
#include "inc/signal.h"
#include "inc/stat.h"

// PROCESSES
void fnu_exit(int status) __attribute__((noreturn));
int fnu_fork(void);
int fnu_exec(const char *path, char *const argv[], char *const envp[]);
pid_t fnu_wait(pid_t pid, struct fnu_exitinfo *out, int flags);
int fnu_signal(pid_t pid, int sig);
int fnu_procids(pid_t pid, struct fnu_procids *out);
pid_t fnu_session_new(void);
int fnu_pgroup_set(pid_t pid, pid_t pgid);
int fnu_ident_get(struct fnu_ident *out);
int fnu_ident_set(const struct fnu_ident *in, int what);
int fnu_niceness(int whom, int id, int how, int *value);
uint32_t fnu_createmask(uint32_t mask);
int fnu_limit(int which, const struct fnu_limit *in, struct fnu_limit *out);
int fnu_usage(int who, struct fnu_usage *out);
int fnu_timer(int which, const struct fnu_timer *in, struct fnu_timer *out);
int fnu_sleep(const struct fnu_timespec *want, struct fnu_timespec *left);
int fnu_halt(int howto);
int fnu_reboot(void);
int fnu_poweroff(void);
long fnu_ptrace(int request, pid_t pid, uintptr_t addr, int64_t data);

// SIGNALS
int fnu_sigact(int sig, const struct fnu_sigact *act, struct fnu_sigact *old);
uint64_t fnu_sigmask(uint64_t block, uint64_t unblock);
uint64_t fnu_sigraised(void);
int fnu_sigtake(uint64_t set, struct fnu_siginfo *out, int block);
int fnu_sigpause(uint64_t mask);
int fnu_altstack(const struct fnu_altstack *in, struct fnu_altstack *out);

// THREADS
long fnu_thread_start(const struct fnu_thread_start *start);
void fnu_thread_exit(uint32_t *cleared) __attribute__((noreturn));
long fnu_thread_tcb(uint64_t tcb);
long fnu_thread_id(void);
void fnu_thread_yield(void);
long fnu_futex(uint32_t *word, int op, uint32_t val, uint64_t nsec, uint32_t *to);

// DESCRIPTORS
long fnu_read(int fd, const struct fnu_segment *seg, int nseg, int64_t at);
long fnu_write(int fd, const struct fnu_segment *seg, int nseg, int64_t at);
long fnu_seek(int fd, int64_t offset, int whence);
int fnu_close(int fd);
int fnu_dup(int fd, int to, int flags);
int fnu_pipe(int fds[2], int flags);
int fnu_ioctl(int fd, uint64_t request, uint64_t arg);
int fnu_fdstate_get(int fd, struct fnu_fdstate *out);
int fnu_fdstate_set(int fd, const struct fnu_fdstate *in);
int fnu_ready(struct fnu_ready *list, int n, const struct fnu_timespec *timeout,
              const uint64_t *mask);
int fnu_flush(int fd);
long fnu_readdir(int fd, void *buf, int size, int64_t *basep);
int fnu_eventq(int flags);
int fnu_events(int fd, const struct fnu_event *changes, int nchanges,
               struct fnu_event *out, int room, const struct fnu_timespec *timeout);

// NAMES
int fnu_open(int dirfd, const char *path, int flags, int mode);
int fnu_getattr(int dirfd, const char *path, int flags, struct fnu_stat *st);
int fnu_setattr(int dirfd, const char *path, int flags,
                const struct fnu_setattr *attr);
int fnu_make(int dirfd, const char *path, int type, int mode, const char *target);
int fnu_remove(int dirfd, const char *path, int flags);
int fnu_link(int fromfd, const char *from, int tofd, const char *to, int flags);
int fnu_rename(int fromfd, const char *from, int tofd, const char *to);
long fnu_readlink(int dirfd, const char *path, char *buf, int size);
int fnu_chdir(int dirfd, const char *path);
int fnu_access(int dirfd, const char *path, int how, int flags);
int64_t fnu_pathconf(int dirfd, const char *path, int flags, int name);
int fnu_pathof(int dirfd, const char *path, char *buf, int size);
int fnu_mount(const struct fnu_mount_req *request);
int fnu_unmount(const char *on, unsigned int flags);
int fnu_fsstat(struct fnu_fsstat *buf, int capacity);
int fnu_swapon(const char *device);
int fnu_swapoff(void);
int fnu_unveil(const char *path, const char *permissions);

// MEMORY
long fnu_map(void *addr, size_t len, int prot, int flags, int fd, int64_t offset);
int fnu_unmap(void *addr, size_t len);
int fnu_protect(void *addr, size_t len, int prot);
int fnu_memsync(void *addr, size_t len, int flags);
int fnu_memuse(void *addr, size_t len, int advice);

// SOCKETS
int fnu_socket(int domain, int type, int protocol);
int fnu_socketpair(int domain, int type, int protocol, int fds[2]);
int fnu_bind(int fd, const void *addr, int len);
int fnu_listen(int fd, int backlog);
int fnu_accept(int fd, void *from, int *len);
int fnu_connect(int fd, const void *addr, int len);
long fnu_send(int fd, const struct fnu_msg *msg, int flags);
long fnu_recv(int fd, struct fnu_msg *msg, int flags);
int fnu_shutdown(int fd, int how);
int fnu_sockaddr(int fd, int which, void *out, int *len);
int fnu_sockopt_set(int fd, int opt, const void *val, uint32_t len);
int fnu_sockopt_get(int fd, int opt, void *val, uint32_t *len);

int fnu_info(struct fnu_info *inf);
int fnu_procinfo(struct fnu_procinfo *pi, int capacity);
int fnu_hwinfo(struct fnu_hwinfo *hw);
int fnu_diskinfo(struct fnu_diskinfo *buf, int capacity);
int fnu_netinfo(struct fnu_ifinfo *buf, int capacity);
int fnu_netconfig(struct fnu_ifconfig *config);
int fnu_rtctime(struct fnu_rtctime *t);
int fnu_gettime(struct fnu_timespec *t, int which);
int fnu_settime(const struct fnu_timespec *now);
int fnu_slewtime(const struct fnu_timespec *delta, struct fnu_timespec *left);
int fnu_random(void *buf, size_t len);
int fnu_sysctl(const char *name, void *old, uint64_t *oldlen, const void *new,
               uint64_t newlen);
int fnu_sysctlnode(int index, struct fnu_sysctl_info *info);
int fnu_pledge(const char *promises, const char *execpromises);
int fnu_sendsyslog(const char *buf, size_t len, int flags);
int fnu_flock(int fd, int op);

const char *fnu_errstr(int err);

int fnu_volumes(struct fnu_fsstat **table);
int fnu_disks(struct fnu_diskinfo **table);
int fnu_processes(struct fnu_procinfo **table);
int fnu_interfaces(struct fnu_ifinfo **table);
void fnu_table_free(void *table);
int fnu_addr_parse(const char *text, uint8 addr[4]);
int fnu_waitread(int fd, int ms);
int fnu_resolve(const char *name, uint8 addr[4], int ms);

#endif
