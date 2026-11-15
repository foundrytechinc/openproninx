#ifndef _SYS_MOUNT_H_
#define _SYS_MOUNT_H_

#include <sys/types.h>
#include "inc/abi.h"

#define MNT_RDONLY FNU_MNT_RDONLY
#define MNT_FORCE FNU_MNT_FORCE
#define MNT_NOSUID FNU_MNT_NOSUID
#define MNT_LOCAL FNU_MNT_LOCAL

// getfsstat(2): the volumes are always counted fresh
#define MNT_WAIT 1
#define MNT_NOWAIT 2

#define MFSNAMELEN FNU_MNT_TYPELEN
#define MNAMELEN FNU_MNT_NAMELEN

typedef struct {
  int32_t val[2];
} fsid_t;

// the part of statfs the system keeps
struct statfs {
  u_int32_t f_flags;
  u_int32_t f_bsize;
  u_int32_t f_iosize;
  u_int64_t f_blocks;
  u_int64_t f_bfree;
  int64_t f_bavail;
  u_int64_t f_files;
  u_int64_t f_ffree;
  int64_t f_favail;
  fsid_t f_fsid;
  u_int32_t f_namemax;
  char f_fstypename[MFSNAMELEN];
  char f_mntonname[MNAMELEN];
  char f_mntfromname[MNAMELEN];
};

int mount(const char *type, const char *dir, int flags, void *data);
int unmount(const char *dir, int flags);
int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);
int getfsstat(struct statfs *buf, size_t bufsize, int flags);

#endif
