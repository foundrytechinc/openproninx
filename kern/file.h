#ifndef PRONINX_X86_64_FILE_H
#define PRONINX_X86_64_FILE_H

#include "fs.h"
#include "inc/types.h"
#include "sleeplock.h"
#include "ufs2.h"

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe;
  struct inode *ip;
  uint off;
};

// in-memory copy of an inode
struct inode {
  uint dev;              // Device number
  uint inum;             // Inode number
  int ref;               // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;             // inode has been read from disk?

  short type;            // Generic type (T_DIR, T_FILE, etc)
  uint size;             // File size in bytes
  short nlink;           // Common cached link count; never overlaps fs data
  uint uid;
  uint gid;
  ushort mode;

  enum { FS_NATIVE, FS_FAT32, FS_UFS2, FS_DEVFS } fs_type;
  union {
    struct {
      short major;
      short minor;
      uint addrs[NDIRECT + 2];
    }; // Anonymous struct for native specific fields
    struct {
      uint cluster;
      uchar attr;
      uint parent_cluster;
      uint dir_off;
    } fat32_info;
    struct ufs2_inode ufs2_info;
  };
};

// table mapping major device number to
// device functions
struct devsw {
  int (*read)(struct inode *, char *, int);
  int (*write)(struct inode *, char *, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
#define FNU_STATE 2

#define FNU_STATE_COMMAND 1
#define FNU_STATE_SERVICES 2
#define FNU_STATE_HEALTH 3

#endif /* ifndef PRONINX_X86_64_FILE_H */
