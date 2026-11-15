#ifndef PRONINX_X86_64_STAT_H
#define PRONINX_X86_64_STAT_H

#include "inc/types.h"

#define FNU_TYPE_NONE 0
#define FNU_TYPE_FILE 1
#define FNU_TYPE_DIR 2
#define FNU_TYPE_LINK 3
#define FNU_TYPE_FIFO 4
#define FNU_TYPE_SOCKET 5
#define FNU_TYPE_DEVICE 6
#define FNU_TYPE_DISK 7

#define FNU_MODE_SETUID 04000
#define FNU_MODE_SETGID 02000
#define FNU_MODE_STICKY 01000
#define FNU_MODE_OWNER_READ 0400
#define FNU_MODE_OWNER_WRITE 0200
#define FNU_MODE_OWNER_EXEC 0100
#define FNU_MODE_GROUP_READ 0040
#define FNU_MODE_GROUP_WRITE 0020
#define FNU_MODE_GROUP_EXEC 0010
#define FNU_MODE_OTHER_READ 0004
#define FNU_MODE_OTHER_WRITE 0002
#define FNU_MODE_OTHER_EXEC 0001
#define FNU_MODE_BITS 07777

#define FNU_UF_SETTABLE 0x0000ffff
#define FNU_UF_NODUMP 0x00000001
#define FNU_UF_IMMUTABLE 0x00000002
#define FNU_UF_APPEND 0x00000004
#define FNU_UF_OPAQUE 0x00000008
#define FNU_SF_SETTABLE 0xffff0000
#define FNU_SF_ARCHIVED 0x00010000
#define FNU_SF_IMMUTABLE 0x00020000
#define FNU_SF_APPEND 0x00040000
#define FNU_IMMUTABLE (FNU_UF_IMMUTABLE | FNU_SF_IMMUTABLE)
#define FNU_APPEND (FNU_UF_APPEND | FNU_SF_APPEND)

#define FNU_SET_MODE 0x01
#define FNU_SET_UID 0x02
#define FNU_SET_GID 0x04
#define FNU_SET_FLAGS 0x08
#define FNU_SET_ATIME 0x10 // FNU_TIME_NOW in nsec: now
#define FNU_SET_MTIME 0x20
#define FNU_SET_SIZE 0x40

struct fnu_setattr {
  uint32 mask;
  uint32 mode;
  uint32 uid;
  uint32 gid;
  uint32 flags;
  uint32 pad;
  uint64 size;
  struct fnu_timespec atime;
  struct fnu_timespec mtime;
};

struct fnu_stat {
  uint8 type;
  uint8 devclass; // a device's FNU_DEVCLASS_*
  pointer_t size;
  pointer_t device;
  uint32 mode;      // FNU_MODE_ bits
  uint32 uid;
  uint32 gid;
  uint32 nlink;
  int16_t major;
  int16_t minor;
  uint32 flags;
  struct fnu_timespec atime;
  struct fnu_timespec mtime;
  struct fnu_timespec ctime;
  struct fnu_timespec birthtime;
  uint64 ino;
  int64_t blocks;
  uint32 gen;
  uint32 blksize; // the size I/O goes best in
};

#endif
