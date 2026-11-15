#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DIRBLKSIZ 1024

DIR *fdopendir(int fd) {
  DIR *dirp;

  if ((dirp = malloc(sizeof(DIR))) == 0)
    return 0;
  if ((dirp->dd_buf = malloc(DIRBLKSIZ)) == 0) {
    free(dirp);
    return 0;
  }
  dirp->dd_fd = fd;
  dirp->dd_len = DIRBLKSIZ;
  dirp->dd_loc = dirp->dd_size = 0;
  dirp->dd_seek = 0;
  return dirp;
}

DIR *opendir(const char *name) {
  DIR *dirp;
  int fd;

  if ((fd = open(name, O_RDONLY | O_CLOEXEC)) < 0)
    return 0;
  if ((dirp = fdopendir(fd)) == 0) {
    close(fd);
    return 0;
  }
  return dirp;
}

struct dirent *readdir(DIR *dirp) {
  struct dirent *dp;

  for (;;) {
    if (dirp->dd_loc >= dirp->dd_size)
      dirp->dd_loc = 0;
    if (dirp->dd_loc == 0) {
      int64_t base;

      dirp->dd_size = getdirentries(dirp->dd_fd, dirp->dd_buf, dirp->dd_len, &base);
      if (dirp->dd_size <= 0)
        return 0;
      dirp->dd_seek = base;
    }
    dp = (struct dirent *)(dirp->dd_buf + dirp->dd_loc);
    if (dp->d_reclen < _GENERIC_DIRLEN(0) ||
        dp->d_reclen > dirp->dd_size - dirp->dd_loc)
      return 0;
    dirp->dd_loc += dp->d_reclen;
    if (dp->d_fileno == 0)
      continue;
    return dp;
  }
}

void rewinddir(DIR *dirp) {
  lseek(dirp->dd_fd, 0, SEEK_SET);
  dirp->dd_loc = dirp->dd_size = 0;
  dirp->dd_seek = 0;
}

int closedir(DIR *dirp) {
  int fd = dirp->dd_fd;

  free(dirp->dd_buf);
  free(dirp);
  return close(fd);
}

int dirfd(DIR *dirp) { return dirp->dd_fd; }
