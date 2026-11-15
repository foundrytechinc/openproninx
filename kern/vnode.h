#ifndef PRONINX_VNODE_H
#define PRONINX_VNODE_H

#include "inc/types.h"
#include "inc/abi.h"

struct inode;
struct uio;
struct fnu_stat;
struct fnu_fsstat;
struct vfs_mount;

#define AT_MODE 0x01
#define AT_OWNER 0x02
#define AT_FLAGS 0x04
#define AT_ATIME 0x08
#define AT_MTIME 0x10

struct vattr {
  int va_mask;
  short va_type;
  ushort va_mode;
  uint va_uid;
  uint va_gid;
  uint va_flags;
  short va_major;
  short va_minor;
  struct fnu_timespec va_atime;
  struct fnu_timespec va_mtime;
};

// back locked
struct vnodeops {
  void (*load)(struct inode *ip);
  void (*update)(struct inode *ip);
  void (*inactive)(struct inode *ip);
  int (*truncate)(struct inode *ip, uint64_t length);
  int (*read)(struct inode *ip, struct uio *uio);
  int (*write)(struct inode *ip, struct uio *uio);
  struct inode *(*lookup)(struct inode *dp, char *name);
  int (*create)(struct inode *dp, char *name, struct vattr *va,
                struct inode **ipp);
  int (*symlink)(struct inode *dp, char *name, struct vattr *va,
                 const char *target, struct inode **ipp);
  int (*link)(struct inode *dp, char *name, struct inode *ip);
  int (*remove)(struct inode *dp, char *name, struct inode *ip);
  int (*rename)(struct inode *fdp, char *fname, struct inode *ip,
                struct inode *tdp, char *tname, struct inode *victim);
  int (*readlink)(struct inode *ip, char *buf, int size);
  int (*readdir)(struct inode *dp, uint off, uint *next, uint *inum,
                 char *name, int namesz, uchar *dtype);
  int (*isempty)(struct inode *dp);
  int (*setattr)(struct inode *ip, const struct vattr *va);
  void (*getattr)(struct inode *ip, struct fnu_stat *st);
  int (*pathconf)(struct inode *ip, int name, int64_t *value);
};

// one filesystem's volumes
struct vfsops {
  const char *name;
  const struct vnodeops *vnops;
  void (*start)(struct vfs_mount *m);
  void (*unmount)(const struct vfs_mount *m);
  void (*sync)(const struct vfs_mount *m);
  int (*update)(struct vfs_mount *m, int flags);
  int (*statfs)(const struct vfs_mount *m, struct fnu_fsstat *st);
};

extern const struct vfsops ufs2_vfsops, fat32_vfsops, devfs_vfsops;
extern const struct vnodeops dead_vnops;

int vop_pathconf_default(int name, int64_t *value);

#endif
