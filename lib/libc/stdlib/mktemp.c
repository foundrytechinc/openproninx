// the X's become random letters; made only if it was not there
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRIES 0x7fff
#define MIN_X 6

static const char letters[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

enum { NAME_ONLY, MAKE_FILE, MAKE_DIR };

static int temp(char *path, int slen, int what, int flags) {
  size_t len = strlen(path);
  char *start, *end;
  int tries, fd;
  struct stat st;

  if (slen < 0 || (size_t)slen > len ||
      (flags & ~(O_APPEND | O_CLOEXEC | O_SYNC | O_DSYNC | O_RSYNC)) != 0) {
    errno = EINVAL;
    return -1;
  }
  end = path + len - slen;
  for (start = end; start > path && start[-1] == 'X'; start--)
    ;
  if (end - start < MIN_X) {
    errno = EINVAL;
    return -1;
  }
  for (tries = 0; tries < TRIES; tries++) {
    char *p;

    for (p = start; p < end; p++)
      *p = letters[arc4random_uniform(sizeof(letters) - 1)];
    switch (what) {
    case NAME_ONLY:
      if (lstat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
      break;
    case MAKE_FILE:
      if ((fd = open(path, O_CREAT | O_EXCL | O_RDWR | flags, 0600)) >= 0)
        return fd;
      if (errno != EEXIST)
        return -1;
      break;
    case MAKE_DIR:
      if (mkdir(path, 0700) == 0)
        return 0;
      if (errno != EEXIST)
        return -1;
      break;
    }
  }
  errno = EEXIST;
  return -1;
}

char *mktemp(char *path) { return temp(path, 0, NAME_ONLY, 0) < 0 ? 0 : path; }

int mkstemp(char *path) { return temp(path, 0, MAKE_FILE, 0); }

int mkostemp(char *path, int flags) { return temp(path, 0, MAKE_FILE, flags); }

int mkstemps(char *path, int slen) { return temp(path, slen, MAKE_FILE, 0); }

int mkostemps(char *path, int slen, int flags) {
  return temp(path, slen, MAKE_FILE, flags);
}

char *mkdtemp(char *path) { return temp(path, 0, MAKE_DIR, 0) < 0 ? 0 : path; }

char *mkdtemps(char *path, int slen) {
  return temp(path, slen, MAKE_DIR, 0) < 0 ? 0 : path;
}
