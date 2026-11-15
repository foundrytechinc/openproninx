#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

char *getcwd(char *buf, size_t size) {
  if (size == 0) {
    errno = EINVAL;
    return 0;
  }
  if (__posix(fnu_pathof(FNU_FD_CWD, 0, buf, size > INT_MAX ? INT_MAX : (int)size)) < 0)
    return 0;
  return buf;
}

int creat(const char *path, mode_t mode) {
  return open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

pid_t getpgrp(void) { return getpgid(0); }

pid_t tcgetpgrp(int fd) {
  pid_t pgrp;

  if (ioctl(fd, TIOCGPGRP, (uint64_t)&pgrp) < 0)
    return -1;
  return pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp) {
  return ioctl(fd, TIOCSPGRP, (uint64_t)&pgrp);
}

void sync(void) { fnu_flush(FNU_FLUSH_ALL); }

int mount(const char *type, const char *dir, int flags, void *data) {
  struct fnu_mount_req req;

  memset(&req, 0, sizeof(req));
  strlcpy(req.type, type, sizeof(req.type));
  strlcpy(req.on, dir, sizeof(req.on));
  if (data != 0)
    strlcpy(req.from, data, sizeof(req.from));
  req.flags = (uint32_t)flags;
  return (int)__posix(fnu_mount(&req));
}

int unmount(const char *dir, int flags) {
  return (int)__posix(fnu_unmount(dir, (unsigned)flags));
}

// BSD howto to the kernel's
int reboot(int howto) {
  int fnu = FNU_HALT_REBOOT;

  if (howto & RB_POWEROFF)
    fnu = FNU_HALT_POWEROFF;
  else if (howto & RB_HALT)
    fnu = FNU_HALT_STOP;
  return (int)__posix(fnu_halt(fnu | FNU_HALT_FORCE));
}

int madvise(void *addr, size_t len, int advice) {
  static const int native[] = {
      [MADV_NORMAL] = FNU_MEMUSE_NORMAL,     [MADV_RANDOM] = FNU_MEMUSE_RANDOM,
      [MADV_SEQUENTIAL] = FNU_MEMUSE_SEQUENTIAL,
      [MADV_WILLNEED] = FNU_MEMUSE_WILLNEED, [MADV_DONTNEED] = FNU_MEMUSE_DONTNEED,
      [MADV_FREE] = FNU_MEMUSE_FREE,
  };

  if (advice < 0 || advice >= (int)(sizeof(native) / sizeof(native[0])) ||
      (advice != MADV_NORMAL && native[advice] == 0)) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_memuse(addr, len, native[advice]));
}

// the error comes back, errno is left alone
int posix_madvise(void *addr, size_t len, int advice) {
  int saved = errno, r;

  if ((r = madvise(addr, len, advice)) < 0)
    r = errno;
  errno = saved;
  return r;
}
