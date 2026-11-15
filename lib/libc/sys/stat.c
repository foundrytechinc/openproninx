// stat family: fnu_stat into struct stat
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

static void to_posix(const struct fnu_stat *k, struct stat *st) {
  mode_t type;

  memset(st, 0, sizeof(*st));
  switch (k->type) {
  case FNU_TYPE_DIR: type = S_IFDIR; break;
  case FNU_TYPE_FILE: type = S_IFREG; break;
  case FNU_TYPE_DEVICE: type = S_IFCHR; break;
  case FNU_TYPE_DISK: type = S_IFBLK; break;
  case FNU_TYPE_LINK: type = S_IFLNK; break;
  case FNU_TYPE_FIFO: type = S_IFIFO; break;
  case FNU_TYPE_SOCKET: type = S_IFSOCK; break;
  default: type = 0; break;
  }
  st->st_dev = (dev_t)k->device;
  st->st_ino = k->ino;
  st->st_mode = type | (k->mode & 07777);
  st->st_nlink = k->nlink;
  st->st_uid = k->uid;
  st->st_gid = k->gid;
  st->st_rdev = k->type == FNU_TYPE_DEVICE || k->type == FNU_TYPE_DISK
                    ? makedev((uint)k->major, (uint)k->minor)
                    : 0;
  st->st_atim.tv_sec = k->atime.sec;
  st->st_atim.tv_nsec = k->atime.nsec;
  st->st_mtim.tv_sec = k->mtime.sec;
  st->st_mtim.tv_nsec = k->mtime.nsec;
  st->st_ctim.tv_sec = k->ctime.sec;
  st->st_ctim.tv_nsec = k->ctime.nsec;
  st->__st_birthtim.tv_sec = k->birthtime.sec;
  st->__st_birthtim.tv_nsec = k->birthtime.nsec;
  st->st_gen = k->gen;
  st->st_size = (off_t)k->size;
  st->st_blocks = k->blocks;
  st->st_flags = k->flags;
  st->st_blksize = k->blksize;
}

int fstat(int fd, struct stat *st) {
  struct fnu_stat k;

  if (__posix(fnu_getattr(fd, 0, 0, &k)) < 0)
    return -1;
  to_posix(&k, st);
  return 0;
}

int fstatat(int fd, const char *path, struct stat *st, int flags) {
  struct fnu_stat k;

  if ((flags & ~AT_SYMLINK_NOFOLLOW) || (flags = __at_flags(flags)) < 0) {
    errno = EINVAL;
    return -1;
  }
  if (__posix(fnu_getattr(__at_fd(fd), path, flags, &k)) < 0)
    return -1;
  to_posix(&k, st);
  return 0;
}

int lstat(const char *path, struct stat *st) {
  return fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int stat(const char *path, struct stat *st) {
  return fstatat(AT_FDCWD, path, st, 0);
}
