#ifndef PRONINX_X86_64_FILE_H
#define PRONINX_X86_64_FILE_H

#include "inc/abi.h"
#include "fs.h"
#include "inc/types.h"
#include "sleeplock.h"
#include "ufs2.h"
#include "vnode.h"

struct socket;
struct eventq;
struct knote;

#define FRELE(f) fileclose(f)

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_SOCKET, FD_EVENTQ } type;
  int ref;
  char readable;
  char writable;
  int flags;
  struct pipe *pipe;
  struct socket *sock;
  struct inode *ip;
  struct eventq *eq;
  uint64_t off;
  int flocked;
};

#define IN_ACCESS 0x01
#define IN_UPDATE 0x02
#define IN_CHANGE 0x04
#define IN_MODIFIED 0x08

// in-memory copy of an inode
struct inode {
  struct inode *i_hash;
  struct inode *i_free; // the spare ones
  uint dev;
  uint inum;
  int ref;
  struct sleeplock lock;
  int valid;

  short type;
  uint64_t size;
  short nlink;
  uint uid;
  uint gid;
  ushort mode;
  uint flags; // FNU_UF_ and FNU_SF_

  struct fnu_timespec atime;
  struct fnu_timespec mtime;
  struct fnu_timespec ctime;
  struct fnu_timespec birthtime;
  int iflag;
  int writecount;
  int flk_shared;
  struct file *flk_excl;
  struct knote *klist;

  const struct vnodeops *i_op;
  union {
    struct {
      short major;
      short minor;
    };
    struct {
      uint cluster;
      uchar attr;
      uint parent_cluster;
      uint dir_off;
      uint fc_index[2];
      uint fc_cluster[2];
    } fat32_info;
    struct ufs2_inode ufs2_info;
  };
};

// character device switch
struct uio;

struct devsw {
  int (*read)(struct inode *, struct uio *);
  int (*write)(struct inode *, struct uio *);
  int (*open)(struct inode *, int);
  void (*close)(struct inode *);
  int (*ioctl)(struct inode *, uint64_t cmd, uint64_t arg);
  int (*poll)(struct inode *, int want);
  int class; // FNU_DEVCLASS_*
};

extern struct devsw devsw[];

#define CONSOLE 1
#define PTM 2
#define HOTPLUG 3
#define RANDOM 4
#define WSMOUSE 5
#define AUDIO 6
#define NULLDEV 7
#define ZERODEV 8
#define PTY 9
#define KLOG 10
#define CONSOLECTL 11
#define SYSMOUSE 12
#define CTTY 13 // /dev/tty
#define PSM 14
#define ACPITBLS 15
#define TPMDEV 16


#endif
