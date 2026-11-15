#ifndef _FCNTL_H_
#define _FCNTL_H_

#include <sys/types.h>

// POSIX flag numbers; libc maps them to FNU_OPEN_
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE 0x0003
#define O_NONBLOCK 0x0004
#define O_APPEND 0x0008
#define O_SYNC 0x0080
#define O_FSYNC O_SYNC
#define O_DSYNC O_SYNC
#define O_RSYNC O_SYNC
#define O_NOFOLLOW 0x0100
#define O_CREAT 0x0200
#define O_TRUNC 0x0400
#define O_EXCL 0x0800
#define O_NOCTTY 0x8000
#define O_CLOEXEC 0x10000
#define O_DIRECTORY 0x20000
#define O_DIRECT 0x40000
// no verifier here, so the flag asks nothing
#define O_VERIFY 0x200000

// no fcntl(2) here; libc splits these into native calls
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 10

#define FD_CLOEXEC 1

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifndef F_OK
#define F_OK 0
#define X_OK 0x01
#define W_OK 0x02
#define R_OK 0x04
#endif

#define AT_FDCWD -100
#define AT_EACCESS 0x01
#define AT_SYMLINK_NOFOLLOW 0x02
#define AT_SYMLINK_FOLLOW 0x04
#define AT_REMOVEDIR 0x08

int open(const char *path, int flags, ...);
int openat(int fd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif
