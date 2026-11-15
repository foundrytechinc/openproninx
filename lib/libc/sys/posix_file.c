// POSIX over the system's own file calls
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/device.h>
#include <sys/filio.h>
#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

// open flags: access pair, then those with a native twin
// O_NOCTTY asks nothing
static const int oflag_map[][2] = {
    {O_NONBLOCK, FNU_OPEN_NOWAIT},     {O_APPEND, FNU_OPEN_APPEND},
    {O_CREAT, FNU_OPEN_CREATE},        {O_TRUNC, FNU_OPEN_TRUNCATE},
    {O_EXCL, FNU_OPEN_NEW},            {O_CLOEXEC, FNU_OPEN_CLOSEEXEC},
    {O_SYNC, FNU_OPEN_WRITETHROUGH},   {O_DIRECT, FNU_OPEN_UNCACHED},
    {O_NOFOLLOW, FNU_OPEN_NOLINK},     {O_DIRECTORY, FNU_OPEN_DIRONLY},
};

int __oflags_to_native(int o) {
  int n, i;

  switch (o & O_ACCMODE) {
  case O_RDONLY: n = FNU_OPEN_READ; break;
  case O_WRONLY: n = FNU_OPEN_WRITE; break;
  case O_RDWR: n = FNU_OPEN_READWRITE; break;
  default: return -1;
  }
  for (i = 0; i < (int)(sizeof(oflag_map) / sizeof(oflag_map[0])); i++)
    if (o & oflag_map[i][0])
      n |= oflag_map[i][1];
  return n;
}

int __oflags_of_native(int n) {
  int o, i;

  o = (n & FNU_OPEN_READWRITE) == FNU_OPEN_READWRITE ? O_RDWR
      : (n & FNU_OPEN_WRITE)                         ? O_WRONLY
                                                     : O_RDONLY;
  for (i = 0; i < (int)(sizeof(oflag_map) / sizeof(oflag_map[0])); i++)
    if (n & oflag_map[i][1])
      o |= oflag_map[i][0];
  return o;
}

int __at_fd(int fd) { return fd == AT_FDCWD ? FNU_FD_CWD : fd; }

// AT_SYMLINK_ to FNU_PATH_; -1 for a flag with no use here
int __at_flags(int f) {
  if (f & ~(AT_SYMLINK_NOFOLLOW | AT_SYMLINK_FOLLOW))
    return -1;
  return ((f & AT_SYMLINK_NOFOLLOW) ? FNU_PATH_NOFOLLOW : 0) |
         ((f & AT_SYMLINK_FOLLOW) ? FNU_PATH_FOLLOW : 0);
}

static int bad(int e) {
  errno = e;
  return -1;
}

// one segment, at the file's offset or the given one
static long rw1(int fd, void *buf, size_t n, int64_t at, int write) {
  struct fnu_segment seg;

  seg.base = buf;
  seg.len = n;
  return write ? fnu_write(fd, &seg, 1, at) : fnu_read(fd, &seg, 1, at);
}

ssize_t read(int fd, void *buf, size_t n) { return __posix(rw1(fd, buf, n, FNU_IO_HERE, 0)); }

ssize_t write(int fd, const void *buf, size_t n) {
  return __posix(rw1(fd, (void *)(uintptr_t)buf, n, FNU_IO_HERE, 1));
}

ssize_t pread(int fd, void *buf, size_t n, off_t offset) {
  if (offset < 0)
    return bad(EINVAL);
  return __posix(rw1(fd, buf, n, offset, 0));
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t offset) {
  if (offset < 0)
    return bad(EINVAL);
  return __posix(rw1(fd, (void *)(uintptr_t)buf, n, offset, 1));
}

// struct iovec is a segment
_Static_assert(sizeof(struct iovec) == sizeof(struct fnu_segment) &&
                   __builtin_offsetof(struct iovec, iov_base) ==
                       __builtin_offsetof(struct fnu_segment, base) &&
                   __builtin_offsetof(struct iovec, iov_len) ==
                       __builtin_offsetof(struct fnu_segment, len),
               "iovec");

