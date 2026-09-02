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

// system calls
int fork(void);
STD_WRAP(void exit(void) __attribute__((noreturn)))
int wait(void);
int waitpid(pid_t pid, int flags);
#define WNOHANG 1
int pipe(int pipefd[2]);
ssize_t read(int fd, void *buf, size_t count);
STD_WRAP(int kill(pid_t pid))
// the last entry of argv should be NULL
int exec(char *path, char **argv);
int stat(int fd, struct stat *statbuf);
int stat_path(const char *path, struct stat *buf);
int chdir(const char *path);
int dup(int oldfd);
int getpid(void);
void *sbrk(intptr_t increment);
int sleep(int n);
int open(const char *pathname, int flags);
ssize_t write(int fd, const void *buf, size_t count);
STD_WRAP(int mknod(const char *pathname, int major, int minor))
int unlink(const char *pathname);
int link(const char *oldpath, const char *newpath);
STD_WRAP(int mkdir(const char *pathname))
int close(int fd);
void *malloc(size_t size);
void free(void *ptr);
int info(struct info *inf);
int reboot(void);
int procinfo(struct procinfo *pi, int capacity);
int ioctl(int fd, uint64_t cmd, uint64_t arg);
int getdents(int fd, struct linux_dirent64 *buf, int count);

// library
int printf(const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
STD_WRAP(char *gets(char *buf, int max))
void *malloc(size_t nbytes);
void free(void *ap);

#endif /* ifndef PRONINX_x86_64_USER_H */
