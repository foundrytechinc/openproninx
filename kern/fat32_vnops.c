// FAT

#include "defs.h"
#include "fat32.h"
#include "file.h"
#include "fs.h"
#include "inc/stat.h"
#include "mmu.h"
#include "param.h"
#include "uio.h"
#include "vfs.h"

#define FAT_SIZE_MAX 0xFFFFFFFFull

static uint fat_inum(uint parent_cluster, uint off) {
  return 0x80000000u | (parent_cluster << 12) | (off / 32);
}

void fat32_renumber(struct inode *ip) {
  uint inum;

  if (ip->fat32_info.cluster == 0)
    inum = fat_inum(ip->fat32_info.parent_cluster, ip->fat32_info.dir_off);
  else
    inum = ip->fat32_info.cluster;
  if (inum != ip->inum && ip->inum != fat32_root_cluster((int)ip->dev))
    iupdate_inum(ip, inum);
}

static void fat_load(struct inode *ip) {
  const struct vfs_mount *m = vfs_on_dev(ip->dev);

  ip->nlink = 1;
  ip->valid = 1;
  if (m != 0 && ip->inum == m->root_inum) {
    ip->type = FNU_TYPE_DIR;
    ip->mode = 0755;
    ip->fat32_info.cluster = m->root_inum;
    fat32_fc_purge(ip);
    ip->fat32_info.attr = FAT32_ATTR_DIRECTORY;
    ip->size = fat32_dir_size(ip);
    fat32_root_times(ip);
  } else
    ip->type = 0;
}

static void fat_inactive(struct inode *ip) {
  fat32_truncate(ip);
  fat32_renumber(ip);
}

static int fat_truncate(struct inode *ip, uint64_t length) {
  int error;

  if ((error = -fat32_truncate_to(ip, length)) > 0)
    return error;
  if (length == 0)
    fat32_renumber(ip);
  ip->iflag |= IN_CHANGE | IN_UPDATE;
  fat32_iupdate(ip);
  return 0;
}

static int fat_read(struct inode *ip, struct uio *uio) {
  uint64_t off = uio->uio_offset;

  if (off >= ip->size)
    return 0;
  if (uio->uio_resid > ip->size - off)
    uio->uio_resid = (size_t)(ip->size - off);
  ip->iflag |= IN_ACCESS;
  return fat32_read_uio(ip, uio);
}

static int fat_fill(struct inode *ip, uint64_t to) {
  char *blank;
  uint64_t at = ip->size;

  if ((blank = page_alloc()) == 0)
    return FNU_ERR_NOMEMORY;
  memset(blank, 0, PGSIZE);
  while (at < to) {
    uint chunk = to - at < PGSIZE ? (uint)(to - at) : PGSIZE;

    if (fat32_write(ip, blank, (uint)at, chunk) != (int)chunk) {
      page_free(blank);
      return FNU_ERR_NOSPACE;
    }
    at += chunk;
    ip->size = (uint)at;
  }
  page_free(blank);
  fat32_renumber(ip);
  return 0;
}

static int fat_write(struct inode *ip, struct uio *uio) {
  uint64_t off = uio->uio_offset;
  size_t n = uio->uio_resid;
  int written, error;

  if (off + n > FAT_SIZE_MAX)
    return -FNU_ERR_TOOLARGE;
  if (off > ip->size && (error = fat_fill(ip, off)) != 0)
    return -error;
  written = fat32_write_uio(ip, uio);
  if (written < 0)
    return -FNU_ERR_IO;
  if (written == 0)
    return n > 0 ? -FNU_ERR_NOSPACE : 0;
  fat32_renumber(ip);
  if (off + (uint)written > ip->size)
    ip->size = (uint)(off + (uint)written);
  ip->iflag |= IN_CHANGE | IN_UPDATE;
  fat32_iupdate(ip);
  return written;
}

static struct inode *fat_lookup(struct inode *dp, char *name) {
  struct fat32_dirent de;
  struct inode *ip;
  uint off = 0, cluster;

