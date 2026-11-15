#ifndef PRONINX_VFS_H
#define PRONINX_VFS_H

#include "inc/types.h"
#include "inc/abi.h"
#include "ufs2.h"
#include "vnode.h"

#define VFS_NAMELEN FNU_MNT_NAMELEN
#define VFS_TYPELEN 8

// the mount table
struct vfs_mount {
  int used;

  uint dev;
  const struct vfsops *op;
  uint root_inum;
  int readonly;
  int nosuid;

  uint cover_dev;
  uint cover_inum;

  char from[VFS_NAMELEN];
  char on[VFS_NAMELEN];
  char type[VFS_TYPELEN];

  struct ufs2_volume ufs2;
};

void vfs_init(void);
int vfs_sync(void);
void vfs_dismount_all(void);
int vfs_sync_dev(uint dev);

int vfs_mount_add(struct vfs_mount *m);
int vfs_update(uint dev, int flags);
int vfs_unmount_at(uint cover_dev, uint cover_inum);
int vfs_dev_mounted(uint dev);

const struct vfs_mount *vfs_covering(uint dev, uint inum);
const struct vfs_mount *vfs_rooted_at(uint dev, uint inum);
const struct vfs_mount *vfs_root(void);
int vfs_nosuid(uint dev);
int vfs_readonly(uint dev);
const struct vfs_mount *vfs_on_dev(uint dev);
const struct vnodeops *vfs_vnops(uint dev);
const struct ufs2_volume *vfs_ufs2_volume(uint dev);

int vfs_slots(void);
const struct vfs_mount *vfs_nth(int i);

#endif