ssize_t readv(int fd, const struct iovec *iov, int count) {
  return __posix(fnu_read(fd, (const struct fnu_segment *)iov, count, FNU_IO_HERE));
}

ssize_t writev(int fd, const struct iovec *iov, int count) {
  return __posix(fnu_write(fd, (const struct fnu_segment *)iov, count, FNU_IO_HERE));
}

ssize_t preadv(int fd, const struct iovec *iov, int count, off_t offset) {
  if (offset < 0)
    return bad(EINVAL);
  return __posix(fnu_read(fd, (const struct fnu_segment *)iov, count, offset));
}

ssize_t pwritev(int fd, const struct iovec *iov, int count, off_t offset) {
  if (offset < 0)
    return bad(EINVAL);
  return __posix(fnu_write(fd, (const struct fnu_segment *)iov, count, offset));
}

int openat(int fd, const char *path, int flags, ...) {
  va_list ap;
  int mode = 0;

  if (flags & O_CREAT) {
    va_start(ap, flags);
    mode = (int)va_arg(ap, int);
    va_end(ap);
  }
  if ((flags = __oflags_to_native(flags)) < 0)
    return bad(EINVAL);
  return (int)__posix(fnu_open(__at_fd(fd), path, flags, mode));
}

// the library's own file; pledge lets it by promise
int __pledge_open(const char *path, int flags) {
  if ((flags = __oflags_to_native(flags)) < 0)
    return bad(EINVAL);
  return (int)__posix(fnu_open(FNU_FD_CWD, path, flags | FNU_OPEN_SYSFILE, 0));
}

int open(const char *path, int flags, ...) {
  va_list ap;
  int mode = 0;

  if (flags & O_CREAT) {
    va_start(ap, flags);
    mode = (int)va_arg(ap, int);
    va_end(ap);
  }
  return openat(AT_FDCWD, path, flags, mode);
}

int close(int fd) { return (int)__posix(fnu_close(fd)); }

int pipe(int fds[2]) { return (int)__posix(fnu_pipe(fds, 0)); }

// the flags POSIX spells with open(2)'s names
static int fdflags_of(int oflags) {
  int f = 0;

  if (oflags & O_CLOEXEC)
    f |= FNU_FD_CLOSEEXEC;
  if (oflags & O_NONBLOCK)
    f |= FNU_FD_NOWAIT;
  return f;
}

int pipe2(int fds[2], int flags) {
  if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_pipe(fds, fdflags_of(flags)));
}

int dup(int fd) { return (int)__posix(fnu_dup(fd, 0, 0)); }

static int dup_exact(int fd, int fd2, int flags) {
  if (fd2 < 0)
    return bad(EBADF);
  return (int)__posix(fnu_dup(fd, fd2, FNU_DUP_EXACT | flags));
}

int dup2(int fd, int fd2) {
  struct fnu_fdstate st;

  if (fd == fd2)
    return fnu_fdstate_get(fd, &st) < 0 ? (errno = EBADF, -1) : fd2;
  return dup_exact(fd, fd2, 0);
}

int dup3(int fd, int fd2, int flags) {
  if (flags & ~O_CLOEXEC) {
    errno = EINVAL;
    return -1;
  }
  if (fd == fd2)
    return bad(EINVAL);
  return dup_exact(fd, fd2, fdflags_of(flags));
}

