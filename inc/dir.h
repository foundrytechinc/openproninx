#ifndef PRONINX_X86_64_DIR_H
#define PRONINX_X86_64_DIR_H

#include "inc/types.h"

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14
// Fixed-width legacy fs.img directory entries remain 14 bytes. Variable-size
// directory formats use this separate VFS pathname buffer.
#define MAXNAMLEN 255
#define NAMEBUFSZ (MAXNAMLEN + 1)

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

#endif /* ifndef PRONINX_X86_64_DIR_H */
