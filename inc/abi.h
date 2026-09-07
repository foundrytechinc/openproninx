#ifndef PRONINX_X86_64_ABI_H
#define PRONINX_X86_64_ABI_H

#include "inc/types.h"

/*
 * OpenProninx Application Binary Interface (ABI) Header
 *
 * Tier 1: STABLE ABI STRUCTURES
 * Frozen layouts with fixed field alignments and sizes. Guaranteed binary
 * compatibility for userland programs.
 *
 * Tier 2: EXPERIMENTAL / SUBSYSTEM-SPECIFIC STRUCTURES
 * Internal structures for kernel-level auth.
 * Subject to change or replacement by userland auth APIs.
 */

/* ========================================================================== */
/* STABLE ABI DATA STRUCTURES                                                 */
/* ========================================================================== */

/* System Information (SYS_info) */
struct info {
  uint64 uptime;           // System uptime in ticks
  uint64 total_ram;        // Total physical memory
  uint64 free_ram;         // Free physical memory
  uint32 nprocs;           // Number of active processes
};

/* Process Inspection (SYS_procinfo) */
enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct procinfo {
  pointer_t pid;           // Process ID
  proc_state_t state;      // Process state
  char name[16];           // Process name
  pointer_t memory_size;   // Process memory size in bytes
};

/* Directory Entry 64-bit (SYS_getdents) */
struct linux_dirent64 {
  uint64 d_ino;            // Inode number (64 bits)
  int64  d_off;            // Offset to next entry
  unsigned short d_reclen; // Size of this entry
  unsigned char  d_type;   // File type (DT_DIR, DT_REG, etc.)
  char           d_name[]; // Filename (null-terminated)
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/* Terminal Attributes & Control (SYS_ioctl TCGETS / TCSETS) */
struct termios {
    uint32 c_iflag;      // Input mode flags
    uint32 c_oflag;      // Output mode flags
    uint32 c_cflag;      // Control mode flags
    uint32 c_lflag;      // Local mode flags (ICANON, ECHO, etc.)
    uint8  c_line;       // Line discipline
    uint8  c_cc[19];     // Control characters
};

#define ICANON   0000002
#define ECHO     0000010
#define ECHOPASS 0000020 // Echo password input as masking characters.

#define TCGETS   0x5401
#define TCSETS   0x5402

/* Display Resolution Control (SYS_ioctl FBIOGET_VSCREENINFO / FBIOPUT_VSCREENINFO) */
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601

struct fb_var_screeninfo {
    uint32 xres;           // visible resolution width
    uint32 yres;           // visible resolution height
    uint32 bits_per_pixel; // bits per pixel
};

/* Every mode the adapter itself offers (SYS_ioctl FBIOGET_MODELIST) */
#define FBIOGET_MODELIST 0x4602
#define FBIODUMPMODES    0x4603  /* raw mode table to the console */
#define FB_MODELIST_MAX 96
struct fb_modelist {
    uint32 count;
    struct { uint16 width, height, bpp; } mode[FB_MODELIST_MAX];
};

/* Network Ping Request (SYS_ping) */
struct network_ping_request {
  uint8 address[4];
  uint32 timeout_ms;
  uint32 round_trip_ms;
};

/* Network Endpoint (IP + Port) */
struct network_endpoint { uint8 address[4]; uint16 port; };

/* Network IPv4 Configuration (SYS_netconfig) */
/* Set dhcp to zero for a static address; non-zero restarts DHCP. */
struct network_ipv4_config {
  uint8 address[4];
  uint8 netmask[4];
  uint8 gateway[4];
  uint8 dns_server[4];
  uint32 dhcp;
};

/* DNS Resolution Request (SYS_dns_resolve) */
#define NETWORK_DNS_NAME_MAX 64
struct network_dns_request {
  char name[NETWORK_DNS_NAME_MAX];
  uint8 address[4];
  uint32 timeout_ms;
};

/* Network Interface Status (SYS_netinfo) */
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

/* CMOS / RTC wall-clock time snapshot. All fields are binary (not BCD). */
struct rtctime {
  uint8  sec;
  uint8  min;
  uint8  hour;
  uint8  wday;      /* 0 = Sunday .. 6 = Saturday */
  uint8  mday;      /* 1..31 */
  uint8  mon;       /* 1..12 */
  uint16 year;      /* full 4-digit year, e.g. 2026 */
};

/* Per-filesystem usage entry returned by SYS_df. */
#define DF_ENTRIES_MAX 4
struct df_stat {
  char     label[24];       /* FNU Root, FNU Data, RAM, ... */
  char     mount_point[32]; /* /, /data, --, etc. */
  uint64   total_bytes;
  uint64   used_bytes;
  uint64   free_bytes;
  uint8    is_read_only;    /* 1 if read-only volume, 0 if writable */
  uint8    is_present;      /* 1 if the device was actually probed */
  uint8    _pad[6];
};

/* ========================================================================== */
/* EXPERIMENTAL / SUBSYSTEM-SPECIFIC STRUCTURES                               */
/* ========================================================================== */

/* In-Kernel Account Information (SYS_users) */
/* Machine inventory, filled by sys_hwinfo. */
#define HW_BRAND_MAX  49
#define HW_VENDOR_MAX 13
struct hwinfo {
  char     cpu_brand[HW_BRAND_MAX];
  char     cpu_vendor[HW_VENDOR_MAX];
  uint8    pad0[2];
  uint32   cpu_count;
  uint32   cpu_family;
  uint32   cpu_model;
  uint32   cpu_stepping;
  uint64   tsc_hz;              /* 0 when the tsc could not be calibrated */
  uint64   cpu_base_hz;         /* rated non-turbo clock, 0 when unknown */
  uint64   cpu_max_hz;          /* highest turbo clock, 0 when unknown */
  uint32   fb_width;
  uint32   fb_height;
  uint32   fb_bpp;
  uint32   fb_columns;
  uint32   fb_rows;
  uint32   pci_devices;         /* functions a driver claimed */
  uint32   pci_total;           /* functions the bus scan found */
  uint32   fb_modeset;          /* non-zero when the mode can change */
  uint32   fb_source;           /* FNU_FB_SOURCE_*: who picked the size */
  uint32   pad1;
  uint64   fb_vram;             /* video memory the adapter admits to, bytes */
  uint64   ram_total;
  uint64   ram_free;
};

/* One attached block device, filled by sys_diskinfo. */
#define DISK_ENTRIES_MAX 8
#define DISK_MODEL_MAX   41
#define DISK_SERIAL_MAX  21
#define FNU_DISK_NONE    0
#define FNU_DISK_SATA    1
#define FNU_DISK_NVME    2
#define FNU_DISK_RAMDISK 3
struct diskinfo {
  char     model[DISK_MODEL_MAX];
  char     serial[DISK_SERIAL_MAX];
  uint8    kind;
  uint8    pad0[1];
  uint32   sector_size;
  uint32   link_gen;            /* SATA generation, 0 when not applicable */
  uint32   pad1;
  uint64   sectors;
};

#define USER_NAME_MAX 16
struct user_info {
  char name[USER_NAME_MAX];
  uint32 uid;
  uint32 gid;
  uint32 wheel;
};

#endif /* ifndef PRONINX_X86_64_ABI_H */
