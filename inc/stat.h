#ifndef PRONINX_X86_64_STAT_H
#define PRONINX_X86_64_STAT_H

#include "inc/types.h"

#define T_DIR 1     // Directory
#define T_FILE 2    // File
#define T_DEVICE 3  // Device

#ifndef S_IRUSR
#define S_IRUSR 0400
#endif
#ifndef S_IWUSR
#define S_IWUSR 0200
#endif
#ifndef S_IXUSR
#define S_IXUSR 0100
#endif
#ifndef S_IRGRP
#define S_IRGRP 0040
#endif
#ifndef S_IWGRP
#define S_IWGRP 0020
#endif
#ifndef S_IXGRP
#define S_IXGRP 0010
#endif
#ifndef S_IROTH
#define S_IROTH 0004
#endif
#ifndef S_IWOTH
#define S_IWOTH 0002
#endif
#ifndef S_IXOTH
#define S_IXOTH 0001
#endif

struct stat {
  uint8 type;       // File type (T_DIR, T_FILE, T_DEVICE)
  pointer_t size;   // Size in bytes
  pointer_t device; // Device ID
  uint32 mode;
  uint32 uid;
  uint32 gid;
};

#endif /* ifndef PRONINX_X86_64_STAT_H */
