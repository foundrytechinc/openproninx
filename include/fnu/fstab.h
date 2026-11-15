#ifndef _FNU_FSTAB_H_
#define _FNU_FSTAB_H_

#include "inc/abi.h"
#include <sys/types.h>

#define FSTAB_PATH "/etc/filesystems"
#define FSTAB_LINE 256

#define FSTAB_AT_BOOT 0
#define FSTAB_AT_PLUG 1
#define FSTAB_AT_MANUAL 2

struct fstab_entry {
  struct fnu_mount_req request;
  int when;
};

struct fstab {
  int fd;
  char buf[FSTAB_LINE];
  int len;
  int pos;
};

int fstab_open(struct fstab *table);
int fstab_next(struct fstab *table, struct fstab_entry *entry);
void fstab_close(struct fstab *table);

int fstab_by_mountpoint(const char *on, struct fstab_entry *entry);
int fstab_by_device(dev_t dev, struct fstab_entry *entry);

#endif