  memset(&de, 0, sizeof(de));
  cluster = fat32_lookup(dp, name, &de, &off);
  if (cluster == 0 && de.name[0] == 0)
    return 0;
  // no cluster is the root
  if (cluster == 0 && (de.attr & FAT32_ATTR_DIRECTORY))
    cluster = fat32_root_cluster((int)dp->dev);
  ip = iget(dp->dev, cluster != 0 ? cluster
                                  : fat_inum(dp->fat32_info.cluster, off));
  if (ip == 0)
    return 0;
  ilock(ip);
  if (ip->type == 0) {
    ip->fat32_info.cluster = cluster;
    fat32_fc_purge(ip);
    ip->fat32_info.attr = de.attr;
    fat32_dirent_times(&de, &ip->atime, &ip->mtime, &ip->birthtime);
    ip->ctime = ip->mtime;
    ip->iflag = 0;
    ip->fat32_info.parent_cluster = dp->fat32_info.cluster;
    ip->fat32_info.dir_off = off;
    ip->size = (de.attr & FAT32_ATTR_DIRECTORY) ? fat32_dir_size(ip) : de.file_size;
    ip->nlink = 1;
    ip->uid = 0;
    ip->gid = 0;
    ip->mode = fat32_mode_of(de.attr);
    ip->type = (de.attr & FAT32_ATTR_DIRECTORY) ? FNU_TYPE_DIR : FNU_TYPE_FILE;
  }
  iunlock(ip);
  return ip;
}

static int fat_create(struct inode *dp, char *name, struct vattr *va,
                      struct inode **ipp) {
  struct inode *ip;

  if (va->va_type != FNU_TYPE_FILE && va->va_type != FNU_TYPE_DIR)
    return FNU_ERR_UNSUPPORTED;
  if ((ip = fat32_ialloc(dp->dev, va->va_type)) == 0)
    return FNU_ERR_NOSPACE;
  ilock(ip);
  if (va->va_type == FNU_TYPE_FILE && !(va->va_mode & 0200)) {
    ip->fat32_info.attr |= FAT32_ATTR_READ_ONLY;
    ip->mode = fat32_mode_of(ip->fat32_info.attr);
  }
  if (fat32_dirlink(dp, name, ip, 1) < 0) {
    iunlockput(ip);
    return FNU_ERR_NOSPACE;
  }
  fat32_renumber(ip);
  *ipp = ip;
  return 0;
}

static int fat_dropname(struct inode *dp, char *name) {
  struct fat32_dirent de;
  uint off = 0;

  memset(&de, 0, sizeof(de));
  if (fat32_lookup(dp, name, &de, &off) == 0 && de.name[0] == 0)
    return -1;
  return fat32_dirunlink(dp, off);
}

static int fat_remove(struct inode *dp, char *name, struct inode *ip) {
  if (ip->nlink < 1 || fat_dropname(dp, name) < 0)
    return FNU_ERR_IO;
  ip->nlink--;
  return 0;
}

static int fat_dotdot(struct inode *ip, struct inode *parent) {
  struct fat32_dirent de;
  uint cluster = parent->fat32_info.cluster;

  if (fat32_read(ip, (char *)&de, sizeof(de), sizeof(de)) != (int)sizeof(de) ||
      de.name[0] != '.' || de.name[1] != '.')
    return -1;
  if (cluster == fat32_root_cluster((int)ip->dev))
    cluster = 0;
  de.first_cluster_high = (ushort)((cluster >> 16) & 0xFFFF);
  de.first_cluster_low = (ushort)(cluster & 0xFFFF);
  return fat32_write(ip, (char *)&de, sizeof(de), sizeof(de)) ==
                 (int)sizeof(de)
             ? 0
             : -1;
}

static int fat_rename(struct inode *fdp, char *fname, struct inode *ip,
                      struct inode *tdp, char *tname, struct inode *victim) {
  if (victim != 0) {
    if (fat_dropname(tdp, tname) < 0)
      return FNU_ERR_IO;
    if (victim->type == FNU_TYPE_DIR)
      victim->nlink = 0;
    else if (victim->nlink > 0)
      victim->nlink--;
  }
  if (fat32_dirlink(tdp, tname, ip, 0) < 0 || fat_dropname(fdp, fname) < 0)
    return FNU_ERR_IO;
  fat32_renumber(ip);
  if (ip->type == FNU_TYPE_DIR && tdp != fdp && fat_dotdot(ip, tdp) < 0)
    return FNU_ERR_IO;
  return 0;
}