// fcntl was a multiplexer. here it is split
int fcntl(int fd, int cmd, ...) {
  struct fnu_fdstate st;
  va_list ap;
  long arg;

  va_start(ap, cmd);
  arg = (long)va_arg(ap, long);
  va_end(ap);

  switch (cmd) {
  case F_DUPFD:
    return (int)__posix(fnu_dup(fd, (int)arg, 0));
  case F_DUPFD_CLOEXEC:
    return (int)__posix(fnu_dup(fd, (int)arg, FNU_FD_CLOSEEXEC));
  case F_GETFD:
    if (__posix(fnu_fdstate_get(fd, &st)) < 0)
      return -1;
    return (st.fdflags & FNU_FD_CLOSEEXEC) ? FD_CLOEXEC : 0;
  case F_SETFD:
    if (__posix(fnu_fdstate_get(fd, &st)) < 0)
      return -1;
    st.fdflags = (arg & FD_CLOEXEC) ? FNU_FD_CLOSEEXEC : 0;
    return (int)__posix(fnu_fdstate_set(fd, &st));
  case F_GETFL:
    if (__posix(fnu_fdstate_get(fd, &st)) < 0)
      return -1;
    return __oflags_of_native((int)st.fileflags);
  case F_SETFL:
    if (__posix(fnu_fdstate_get(fd, &st)) < 0)
      return -1;
    st.fileflags = (uint32_t)__oflags_to_native(
        (int)((arg & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_EXCL | O_CLOEXEC | O_DIRECTORY)) |
              (__oflags_of_native((int)st.fileflags) & O_ACCMODE)));
    return (int)__posix(fnu_fdstate_set(fd, &st));
  default:
    errno = EINVAL;
    return -1;
  }
}

off_t lseek(int fd, off_t offset, int whence) {
  switch (whence) {
  case SEEK_SET: whence = FNU_SEEK_START; break;
  case SEEK_CUR: whence = FNU_SEEK_HERE; break;
  case SEEK_END: whence = FNU_SEEK_TAIL; break;
  default: return bad(EINVAL);
  }
  return __posix(fnu_seek(fd, offset, whence));
}

static int setsize(int fd, const char *path, off_t length) {
  struct fnu_setattr a;

  if (length < 0)
    return bad(EINVAL);
  memset(&a, 0, sizeof(a));
  a.mask = FNU_SET_SIZE;
  a.size = (uint64_t)length;
  return (int)__posix(fnu_setattr(fd, path, 0, &a));
}

int ftruncate(int fd, off_t length) { return setsize(fd, 0, length); }

int truncate(const char *path, off_t length) {
  return setsize(FNU_FD_CWD, path, length);
}

int fsync(int fd) {
  if (fd == FNU_FLUSH_ALL)
    return bad(EBADF);
  return (int)__posix(fnu_flush(fd));
}

// the data and the volume go out together here
int fdatasync(int fd) { return fsync(fd); }

// FIODTYPE: the device class fstat reports, devfs D_* bits
static int fiodtype(int fd, int *type) {
  struct fnu_stat st;

  if (__posix(fnu_getattr(fd, 0, 0, &st)) < 0)
    return -1;
  if (st.type != FNU_TYPE_DEVICE && st.type != FNU_TYPE_DISK) {
    errno = ENOTTY;
    return -1;
  }
  switch (st.type == FNU_TYPE_DISK ? FNU_DEVCLASS_DISK : st.devclass) {
  case FNU_DEVCLASS_DISK:
    *type = D_DISK;
    break;
  case FNU_DEVCLASS_TERMINAL:
    *type = D_TTY;
    break;
  case FNU_DEVCLASS_MEMORY:
    *type = D_MEM;
    break;
  default:
    *type = 0;
  }
  return 0;
}

int ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  uint64_t arg;
  int r;

  va_start(ap, request);
  arg = (uint64_t)va_arg(ap, uint64_t);
  va_end(ap);
  if (request == FIODTYPE)
    return fiodtype(fd, (int *)arg);
  if ((r = __tty_ioctl(fd, request, (void *)(uintptr_t)arg)) != -2)
    return r;
  return (int)__posix(fnu_ioctl(fd, request, arg));
}

static int access_mode(int m) {
  if (m & ~(R_OK | W_OK | X_OK))
    return -1;
  return ((m & R_OK) ? FNU_ACCESS_READ : 0) |
         ((m & W_OK) ? FNU_ACCESS_WRITE : 0) | ((m & X_OK) ? FNU_ACCESS_EXEC : 0);
}

// real ids unless AT_EACCESS wants the effective ones
int faccessat(int fd, const char *path, int mode, int flags) {
  if ((mode = access_mode(mode)) < 0 || (flags & ~AT_EACCESS))
    return bad(EINVAL);
  return (int)__posix(fnu_access(__at_fd(fd), path, mode,
                                 (flags & AT_EACCESS) ? 0 : FNU_ACCESS_REAL));
}

