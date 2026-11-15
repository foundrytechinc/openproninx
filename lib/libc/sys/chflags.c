// chflags(2) family, BSD's own: the file flags word
#include <sys/stat.h>
#include <fcntl.h>
#include <fnu/sys.h>
#include <string.h>
#include "posix.h"

static int setflags(int fd, const char *path, unsigned int flags, int at) {
  struct fnu_setattr a;

  memset(&a, 0, sizeof(a));
  a.mask = FNU_SET_FLAGS;
  a.flags = flags;
  return (int)__posix(fnu_setattr(fd, path, at, &a));
}

int chflags(const char *path, unsigned int flags) {
  return setflags(FNU_FD_CWD, path, flags, 0);
}

int lchflags(const char *path, unsigned int flags) {
  return setflags(FNU_FD_CWD, path, flags, FNU_PATH_NOFOLLOW);
}

int chflagsat(int fd, const char *path, unsigned int flags, int atflags) {
  if ((atflags & ~AT_SYMLINK_NOFOLLOW) || (atflags = __at_flags(atflags)) < 0) {
    errno = EINVAL;
    return -1;
  }
  return setflags(__at_fd(fd), path, flags, atflags);
}

int fchflags(int fd, unsigned int flags) { return setflags(fd, 0, flags, 0); }
