#ifndef PRONINX_X86_64_ABI_H
#define PRONINX_X86_64_ABI_H

#include "inc/types.h"

struct info {
  uint64 uptime;           // System uptime in ticks
  uint64 total_ram;        // Total physical memory
  uint64 free_ram;         // Free physical memory
  uint32 nprocs;           // Number of active processes
};

struct procinfo {
  pointer_t pid;           // Process ID
  proc_state_t state;      // Process state
  char name[16];           // Process name
  pointer_t memory_size;   // Process memory size in bytes
};

struct linux_dirent64 {
  uint64 d_ino;          // Inode number (64 bits)
  int64  d_off;          // Offset to next entry
  unsigned short d_reclen; // Size of this entry
  unsigned char  d_type;   // File type (DT_DIR, DT_REG, etc.)
  char           d_name[]; // Filename (null-terminated)
};

struct termios {
    uint32 c_iflag;      // Input mode flags
    uint32 c_oflag;      // Output mode flags
    uint32 c_cflag;      // Control mode flags
    uint32 c_lflag;      // Local mode flags (ICANON, ECHO, etc.)
    uint8  c_line;       // Line discipline
    uint8  c_cc[19];     // Control characters
};

#define ICANON 0000002
#define ECHO   0000010

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#define TCGETS 0x5401
#define TCSETS 0x5402

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

#endif