int access(const char *path, int mode) {
  return faccessat(AT_FDCWD, path, mode, 0);
}

int eaccess(const char *path, int mode) {
  return faccessat(AT_FDCWD, path, mode, AT_EACCESS);
}

int unlinkat(int fd, const char *path, int flags) {
  if (flags & ~AT_REMOVEDIR)
    return bad(EINVAL);
  return (int)__posix(fnu_remove(__at_fd(fd), path,
                                 (flags & AT_REMOVEDIR) ? FNU_REMOVE_DIR : 0));
}

int unlink(const char *path) { return unlinkat(AT_FDCWD, path, 0); }

int rmdir(const char *path) { return unlinkat(AT_FDCWD, path, AT_REMOVEDIR); }

int linkat(int fromfd, const char *from, int tofd, const char *to, int flags) {
  if ((flags & ~AT_SYMLINK_FOLLOW) || (flags = __at_flags(flags)) < 0)
    return bad(EINVAL);
  return (int)__posix(
      fnu_link(__at_fd(fromfd), from, __at_fd(tofd), to, flags));
}

int link(const char *from, const char *to) {
  return linkat(AT_FDCWD, from, AT_FDCWD, to, AT_SYMLINK_FOLLOW);
}

int symlinkat(const char *from, int fd, const char *to) {
  return (int)__posix(fnu_make(__at_fd(fd), to, FNU_TYPE_LINK, 0, from));
}

int symlink(const char *from, const char *to) {
  return symlinkat(from, AT_FDCWD, to);
}

ssize_t readlinkat(int fd, const char *path, char *buf, size_t size) {
  if (size > INT_MAX)
    size = INT_MAX;
  return __posix(fnu_readlink(__at_fd(fd), path, buf, (int)size));
}

ssize_t readlink(const char *path, char *buf, size_t size) {
  return readlinkat(AT_FDCWD, path, buf, size);
}

int renameat(int fromfd, const char *from, int tofd, const char *to) {
  return (int)__posix(fnu_rename(__at_fd(fromfd), from, __at_fd(tofd), to));
}

int rename(const char *from, const char *to) {
  return renameat(AT_FDCWD, from, AT_FDCWD, to);
}

int mkdirat(int fd, const char *path, mode_t mode) {
  return (int)__posix(fnu_make(__at_fd(fd), path, FNU_TYPE_DIR, mode & 07777, 0));
}

int mkdir(const char *path, mode_t mode) { return mkdirat(AT_FDCWD, path, mode); }

int mkfifoat(int fd, const char *path, mode_t mode) {
  return (int)__posix(fnu_make(__at_fd(fd), path, FNU_TYPE_FIFO, mode & 07777, 0));
}

int mkfifo(const char *path, mode_t mode) { return mkfifoat(AT_FDCWD, path, mode); }

// a fifo only: a device is made by its driver, in /dev
int mknodat(int fd, const char *path, mode_t mode, dev_t dev __unused) {
  if (S_ISFIFO(mode))
    return mkfifoat(fd, path, mode);
  return bad(S_ISCHR(mode) || S_ISBLK(mode) ? EOPNOTSUPP : EINVAL);
}

int mknod(const char *path, mode_t mode, dev_t dev) {
  return mknodat(AT_FDCWD, path, mode, dev);
}

long pathconfat(int fd, const char *path, int name, int flags) {
  if ((flags = __at_flags(flags)) < 0)
    return bad(EINVAL);
  return (long)__posix((long)fnu_pathconf(__at_fd(fd), path, flags, name));
}

long pathconf(const char *path, int name) {
  return pathconfat(AT_FDCWD, path, name, 0);
}

long fpathconf(int fd, int name) {
  return (long)__posix((long)fnu_pathconf(fd, 0, 0, name));
}

int chdir(const char *path) { return (int)__posix(fnu_chdir(FNU_FD_CWD, path)); }

