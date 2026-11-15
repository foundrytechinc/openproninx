#ifndef _DIRENT_H_
#define _DIRENT_H_

#include <sys/types.h>

// libc turns the system's records into this
struct dirent {
  uint64_t d_fileno;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  uint8_t d_namlen;
  uint8_t __d_padding[4];
  char d_name[255 + 1];
};

#define MAXNAMLEN 255
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

#define _GENERIC_DIRLEN(namlen)                                                \
  ((__builtin_offsetof(struct dirent, d_name) + (namlen) + 1 + 7) & ~7)
#define _GENERIC_DIRSIZ(dp) _GENERIC_DIRLEN((dp)->d_namlen)

typedef struct _dirdesc {
  int dd_fd;
  long dd_loc;
  long dd_size;
  char *dd_buf;
  int dd_len;
  off_t dd_seek;
} DIR;

DIR *opendir(const char *filename);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *dirp);
void rewinddir(DIR *dirp);
int closedir(DIR *dirp);
int dirfd(DIR *dirp);
int scandir(const char *dirname, struct dirent ***namelist,
            int (*select)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);
int getdirentries(int fd, char *buf, int nbytes, int64_t *basep);

#endif
