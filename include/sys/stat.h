#ifndef _SYS_STAT_H_
#define _SYS_STAT_H_

#include <sys/types.h>
#include <time.h>
#include "inc/stat.h"

// the mode word as POSIX lays it out: the type in the high bits
#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001

#define S_ISTXT S_ISVTX

struct stat {
  dev_t st_dev;
  ino_t st_ino;
  mode_t st_mode;
  nlink_t st_nlink;
  uid_t st_uid;
  gid_t st_gid;
  dev_t st_rdev;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
  off_t st_size;
  blkcnt_t st_blocks;
  blksize_t st_blksize;
  uint32_t st_flags;
  uint32_t st_gen;
  struct timespec __st_birthtim;
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
#define __st_birthtime __st_birthtim.tv_sec

#define S_BLKSIZE 512 // the unit of st_blocks
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#define ACCESSPERMS 0777
#define ALLPERMS 07777
#define DEFFILEMODE 0666

#define UTIME_NOW -2L
#define UTIME_OMIT -1L

#define UF_SETTABLE FNU_UF_SETTABLE
#define UF_NODUMP FNU_UF_NODUMP
#define UF_IMMUTABLE FNU_UF_IMMUTABLE
#define UF_APPEND FNU_UF_APPEND
#define UF_OPAQUE FNU_UF_OPAQUE
#define SF_SETTABLE FNU_SF_SETTABLE
#define SF_ARCHIVED FNU_SF_ARCHIVED
#define SF_IMMUTABLE FNU_SF_IMMUTABLE
#define SF_APPEND FNU_SF_APPEND

int stat(const char *path, struct stat *sb);
int fstatat(int fd, const char *path, struct stat *sb, int flags);
int mkfifo(const char *path, mode_t mode);
int mkfifoat(int fd, const char *path, mode_t mode);
int fchmodat(int fd, const char *path, mode_t mode, int flags);
int utimensat(int fd, const char *path, const struct timespec times[2],
              int flags);
int futimens(int fd, const struct timespec times[2]);
int fstat(int fd, struct stat *sb);
int lstat(const char *path, struct stat *sb);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
int chflags(const char *path, unsigned int flags);
int lchflags(const char *path, unsigned int flags);
int chflagsat(int fd, const char *path, unsigned int flags, int atflags);
int fchflags(int fd, unsigned int flags);
int mkdir(const char *path, mode_t mode);
int mkdirat(int fd, const char *path, mode_t mode);
int mknod(const char *path, mode_t mode, dev_t dev);
int mknodat(int fd, const char *path, mode_t mode, dev_t dev);
mode_t umask(mode_t numask);

#endif
