#ifndef PRONINX_X86_64_STAT_H
#define PRONINX_X86_64_STAT_H

#include "inc/types.h"

#define T_DIR 1     // Directory
#define T_FILE 2    // File
#define T_DEVICE 3  // Device

struct stat {
  uint8 type;       // File type (T_DIR, T_FILE, T_DEVICE)
  pointer_t size;   // Size in bytes
  pointer_t device; // Device ID
};

#endif /* ifndef PRONINX_X86_64_STAT_H */
