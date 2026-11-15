// UFS2
#ifndef PRONINX_BOOT_UFS2READ_H
#define PRONINX_BOOT_UFS2READ_H

#include "boot/bootdisk.h"

#define UFS2RD_DIRBLK 512
#define UFS2RD_VBLK BOOTDISK_MAXBLK

struct ufs2rd {
  const struct bootdisk *disk;
  uint64 start;
  uint32 bsize, fsize, frag, bshift, fshift, nshift;
  uint32 fpg, ipg, inopb, iblkno;
  uint8 buf[UFS2RD_VBLK];
  uint8 dir[UFS2RD_DIRBLK];
};

int ufs2rd_mount(struct ufs2rd *u);
int ufs2rd_lookup(struct ufs2rd *u, const char *path, uint32 *ino, uint64 *size);
int ufs2rd_read(struct ufs2rd *u, uint32 ino, uint64 off, void *dst, uint32 n);

#endif
