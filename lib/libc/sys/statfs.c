// statfs, getfsstat, statvfs over the volume table
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

static void statfs_of(const struct fnu_fsstat *v, struct statfs *sf) {
  memset(sf, 0, sizeof(*sf));
  sf->f_flags = v->flags;
  sf->f_bsize = v->bsize;
  sf->f_iosize = v->iosize;
  sf->f_blocks = v->blocks;
  sf->f_bfree = v->bfree;
  sf->f_bavail = v->bavail;
  sf->f_files = v->files;
  sf->f_ffree = v->ffree;
  sf->f_favail = v->favail;
  sf->f_fsid.val[0] = (int32_t)v->dev;
  sf->f_namemax = v->namemax;
  strlcpy(sf->f_fstypename, v->type, sizeof(sf->f_fstypename));
  strlcpy(sf->f_mntonname, v->on, sizeof(sf->f_mntonname));
  strlcpy(sf->f_mntfromname, v->from, sizeof(sf->f_mntfromname));
}

// the volume the descriptor or name is on
static int volume_of(int fd, const char *path, struct statfs *sf) {
  struct fnu_fsstat *table;
  struct fnu_stat st;
  int n, i, r;

  if (__posix(fnu_getattr(fd, path, 0, &st)) < 0)
    return -1;
  if ((n = (int)__posix(fnu_volumes(&table))) < 0)
    return -1;
  for (i = 0; i < n && table[i].dev != st.device; i++)
    ;
  if ((r = i < n ? 0 : -1) == 0)
    statfs_of(&table[i], sf);
  else
    errno = ENOENT;
  fnu_table_free(table);
  return r;
}

int statfs(const char *path, struct statfs *buf) {
  return volume_of(FNU_FD_CWD, path, buf);
}

int fstatfs(int fd, struct statfs *buf) { return volume_of(fd, 0, buf); }

// no buffer: how many; else as many as fit
int getfsstat(struct statfs *buf, size_t bufsize, int flags) {
  struct fnu_fsstat *table;
  int n, i, room;

  if (flags != 0 && flags != MNT_WAIT && flags != MNT_NOWAIT) {
    errno = EINVAL;
    return -1;
  }
  if ((n = (int)__posix(fnu_volumes(&table))) < 0)
    return -1;
  if (buf != 0) {
    room = (int)(bufsize / sizeof(*buf));
    if (n > room)
      n = room;
    for (i = 0; i < n; i++)
      statfs_of(&table[i], &buf[i]);
  }
  fnu_table_free(table);
  return n;
}

static void statvfs_of(const struct statfs *sf, struct statvfs *vf) {
  memset(vf, 0, sizeof(*vf));
  vf->f_bsize = sf->f_iosize;
  vf->f_frsize = sf->f_bsize;
  vf->f_blocks = sf->f_blocks;
  vf->f_bfree = sf->f_bfree;
  vf->f_bavail = sf->f_bavail < 0 ? 0 : (fsblkcnt_t)sf->f_bavail;
  vf->f_files = sf->f_files;
  vf->f_ffree = sf->f_ffree;
  vf->f_favail = sf->f_favail < 0 ? 0 : (fsfilcnt_t)sf->f_favail;
  vf->f_fsid = (unsigned long)(uint32_t)sf->f_fsid.val[0];
  vf->f_flag = ((sf->f_flags & MNT_RDONLY) ? ST_RDONLY : 0) |
               ((sf->f_flags & MNT_NOSUID) ? ST_NOSUID : 0);
  vf->f_namemax = sf->f_namemax;
}

int statvfs(const char *path, struct statvfs *buf) {
  struct statfs sf;

  if (statfs(path, &sf) < 0)
    return -1;
  statvfs_of(&sf, buf);
  return 0;
}

int fstatvfs(int fd, struct statvfs *buf) {
  struct statfs sf;

  if (fstatfs(fd, &sf) < 0)
    return -1;
  statvfs_of(&sf, buf);
  return 0;
}