// the root has no . and ..: made up in front of it
static int fat_readdir(struct inode *dp, uint off, uint *next, uint *inum,
                       char *name, int namesz, uchar *dtype) {
  struct fat32_dirent de;
  uint bias = dp->inum == fat32_root_cluster((int)dp->dev) ? 2 * sizeof(de) : 0;
  uint at;

  if (off < bias) {
    if (namesz < 3)
      return -1;
    strlcpy(name, off == 0 ? "." : "..", (size_t)namesz);
    *inum = dp->inum;
    *dtype = FNU_TYPE_DIR;
    *next = off + sizeof(de);
    return 0;
  }
  at = off - bias;
  if (fat32_readdir(dp, 0, &at, &de, name, namesz) <= 0)
    return -1;
  *inum = fat32_dirent_cluster((int)dp->dev, &de);
  if (*inum == 0)
    *inum = fat_inum(dp->fat32_info.cluster, at - sizeof(de));
  *dtype = (de.attr & FAT32_ATTR_DIRECTORY) ? FNU_TYPE_DIR : FNU_TYPE_FILE;
  *next = at + bias;
  return 0;
}

static int fat_setattr(struct inode *ip, const struct vattr *va) {
  if (((va->va_mask & AT_OWNER) &&
       (va->va_uid != ip->uid || va->va_gid != ip->gid)) ||
      (va->va_mask & AT_FLAGS))
    return FNU_ERR_UNSUPPORTED;
  if ((va->va_mask & AT_MODE) && ip->type == FNU_TYPE_FILE) {
    if (va->va_mode & 0200)
      ip->fat32_info.attr &= ~FAT32_ATTR_READ_ONLY;
    else
      ip->fat32_info.attr |= FAT32_ATTR_READ_ONLY;
    ip->mode = fat32_mode_of(ip->fat32_info.attr);
  }
  if (va->va_mask & AT_ATIME) {
    ip->atime = va->va_atime;
    ip->iflag &= ~IN_ACCESS;
  }
  if (va->va_mask & AT_MTIME) {
    ip->mtime = va->va_mtime;
    ip->iflag &= ~IN_UPDATE;
  }
  ip->iflag |= IN_CHANGE;
  fat32_iupdate(ip);
  return 0;
}

static void fat_getattr(struct inode *ip, struct fnu_stat *st) {
  uint per = fat32_cluster_bytes(ip->dev);

  if (per > st->blksize)
    st->blksize = per;
  st->blocks = per == 0 ? 0 : (int64_t)((ip->size + per - 1) / per) * (per / 512);
}

static int fat_pathconf(struct inode *ip, int name, int64_t *value) {
  switch (name) {
  case FNU_PC_LINK_MAX:
    *value = 1;
    return 0;
  case FNU_PC_NAME_MAX:
    *value = 255;
    return 0;
  case FNU_PC_2_SYMLINKS:
    *value = 0;
    return 0;
  case FNU_PC_ALLOC_SIZE_MIN:
    *value = fat32_cluster_bytes(ip->dev);
    return 0;
  case FNU_PC_FILESIZEBITS:
    *value = 32;
    return 0;
  case FNU_PC_TIMESTAMP_RESOLUTION:
    *value = 2000000000;
    return 0;
  default:
    return vop_pathconf_default(name, value);
  }
}

static const struct vnodeops fat32_vnops = {
    .load = fat_load,
    .update = fat32_iupdate,
    .inactive = fat_inactive,
    .truncate = fat_truncate,
    .read = fat_read,
    .write = fat_write,
    .lookup = fat_lookup,
    .create = fat_create,
    .remove = fat_remove,
    .rename = fat_rename,
    .readdir = fat_readdir,
    .isempty = fat32_isdirempty,
    .setattr = fat_setattr,
    .getattr = fat_getattr,
    .pathconf = fat_pathconf,
};

static void fat_unmount(const struct vfs_mount *m) { fat32_forget((int)m->dev); }

static void fat_sync(const struct vfs_mount *m) { fat32_sync((int)m->dev); }

// remount
static int fat_update(struct vfs_mount *m, int flags) {
  if (flags & FNU_MNT_RDONLY)
    fat32_quiesce((int)m->dev);
  return 0;
}

static int fat_statfs(const struct vfs_mount *m, struct fnu_fsstat *st) {
  return fat32_statfs((int)m->dev, st);
}

const struct vfsops fat32_vfsops = {
    .name = "fat32",
    .vnops = &fat32_vnops,
    .unmount = fat_unmount,
    .sync = fat_sync,
    .update = fat_update,
    .statfs = fat_statfs,
};