int fchdir(int fd) { return (int)__posix(fnu_chdir(fd, 0)); }

static int setperm(int fd, const char *path, mode_t mode, int flags) {
  struct fnu_setattr a;

  memset(&a, 0, sizeof(a));
  a.mask = FNU_SET_MODE;
  a.mode = mode & 07777;
  return (int)__posix(fnu_setattr(fd, path, flags, &a));
}

int fchmodat(int fd, const char *path, mode_t mode, int flags) {
  if ((flags & ~AT_SYMLINK_NOFOLLOW) || (flags = __at_flags(flags)) < 0)
    return bad(EINVAL);
  return setperm(__at_fd(fd), path, mode, flags);
}

int chmod(const char *path, mode_t mode) {
  return setperm(FNU_FD_CWD, path, mode, 0);
}

int fchmod(int fd, mode_t mode) { return setperm(fd, 0, mode, 0); }

// (uid_t)-1 and (gid_t)-1 leave that one as it is
static int setowner(int fd, const char *path, uid_t uid, gid_t gid, int flags) {
  struct fnu_setattr a;

  memset(&a, 0, sizeof(a));
  if (uid != (uid_t)-1) {
    a.mask |= FNU_SET_UID;
    a.uid = uid;
  }
  if (gid != (gid_t)-1) {
    a.mask |= FNU_SET_GID;
    a.gid = gid;
  }
  return (int)__posix(fnu_setattr(fd, path, flags, &a));
}

int fchownat(int fd, const char *path, uid_t uid, gid_t gid, int flags) {
  if ((flags & ~AT_SYMLINK_NOFOLLOW) || (flags = __at_flags(flags)) < 0)
    return bad(EINVAL);
  return setowner(__at_fd(fd), path, uid, gid, flags);
}

int chown(const char *path, uid_t uid, gid_t gid) {
  return setowner(FNU_FD_CWD, path, uid, gid, 0);
}

int lchown(const char *path, uid_t uid, gid_t gid) {
  return setowner(FNU_FD_CWD, path, uid, gid, FNU_PATH_NOFOLLOW);
}

int fchown(int fd, uid_t uid, gid_t gid) { return setowner(fd, 0, uid, gid, 0); }

// records turned in place into struct dirent; the heads are the same length
_Static_assert(__builtin_offsetof(struct fnu_dirent, name) ==
                   __builtin_offsetof(struct dirent, d_name),
               "dirent heads");

static unsigned char dt_of(int type) {
  switch (type) {
  case FNU_TYPE_FILE: return DT_REG;
  case FNU_TYPE_DIR: return DT_DIR;
  case FNU_TYPE_LINK: return DT_LNK;
  case FNU_TYPE_FIFO: return DT_FIFO;
  case FNU_TYPE_SOCKET: return DT_SOCK;
  case FNU_TYPE_DEVICE: return DT_CHR;
  case FNU_TYPE_DISK: return DT_BLK;
  default: return DT_UNKNOWN;
  }
}

int getdirentries(int fd, char *buf, int size, off_t *basep) {
  struct fnu_dirent n;
  struct dirent *d;
  int got, at;

  if ((got = (int)__posix(fnu_readdir(fd, buf, size, basep))) <= 0)
    return got;
  for (at = 0; at + (int)__builtin_offsetof(struct fnu_dirent, name) <= got;
       at += n.reclen) {
    memcpy(&n, buf + at, __builtin_offsetof(struct fnu_dirent, name));
    if (n.reclen == 0 || n.namelen > 255)
      break;
    d = (struct dirent *)(buf + at);
    d->d_fileno = n.ino;
    d->d_off = n.next;
    d->d_reclen = n.reclen;
    d->d_type = dt_of(n.type);
    d->d_namlen = (uint8_t)n.namelen;
    memset(d->__d_padding, 0, sizeof(d->__d_padding));
  }
  return got;
}

mode_t umask(mode_t mask) { return (mode_t)fnu_createmask(mask); }

int flock(int fd, int op) { return (int)__posix(fnu_flock(fd, op)); }
