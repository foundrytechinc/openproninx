#include "defs.h"
#include "fs.h"
#include "file.h"
#include "fat32.h"
#include "ufs2.h"
#include "inc/abi.h"
#include "inc/dir.h"
#include "inc/fcntl.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"

static int argfd(int n, int *pfd, struct file **pf);
static int fdalloc(struct file *f);

int64_t sys_ioctl(void) {
  struct file *f;
  uint64_t cmd, arg_val;

  if (argfd(0, 0, &f) < 0 || arg(1, &cmd) < 0 || arg(2, &arg_val) < 0)
    return -1;

  if (f->type == FD_INODE && f->ip->major == CONSOLE) {
    return consoleioctl(f->ip, cmd, arg_val);
  }

  return -1;
}

int64_t sys_getdents(void) {
  struct file *f;
  char *buf;
  int count;
  struct dirent de;
  struct linux_dirent64 lde;
  int p = 0;

  if (argfd(0, 0, &f) < 0 || argptr(1, &buf, 0) < 0 || argint(2, &count) < 0)
    return -1;

  if (f->type != FD_INODE || f->ip->type != T_DIR)
    return -1;

  ilock(f->ip);
  if (!inode_access(f->ip, myproc()->uid, 4)) {
    iunlock(f->ip);
    return -1;
  }
  if (f->ip->fs_type == FS_FAT32) {
      char fat_buf[BSIZE];
      while (1) {
          int n = fat32_read(f->ip->dev, f->ip->fat32_info.cluster, fat_buf, f->off, BSIZE);
          if (n <= 0) break;
          
          struct fat32_dirent *fde = (struct fat32_dirent*)fat_buf;
          for (uint i = 0; i < n / sizeof(struct fat32_dirent); i++) {
              if (fde[i].name[0] == 0) {
                  f->off = 0xFFFFFFFF; // End of directory
                  goto done;
              }
              if (fde[i].name[0] == 0xE5 || fde[i].attr == FAT32_ATTR_LONG_NAME) {
                  f->off += sizeof(struct fat32_dirent);
                  continue;
              }
              
              char name[13];
              int ni = 0;
              for (int j = 0; j < 8 && fde[i].name[j] != ' '; j++) name[ni++] = fde[i].name[j];
              if (fde[i].name[8] != ' ') {
                  name[ni++] = '.';
                  for (int j = 8; j < 11 && fde[i].name[j] != ' '; j++) name[ni++] = fde[i].name[j];
              }
              name[ni] = 0;
              
              int namelen = ni;
              int reclen = (8 + 8 + 2 + 1 + namelen + 1 + 7) & ~7;
              
              if (p + reclen > count) goto done;
              
              lde.d_ino = ((uint)fde[i].first_cluster_high << 16) | fde[i].first_cluster_low;
              lde.d_off = f->off + sizeof(struct fat32_dirent);
              lde.d_reclen = (unsigned short)reclen;
              lde.d_type = (fde[i].attr & FAT32_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
              
              if (copyout(myproc()->pgdir, (uintptr_t)(buf + p), &lde, 19) < 0) goto done;
              if (copyout(myproc()->pgdir, (uintptr_t)(buf + p + 19), name, namelen + 1) < 0) goto done;
              
              f->off += sizeof(struct fat32_dirent);
              p += reclen;
          }
          if (n < BSIZE) break;
      }
  } else if (f->ip->fs_type == FS_UFS2) {
    char name[NAMEBUFSZ];
    uint inum;
    while (ufs2_readdir(storage_ufs2_volume(f->ip->dev), f->ip->inum,
                        f->off / sizeof(struct dirent), &inum, name,
                        sizeof(name)) == 0) {
      int namelen = strlen(name);
      int reclen = (19 + namelen + 1 + 7) & ~7;
      if (p + reclen > count)
        break;
      lde.d_ino = inum;
      lde.d_off = f->off + sizeof(struct dirent);
      lde.d_reclen = (unsigned short)reclen;
      lde.d_type = DT_UNKNOWN;
      if (copyout(myproc()->pgdir, (uintptr_t)(buf + p), &lde, 19) < 0 ||
          copyout(myproc()->pgdir, (uintptr_t)(buf + p + 19), name,
                  namelen + 1) < 0)
        break;
      f->off += sizeof(struct dirent);
      p += reclen;
    }
  } else {
    while (f->off < f->ip->size) {
      char name[DIRSIZ + 1];
      int namelen;
      if (readi(f->ip, (char *)&de, f->off, sizeof(de)) != sizeof(de))
        break;

      if (de.inum == 0) {
        f->off += sizeof(de);
        continue;
      }

      for (namelen = 0; namelen < DIRSIZ && de.name[namelen] != '\0';
           namelen++)
        ;
      memmove(name, de.name, namelen);
      name[namelen] = '\0';
      int reclen = 8 + 8 + 2 + 1 + namelen + 1; // ino, off, reclen, type, name, null
      reclen = (reclen + 7) & ~7;               // 8-byte alignment

      if (p + reclen > count)
        break;

      lde.d_ino = de.inum;
      lde.d_off = f->off + sizeof(de);
      lde.d_reclen = (unsigned short)reclen;
      lde.d_type = DT_UNKNOWN;

      if (copyout(myproc()->pgdir, (uintptr_t)(buf + p), &lde, 19) < 0) // copy header
        break;
      if (copyout(myproc()->pgdir, (uintptr_t)(buf + p + 19), name, namelen + 1) < 0)
        break;

      f->off += sizeof(de);
      p += reclen;
    }
  }
done:
  iunlock(f->ip);

  return p;
}

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int argfd(int n, int *pfd, struct file **pf) {
  int fd;
  struct file *f;

  if (argint(n, &fd) < 0) {
    return -1;
  }
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0) {
    return -1;
  }
  if (pfd) {
    *pfd = fd;
  }
  if (pf) {
    *pf = f;
  }
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int fdalloc(struct file *f) {
  int fd;
  struct proc *curproc = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (curproc->ofile[fd] == 0) {
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int sys_dup(void) {
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int sys_read(void) {
  struct file *f;
  int n;
  char *p;

  if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int sys_write(void) {
  struct file *f;
  int n;
  char *p;

  if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int64_t sys_close(void) {
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int64_t sys_stat(void) {
  struct file *f;
  struct stat *st;

  if (argfd(0, 0, &f) < 0 || argptr(1, (void *)&st, sizeof(*st)) < 0) {
    return -1;
  }
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int sys_link(void) {
  char name[NAMEBUFSZ], *new, *old;
  struct inode *dp, *ip;

  if (argstr(0, &old) < 0 || argstr(1, &new) < 0) {
    return -1;
  }

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((dp = nameiparent(new, name)) == 0) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  ilock(dp);
  if (!inode_access(dp, myproc()->uid, 3)) {
    iunlockput(dp); iunlockput(ip); end_op(); return -1;
  }
  if (ip->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(ip->dev);
    if (dp->fs_type != FS_UFS2 || dp->dev != ip->dev || volume == 0 ||
        ip->ufs2_info.nlink == 0 || ip->ufs2_info.nlink == 0xffff) {
      iunlockput(dp);
      iunlockput(ip);
      end_op();
      return -1;
    }
    ip->ufs2_info.nlink++;
    ip->nlink = ip->ufs2_info.nlink;
    if (ufs2_write_inode(volume, ip->inum, &ip->ufs2_info) < 0 ||
        ufs2_dirlink(volume, dp->inum, name, ip->inum, UFS2_DIRTYPE_REG) < 0) {
      ip->ufs2_info.nlink--;
      ip->nlink = ip->ufs2_info.nlink;
      ufs2_write_inode(volume, ip->inum, &ip->ufs2_info);
      iunlockput(dp);
      iunlockput(ip);
      end_op();
      return -1;
    }
    iunlockput(dp);
    iunlockput(ip);
    end_op();
    return 0;
  }
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int isdirempty(struct inode *dp) {
  uint off;
  struct dirent de;

  if (dp->fs_type == FS_FAT32) {
    return fat32_isdirempty(dp);
  }
  if (dp->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(dp->dev);
    char name[NAMEBUFSZ];
    uint inum, index;
    if (volume == 0)
      return 0;
    for (index = 0;
         ufs2_readdir(volume, dp->inum, index, &inum, name, sizeof(name)) == 0;
         index++)
      if (namecmp(name, ".") != 0 && namecmp(name, "..") != 0)
        return 0;
    return 1;
  }

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
      panic("isdirempty: readi");
    }
    if (de.inum != 0) {
      return 0;
    }
  }
  return 1;
}

int sys_unlink(void) {
  struct inode *ip, *dp;
  struct dirent de;
  char name[NAMEBUFSZ], *path;
  uint off;

  if (argstr(0, &path) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  if (!inode_access(dp, myproc()->uid, 3))
    goto bad;

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0) {
    goto bad;
  }

  if ((ip = dirlookup(dp, name, &off)) == 0) {
    goto bad;
  }
  ilock(ip);

  if (dp->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(dp->dev);
    uint removed;
    if (volume == 0 || ip->fs_type != FS_UFS2 ||
        (ip->type == T_DIR && !isdirempty(ip)) ||
        ufs2_dirunlink(volume, dp->inum, name, &removed) < 0 ||
        removed != ip->inum || ip->ufs2_info.nlink == 0) {
      iunlockput(ip);
      goto bad;
    }
    if (ip->type == T_DIR) {
      struct ufs2_inode parent;
      if (ufs2_read_inode(volume, dp->inum, &parent) < 0 || parent.nlink < 1) {
        iunlockput(ip);
        goto bad;
      }
      parent.nlink--;
      dp->ufs2_info = parent;
      dp->nlink = parent.nlink;
      if (ufs2_write_inode(volume, dp->inum, &parent) < 0) {
        iunlockput(ip);
        goto bad;
      }
    }
    ip->ufs2_info.nlink--;
    ip->nlink = ip->ufs2_info.nlink;
    if (ufs2_write_inode(volume, ip->inum, &ip->ufs2_info) < 0) {
      iunlockput(ip);
      goto bad;
    }
    iunlockput(dp);
    iunlockput(ip);
    end_op();
    return 0;
  }

  if (ip->nlink < 1) {
    panic("unlink: nlink < 1");
  }
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  if (dp->fs_type == FS_FAT32) {
    if (fat32_unlink(dp, off) < 0) {
      goto bad;
    }
  } else {
    memset(&de, 0, sizeof(de));
    if (writei(dp, (char *)&de, off, sizeof(de)) != sizeof(de)) {
      panic("unlink: writei");
    }
  }
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode *create(char *path, short type, short major, short minor) {
  struct inode *ip, *dp;
  char name[NAMEBUFSZ];

  if ((dp = nameiparent(path, name)) == 0) {
    return 0;
  }
  ilock(dp);
  if (!inode_access(dp, myproc()->uid, 3)) {
    iunlockput(dp);
    return 0;
  }

  if (dp->fs_type == FS_UFS2) {
    const struct ufs2_volume *volume = storage_ufs2_volume(dp->dev);
    uint inum = 0;
    uint16_t mode;
    uchar directory_type;

    if ((ip = dirlookup(dp, name, 0)) != 0) {
      iunlockput(dp);
      ilock(ip);
      if (type == T_FILE && ip->type == T_FILE)
        return ip;
      iunlockput(ip);
      return 0;
    }
    if (type == T_FILE) {
      mode = 0100644;
      directory_type = UFS2_DIRTYPE_REG;
    } else if (type == T_DIR) {
      mode = 0040755;
      directory_type = UFS2_DIRTYPE_DIR;
    } else {
      iunlockput(dp);
      return 0;
    }
    if (volume == 0 || ufs2_alloc_inode(volume, mode, &inum) < 0 ||
        (type == T_DIR && ufs2_make_directory(volume, inum, dp->inum) < 0) ||
        ufs2_dirlink(volume, dp->inum, name, inum, directory_type) < 0) {
      if (volume != 0 && inum >= UFS2_ROOT_INO)
        ufs2_free_inode(volume, inum);
      iunlockput(dp);
      return 0;
    }
    if (type == T_DIR) {
      struct ufs2_inode parent;
      if (ufs2_read_inode(volume, dp->inum, &parent) == 0) {
        parent.nlink++;
        dp->ufs2_info = parent;
        dp->nlink = parent.nlink;
        ufs2_write_inode(volume, dp->inum, &parent);
      }
    }
    ip = iget(dp->dev, inum);
    iunlockput(dp);
    ilock(ip);
    return ip;
  }

  if (dp->fs_type == FS_FAT32) {
      struct fat32_dirent de;
      uint off;
      uint cluster = fat32_lookup(dp->dev, dp->fat32_info.cluster, name, &de, &off);
      if (cluster != 0 || de.name[0] != 0) { // Found
          iunlockput(dp);
          uint inum = (cluster == 0) ? (0x80000000 | (dp->fat32_info.cluster << 12) | (off / 32)) : cluster;
          ip = iget(dp->dev, inum);
          ilock(ip);
          if (type == T_FILE && ip->type == T_FILE) return ip;
          iunlockput(ip);
          return 0;
      }
      
      ip = fat32_ialloc(dp->dev, type);
       ilock(ip);
       if (fat32_dirlink(dp, name, ip) < 0) panic("fat32 create dirlink");
       
       uint ip_cluster = ip->fat32_info.cluster;
        uint inum;
        if (ip_cluster == 0) {
            inum = 0x80000000 | (ip->fat32_info.parent_cluster << 12) | (ip->fat32_info.dir_off / 32);
        } else {
            inum = ip_cluster;
        }
        iupdate_inum(ip, inum);
       
       iunlockput(dp);
       return ip;
   }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && ip->type == T_FILE) {
      return ip;
    }
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    panic("create: ialloc");
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  ip->uid = myproc()->uid;
  ip->gid = myproc()->gid;
  ip->mode = type == T_DIR ? 0755 : (type == T_DEVICE ? 0600 : 0644);
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    dp->nlink++;       // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0) {
      panic("create dots");
    }
  }

  if (dirlink(dp, name, ip->inum) < 0) {
    panic("create: dirlink");
  }

  iunlockput(dp);

  return ip;
}

int64_t sys_open(void) {
  char *path;
  int fd, omode;
  size_t sz, off;
  struct file *f;
  struct inode *ip;

  if (argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  // The supervisor command endpoint changes system-wide state.  It must not
  // be writable by a regular account merely because devfs has no on-disk ACL.
  if (((omode & O_WRONLY) || (omode & O_RDWR)) && myproc()->uid != 0 &&
      ((strncmp(path, "/fnusvc.command", 16) == 0 && path[16] == 0) ||
       (strncmp(path, "fnusvc.command", 15) == 0 && path[15] == 0)))
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
  if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    if (((omode & O_WRONLY) || (omode & O_RDWR)) &&
        !inode_access(ip, myproc()->uid, 2)) {
      iunlockput(ip);
      end_op();
      return -1;
    }
    if (!(omode & O_WRONLY) && !(omode & O_RDWR) &&
        !inode_access(ip, myproc()->uid, 4)) {
      iunlockput(ip);
      end_op();
      return -1;
    }
    if (ip->type != T_FILE && ip->type != T_DIR && ip->type != T_DEVICE) {
      iunlockput(ip);
      end_op();
      return -1;
    }
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if ((omode & O_TRUNC) && ip->fs_type == FS_UFS2) {
    if (ufs2_truncate(storage_ufs2_volume(ip->dev), ip->inum,
                      &ip->ufs2_info) < 0) {
      iunlockput(ip);
      end_op();
      return -1;
    }
    ip->size = 0;
  }
  sz = (size_t)ip->size;

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f) {
      fileclose(f);
    }
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = (omode & O_APPEND) ? sz : 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int sys_mkdir(void) {
  char *path;
  struct inode *ip;

  begin_op();
  if (argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int sys_mknod(void) {
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if ((argstr(0, &path)) < 0 || argint(1, &major) < 0 ||
      argint(2, &minor) < 0 || (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int sys_chdir(void) {
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();

  begin_op();
  if (argstr(0, &path) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  if (!inode_access(ip, curproc->uid, 1)) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int64_t sys_getcwd(void) {
  char path[MAXPATHLEN];
  char *buf;
  int size;

  if (argptr(0, &buf, 1) < 0 || argint(1, &size) < 0 || size < 2)
    return -1;
  if (size > MAXPATHLEN)
    size = MAXPATHLEN;
  begin_op();
  if (inode_path(myproc()->cwd, path, (uint)size) < 0) {
    end_op();
    return -1;
  }
  end_op();
  if (copyout(myproc()->pgdir, (uintptr_t)buf, path, strlen(path) + 1) < 0)
    return -1;
  return 0;
}

int64_t sys_chmod(void) {
  char *path; int mode; struct inode *ip;
  if (argstr(0, &path) < 0 || argint(1, &mode) < 0 || (mode & ~07777)) return -1;
  begin_op();
  if ((ip = namei(path)) == 0) { end_op(); return -1; }
  ilock(ip);
  if (myproc()->uid != 0 && myproc()->uid != ip->uid) {
    iunlockput(ip); end_op(); return -1;
  }
  ip->mode = mode & 07777;
  if (ip->fs_type == FS_UFS2) {
    ip->ufs2_info.mode = (ip->ufs2_info.mode & 0170000) | ip->mode;
    if (ufs2_write_inode(storage_ufs2_volume(ip->dev), ip->inum,
                         &ip->ufs2_info) < 0) { iunlockput(ip); end_op(); return -1; }
  } else iupdate(ip);
  iunlockput(ip); end_op(); return 0;
}

int64_t sys_chown(void) {
  char *path; int uid, gid; struct inode *ip;
  if (myproc()->uid != 0 || argstr(0, &path) < 0 ||
      argint(1, &uid) < 0 || argint(2, &gid) < 0 || uid < 0 || gid < 0) return -1;
  begin_op();
  if ((ip = namei(path)) == 0) { end_op(); return -1; }
  ilock(ip); ip->uid = uid; ip->gid = gid;
  if (ip->fs_type == FS_UFS2) {
    ip->ufs2_info.uid = uid; ip->ufs2_info.gid = gid;
    if (ufs2_write_inode(storage_ufs2_volume(ip->dev), ip->inum,
                         &ip->ufs2_info) < 0) { iunlockput(ip); end_op(); return -1; }
  } else iupdate(ip);
  iunlockput(ip); end_op(); return 0;
}

int64_t sys_exec(void) {
  char *path, *argv[MAXARG];
  int i;
  uint64_t uargv, uarg;

  if (argstr(0, &path) < 0 || arg(1, &uargv) < 0) {
    return -1;
  }

  memset(argv, 0, sizeof(argv));

  for (i = 0;; i++) {
    if (i >= MAXARG) {
      return -1;
    }
    if (fetchint(uargv + sizeof(uintptr_t) * i, (uint64_t *)&uarg) < 0) {
      return -1;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    if (fetchstr(uarg, &argv[i]) < 0) {
      return -1;
    }
  }

  return exec(path, argv);
}

int sys_pipe(void) {
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if (argptr(0, (void *)&fd, 2 * sizeof(fd[0])) < 0) {
    return -1;
  }
  if (pipealloc(&rf, &wf) < 0) {
    return -1;
  }
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0) {
      myproc()->ofile[fd0] = 0;
    }
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}
