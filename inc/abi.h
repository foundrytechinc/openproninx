#ifndef PRONINX_X86_64_ABI_H
#define PRONINX_X86_64_ABI_H

#include "inc/types.h"

struct info {
  uint64 uptime;           // System uptime in ticks
  uint64 total_ram;        // Total physical memory
  uint64 free_ram;         // Free physical memory
  uint32 nprocs;           // Number of active processes
};

#define USER_NAME_MAX 16
struct user_info {
  char name[USER_NAME_MAX];
  uint32 uid;
  uint32 gid;
  uint32 wheel;
};

struct network_ping_request {
  uint8 address[4];
  uint32 timeout_ms;
  uint32 round_trip_ms;
};

struct network_endpoint { uint8 address[4]; uint16 port; };

/* Set dhcp to zero for a static address; non-zero restarts DHCP. */
struct network_ipv4_config {
  uint8 address[4];
  uint8 netmask[4];
  uint8 gateway[4];
  uint8 dns_server[4];
  uint32 dhcp;
};

#define NETWORK_DNS_NAME_MAX 64
struct network_dns_request {
  char name[NETWORK_DNS_NAME_MAX];
  uint8 address[4];
  uint32 timeout_ms;
};

struct network_status {
  char interface_name[16];
  uint8 hardware_address[6];
  uint8 address[4];
  uint8 gateway[4];
  uint8 dns_server[4];
  uint32 dhcp_bound;
  uint64 received_frames;
  uint64 dropped_frames;
  uint64 transmitted_frames;
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
#define ECHOPASS 0000020 // Echo password input as masking characters.

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
