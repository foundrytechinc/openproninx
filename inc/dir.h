#ifndef PRONINX_X86_64_DIR_H
#define PRONINX_X86_64_DIR_H

#include "inc/types.h"

#define DIRSIZ 14 // fs.img fixed-width name
#define MAXNAMLEN 255 // vfs pathname buffer
#define NAMEBUFSZ (MAXNAMLEN + 1)
#define MAXPATHLEN 512

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

#endif /* ifndef PRONINX_X86_64_DIR_H */
