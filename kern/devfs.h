#ifndef PRONINX_DEVFS_H
#define PRONINX_DEVFS_H

#include "inc/types.h"

struct inode;
struct process;

#define DEVFS_DEV 0xfffffff0U
#define DEVFS_ROOT_INO 1
#define DEVFS_NAME_MAX 40

struct devfs_node {
  char name[DEVFS_NAME_MAX + 1];
  uint inum;
  uint parent;
  short type;
  short major;
  short minor;
  ushort mode;
  uint uid;
  uint gid;
  int used;
  int64_t born; // uptime ns at make
};

void devfs_init(void);

uint devfs_add(const char *name, short type, short major, short minor,
               ushort mode);
void devfs_remove(const char *name);

uint devfs_add_alias(const char *dir, const char *what, short type,
                     short major, short minor, ushort mode);
int devfs_has_alias(const char *dir, short major, short minor);
void devfs_remove_aliases(const char *dir, short major, short minor);
void devfs_remove_alias(const char *dir, const char *what, short major,
                        short minor);

int devfs_lookup(uint dir, const char *name, struct devfs_node *out);
struct inode *devfs_ctty(struct process *pr);
int devfs_by_inum(uint inum, struct devfs_node *out);
int devfs_nth(uint dir, uint index, struct devfs_node *out);
uint devfs_parent(uint inum);
int devfs_setattr(uint inum, ushort mode, uint uid, uint gid);

#endif
