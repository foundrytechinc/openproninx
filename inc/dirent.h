#ifndef PRONINX_DIRENT_H
#define PRONINX_DIRENT_H

#include "inc/types.h"
#include "inc/syslimits.h"

struct fnu_dirent {
  uint64 ino;
  int64 next;
  uint16 reclen;
  uint8 type;
  uint8 pad0;
  uint16 namelen;
  uint16 pad1;
  char name[FNU_NAMELEN_MAX + 1];
};

#define FNU_DIRENT_LEN(namelen)                                                \
  ((__offsetof(struct fnu_dirent, name) + (namelen) + 1 + 7) & ~7)

#endif
