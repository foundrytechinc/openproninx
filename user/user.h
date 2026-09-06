#ifndef PRONINX_x86_64_USER_H
#define PRONINX_x86_64_USER_H

#define NULL 0

#include "inc/dir.h"
#include "inc/fcntl.h"
#include "inc/stat.h"
#include "inc/stdarg.h"
#include "inc/string.h"
#include "inc/types.h"
#include "inc/abi.h"

// wrap functions which exist in stds, but have different signatures.
#define STD_WRAP(f)                                                            \
  _Pragma("GCC diagnostic push")                                               \
      _Pragma("GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\"") f;  \
  _Pragma("GCC diagnostic pop")

// ============================================================================
// STABLE CORE ABI syscalls  (frozen -- guaranteed backward compatibility)
// ============================================================================

// Process Management
int fork(void);
STD_WRAP(void exit(void) __attribute__((noreturn)))
int wait(void);
int waitpid(pid_t pid, int flags);
#define WNOHANG 1
STD_WRAP(int kill(pid_t pid))
// the last entry of argv should be NULL
int exec(char *path, char **argv);
int getpid(void);
int sleep(int n);

// File System & I/O
int pipe(int pipefd[2]);
ssize_t read(int fd, void *buf, size_t count);
int open(const char *pathname, int flags);
int close(int fd);
int stat(int fd, struct stat *statbuf);
int stat_path(const char *path, struct stat *buf);
ssize_t write(int fd, const void *buf, size_t count);
int dup(int oldfd);
STD_WRAP(int mknod(const char *pathname, int major, int minor))
int unlink(const char *pathname);
int link(const char *oldpath, const char *newpath);
STD_WRAP(int mkdir(const char *pathname))
int chdir(const char *path);
int getdents(int fd, struct linux_dirent64 *buf, int count);
int ioctl(int fd, uint64_t cmd, uint64_t arg);
int chmod(const char *path, int mode);
int chown(const char *path, int uid, int gid);

// Process Security & Terminal Control
int getuid(void);
int setuid(int);
int setforeground(pid_t pid);

// Memory Management
void *sbrk(intptr_t increment);
void *malloc(size_t size);
void free(void *ptr);

// System Information & Machine Control
int info(struct info *inf);
int reboot(void);
int poweroff(void);
int procinfo(struct procinfo *pi, int capacity);

// Network Subsystem (lwIP slot-handle API)
int ping(struct network_ping_request *request);
int udp_open(void); int udp_bind(int, int); int udp_sendto(int, const void *, int, const struct network_endpoint *); int udp_recvfrom(int, void *, int, struct network_endpoint *, int); int udp_close(int);
int netinfo(struct network_status *);
int netconfig(struct network_ipv4_config *);
int tcp_open(void); int tcp_bind(int, int); int tcp_listen(int); int tcp_accept(int, int); int tcp_connect(int, const struct network_endpoint *, int); int tcp_send(int, const void *, int); int tcp_recv(int, void *, int, int); int tcp_close(int);
int dns_resolve(struct network_dns_request *);

// System Time
int rtctime(struct rtctime *t);

// Storage Information
int df(struct df_stat *buf, int capacity);

// ============================================================================
// EXPERIMENTAL / NON-ABI syscalls  (may change without notice)
// ============================================================================

// In-Kernel Authentication (pending migration to userspace)
int login(const char *, const char *);
int doas_auth(const char *);
int useradd(const char *, const char *, int);
int passwd(const char *, const char *);
int users(struct user_info *, int);

// ============================================================================
// Library functions
// ============================================================================
int printf(const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int putchar(int c);
STD_WRAP(char *gets(char *buf, int max))
int read_password(char *buf, int max);

#endif /* ifndef PRONINX_x86_64_USER_H */
