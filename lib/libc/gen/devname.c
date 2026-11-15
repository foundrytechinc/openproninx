// devname(3), no database
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

char *devname(dev_t dev, mode_t type) {
  static char buf[NAME_MAX + 1];
  char *name = NULL;
  struct dirent *dp;
  struct stat sb;
  DIR *dirp;

  if ((dirp = opendir("/dev")) == NULL)
    return "??";
  while ((dp = readdir(dirp)) != NULL) {
    if (fstatat(dirfd(dirp), dp->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0 ||
        sb.st_rdev != dev || (sb.st_mode & S_IFMT) != type)
      continue;
    strlcpy(buf, dp->d_name, sizeof(buf));
    name = buf;
    break;
  }
  closedir(dirp);
  return name != NULL ? name : "??";
}
