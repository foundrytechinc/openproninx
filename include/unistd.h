#ifndef _UNISTD_H_
#define _UNISTD_H_

#include <sys/types.h>
#include "inc/abi.h"
#include "inc/cdefs.h"

// lseek's whence, which POSIX has unistd.h say as well
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern char **environ;
#ifndef _GETOPT_DEFINED_
#define _GETOPT_DEFINED_
int getopt(int argc, char *const argv[], const char *optstring);
extern char *optarg;
extern int optind, opterr, optopt, optreset;
#endif

ssize_t read(int fd, void *buf, size_t nbytes);
ssize_t write(int fd, const void *buf, size_t nbytes);
int close(int fd);
int dup(int fd);
int dup2(int fd, int fd2);
int getgrouplist(const char *, gid_t, gid_t *, int *);
int dup3(int fd, int fd2, int flags);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
off_t lseek(int fd, off_t offset, int whence);
int ftruncate(int fd, off_t length);
int truncate(const char *path, off_t length);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);
int isatty(int fd);
int ttyname_r(int fd, char *buf, size_t len);
long sysconf(int name);

// sysconf(3) names, as sys/unistd.h numbers them
#define _SC_ARG_MAX   1
#define _SC_CHILD_MAX   2
#define _SC_CLK_TCK   3
#define _SC_NGROUPS_MAX   4
#define _SC_OPEN_MAX   5
#define _SC_JOB_CONTROL   6
#define _SC_SAVED_IDS   7
#define _SC_VERSION   8
#define _SC_BC_BASE_MAX   9
#define _SC_BC_DIM_MAX  10
#define _SC_BC_SCALE_MAX 11
#define _SC_BC_STRING_MAX 12
#define _SC_COLL_WEIGHTS_MAX 13
#define _SC_EXPR_NEST_MAX 14
#define _SC_LINE_MAX  15
#define _SC_RE_DUP_MAX  16
#define _SC_2_VERSION  17
#define _SC_2_C_BIND  18
#define _SC_2_C_DEV  19
#define _SC_2_CHAR_TERM  20
#define _SC_2_FORT_DEV  21
#define _SC_2_FORT_RUN  22
#define _SC_2_LOCALEDEF  23
#define _SC_2_SW_DEV  24
#define _SC_2_UPE  25
#define _SC_STREAM_MAX  26
#define _SC_TZNAME_MAX  27
#define _SC_PAGESIZE  28
#define _SC_PAGE_SIZE  _SC_PAGESIZE /* 1170 compatibility */
#define _SC_FSYNC  29
#define _SC_XOPEN_SHM  30
#define _SC_SEM_NSEMS_MAX 31
#define _SC_SEM_VALUE_MAX 32
#define _SC_NPROCESSORS_CONF 502
#define _SC_NPROCESSORS_ONLN 503

int getpagesize(void);
int issetugid(void);
int pledge(const char *promises, const char *execpromises);
int unveil(const char *path, const char *permissions);
int eaccess(const char *path, int mode);
ssize_t pread(int fd, void *buf, size_t nbytes, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t nbytes, off_t offset);
int gethostname(char *name, size_t namelen);
int sethostname(const char *name, size_t namelen);
char *getusershell(void);
void *setmode(const char *);
mode_t getmode(const void *, mode_t);
void setusershell(void);
void endusershell(void);
char *ttyname(int fd);

pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
void _exit(int status) __dead;
pid_t getpid(void);
pid_t getppid(void);
pid_t setsid(void);
char *getlogin(void);
char *fflagstostr(u_int32_t flags);
int strtofflags(char **stringp, u_int32_t *setp, u_int32_t *clrp);
int getlogin_r(char *, size_t);
int setlogin(const char *);
pid_t vfork(void);
int setpgid(pid_t pid, pid_t pgrp);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
pid_t getsid(pid_t pid);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);

uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t euid);
int setegid(gid_t egid);
int setreuid(uid_t ruid, uid_t euid);
int setregid(gid_t rgid, gid_t egid);
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
int getgroups(int gidsetlen, gid_t *gidset);
int setgroups(int ngroups, const gid_t *gidset);

int chdir(const char *path);
int fchdir(int fd);
char *getcwd(char *buf, size_t size);
#ifndef F_OK
#define F_OK 0
#define X_OK 0x01
#define W_OK 0x02
#define R_OK 0x04
#endif

int access(const char *path, int mode);
int faccessat(int fd, const char *path, int mode, int flags);
int unlink(const char *path);
int unlinkat(int fd, const char *path, int flags);
int rmdir(const char *path);
/* what pathconf(2) asks about */
#define _PC_LINK_MAX FNU_PC_LINK_MAX
#define _PC_NAME_MAX FNU_PC_NAME_MAX
#define _PC_PATH_MAX FNU_PC_PATH_MAX
#define _PC_PIPE_BUF FNU_PC_PIPE_BUF
#define _PC_CHOWN_RESTRICTED FNU_PC_CHOWN_RESTRICTED
#define _PC_NO_TRUNC FNU_PC_NO_TRUNC
#define _PC_2_SYMLINKS FNU_PC_2_SYMLINKS
#define _PC_ALLOC_SIZE_MIN FNU_PC_ALLOC_SIZE_MIN
#define _PC_FILESIZEBITS FNU_PC_FILESIZEBITS
#define _PC_SYMLINK_MAX FNU_PC_SYMLINK_MAX
#define _PC_TIMESTAMP_RESOLUTION FNU_PC_TIMESTAMP_RESOLUTION

long pathconf(const char *path, int name);
long pathconfat(int fd, const char *path, int name, int flags);
long fpathconf(int fd, int name);

int link(const char *name1, const char *name2);
int linkat(int fd1, const char *name1, int fd2, const char *name2, int flags);
int symlink(const char *name1, const char *name2);
int symlinkat(const char *name1, int fd, const char *name2);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
ssize_t readlinkat(int fd, const char *path, char *buf, size_t bufsiz);
int chown(const char *path, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int fchownat(int fd, const char *path, uid_t owner, gid_t group, int flags);
int fchown(int fd, uid_t owner, gid_t group);

unsigned int sleep(unsigned int seconds);
int usleep(useconds_t microseconds);
unsigned int alarm(unsigned int seconds);
int pause(void);
int nice(int incr);
int getentropy(void *buf, size_t buflen);

#endif
