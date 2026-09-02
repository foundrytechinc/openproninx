#include "user.h"

int stat_path(const char *path, struct stat *st) {
  int fd;
  int r;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  r = stat(fd, st);
  close(fd);
  return r;
}
