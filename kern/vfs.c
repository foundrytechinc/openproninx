// VFS

#include "defs.h"
#include "file.h"
#include "fs.h"
#include "mutex.h"
#include "blockdev.h"
#include "devfs.h"
#include "malloc.h"
#include "inc/stat.h"
#include "vfs.h"

#define VFS_GROW 8

static struct {
  struct mutex lock;
  struct vfs_mount **m;
  int n;
} mtab;

void vfs_init(void) { mtx_init(&mtab.lock, "vfs"); }

int vfs_slots(void) {
  int n;

  mtx_enter(&mtab.lock);
  n = mtab.n;
  mtx_leave(&mtab.lock);
  return n;
}

// VFS_GROW
static void grow(void) {
  struct vfs_mount **v, **old = 0, *slab;
  int i, n;

  n = vfs_slots();
  v = malloc((size_t)(n + VFS_GROW) * sizeof(*v), M_MOUNT, M_WAITOK);
  slab = malloc(VFS_GROW * sizeof(*slab), M_MOUNT, M_WAITOK | M_ZERO);
  mtx_enter(&mtab.lock);
  if (mtab.n != n) {
    mtx_leave(&mtab.lock);
    free(v, M_MOUNT);
    free(slab, M_MOUNT);
    return;
  }
  for (i = 0; i < n; i++)
    v[i] = mtab.m[i];
  for (i = 0; i < VFS_GROW; i++)
    v[n + i] = &slab[i];
  old = mtab.m;
  mtab.m = v;
  mtab.n = n + VFS_GROW;
  mtx_leave(&mtab.lock);
  if (old != 0)
    free(old, M_MOUNT);
}

int vfs_mount_add(struct vfs_mount *m) {
  struct vfs_mount *e;
  int i;

  if (m == 0 || m->dev == 0)
    return -1;
  for (;;) {
    mtx_enter(&mtab.lock);
    for (i = 0; i < mtab.n; i++) {
      e = mtab.m[i];
      if (!e->used)
        continue;
      if (e->dev == m->dev ||
          (m->cover_inum != 0 && e->cover_dev == m->cover_dev &&
           e->cover_inum == m->cover_inum)) {
        mtx_leave(&mtab.lock);
        return -1;
      }
    }
    for (i = 0; i < mtab.n; i++)
      if (!mtab.m[i]->used)
        break;
    if (i < mtab.n)
      break;
    mtx_leave(&mtab.lock);
    grow();
  }
  e = mtab.m[i];
  *e = *m;
  e->used = 1;
  mtx_leave(&mtab.lock);
  if (e->op->start != 0)
    e->op->start(e);
  return 0;
}

static void vfs_letgo(struct vfs_mount *m) {
  if (m->op->unmount != 0)
    m->op->unmount(m);
}

int vfs_unmount_at(uint cover_dev, uint cover_inum) {
  int i, found = -1;

  if (cover_inum == 0)
    return -1;
  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++)
    if (mtab.m[i]->used && mtab.m[i]->cover_dev == cover_dev &&
        mtab.m[i]->cover_inum == cover_inum) {
      struct vfs_mount *e = mtab.m[i];

      mtx_leave(&mtab.lock);
      vfs_letgo(e);
      mtx_enter(&mtab.lock);
      e->used = 0;
      found = 0;
      mtx_leave(&mtab.lock);
      blockdev_released(e->dev);
      return found;
    }
  mtx_leave(&mtab.lock);
  return found;
}

// a device mounted, or one whose partition is
int vfs_dev_mounted(uint dev) {
  int i, r = 0;

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n && !r; i++)
    if (mtab.m[i]->used && mtab.m[i]->dev == dev)
      r = 1;
  mtx_leave(&mtab.lock);
  return r;
}

const struct vfs_mount *vfs_covering(uint dev, uint inum) {
  const struct vfs_mount *found = 0;
  int i;

  if (inum == 0)
    return 0;
  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++)
    if (mtab.m[i]->used && mtab.m[i]->cover_inum == inum &&
        mtab.m[i]->cover_dev == dev) {
      found = mtab.m[i];
      break;
    }
  mtx_leave(&mtab.lock);
  return found;
}

const struct vfs_mount *vfs_rooted_at(uint dev, uint inum) {
  const struct vfs_mount *found = 0;
  int i;

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++)
    if (mtab.m[i]->used && mtab.m[i]->dev == dev &&
        mtab.m[i]->root_inum == inum && mtab.m[i]->cover_inum != 0) {
      found = mtab.m[i];
      break;
    }
  mtx_leave(&mtab.lock);
  return found;
}

const struct vfs_mount *vfs_root(void) {
  const struct vfs_mount *found = 0;
  int i;

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++)
    if (mtab.m[i]->used && mtab.m[i]->cover_inum == 0) {
      found = mtab.m[i];
      break;
    }
  mtx_leave(&mtab.lock);
  return found;
}

int vfs_sync_dev(uint dev) {
  const struct vfs_mount *m = vfs_on_dev(dev);
  int written;

  if (m != 0 && !m->readonly && m->op->sync != 0)
    m->op->sync(m);
  written = bio_flush_device(dev);
  blockdev_flush(dev);
  return written;
}

int vfs_sync(void) {
  int i;

  for (i = 0; i < vfs_slots(); i++) {
    struct vfs_mount *e;

    mtx_enter(&mtab.lock);
    e = mtab.m[i];
    if (!e->used || e->readonly || e->op->sync == 0) {
      mtx_leave(&mtab.lock);
      continue;
    }
    mtx_leave(&mtab.lock);
    e->op->sync(e);
  }
  return bio_flush();
}

void vfs_dismount_all(void) {
  int i;

  for (i = 0; i < vfs_slots(); i++) {
    struct vfs_mount *e;

    mtx_enter(&mtab.lock);
    e = mtab.m[i];
    if (!e->used || e->readonly) {
      mtx_leave(&mtab.lock);
      continue;
    }
    mtx_leave(&mtab.lock);
    vfs_letgo(e);
  }
  bio_flush();
}

// MNT_UPDATE: rw and nosuid of a mounted volume
int vfs_update(uint dev, int flags) {
  struct vfs_mount *e = 0;
  int i, ro = (flags & FNU_MNT_RDONLY) != 0, r = 0;

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++)
    if (mtab.m[i]->used && mtab.m[i]->dev == dev)
      e = mtab.m[i];
  mtx_leave(&mtab.lock);
  if (e == 0)
    return FNU_ERR_INVALID;
  if (ro != e->readonly) {
    if (e->op->update == 0)
      return FNU_ERR_UNSUPPORTED;
    if (ro) {
      if (!(flags & FNU_MNT_FORCE) && fs_dev_writers(dev))
        return FNU_ERR_BUSY;
      vfs_sync_dev(dev);
      e->readonly = 1;
      if (e->op->update(e, FNU_MNT_RDONLY) < 0) {
        e->readonly = 0;
        r = FNU_ERR_BUSY;
      }
    } else if (e->op->update(e, 0) < 0) {
      r = FNU_ERR_IO;
    } else {
      e->readonly = 0;
    }
  }
  if (r == 0)
    e->nosuid = (flags & FNU_MNT_NOSUID) != 0;
  return r;
}

// nosuid, asked from exec
int vfs_nosuid(uint dev) {
  const struct vfs_mount *m = vfs_on_dev(dev);

  return m != 0 && m->nosuid;
}

int vfs_readonly(uint dev) {
  const struct vfs_mount *m = vfs_on_dev(dev);

  return m != 0 && m->readonly;
}

const struct vfs_mount *vfs_on_dev(uint dev) {
  const struct vfs_mount *found = 0;
  int i;

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++)
    if (mtab.m[i]->used && mtab.m[i]->dev == dev) {
      found = mtab.m[i];
      break;
    }
  mtx_leave(&mtab.lock);
  return found;
}

const struct ufs2_volume *vfs_ufs2_volume(uint dev) {
  const struct vfs_mount *m = vfs_on_dev(dev);

  if (m == 0 || m->op != &ufs2_vfsops)
    return 0;
  return &m->ufs2;
}

const struct vnodeops *vfs_vnops(uint dev) {
  const struct vfs_mount *m;

  if (dev == DEVFS_DEV)
    return devfs_vfsops.vnops;
  m = vfs_on_dev(dev);
  return m != 0 ? m->op->vnops : &dead_vnops;
}

const struct vfs_mount *vfs_nth(int i) {
  const struct vfs_mount *found = 0;

  mtx_enter(&mtab.lock);
  if (i >= 0 && i < mtab.n && mtab.m[i]->used)
    found = mtab.m[i];
  mtx_leave(&mtab.lock);
  return found;
}

// BOOT TIME FILESYSTEMS

static int cover_of(const char *path, uint *dev, uint *inum) {
  struct inode *ip;

  if (namei((char *)path, &ip) != 0)
    return -1;
  ilock(ip);
  if (ip->type != FNU_TYPE_DIR) {
    iunlockput(ip);
    return -1;
  }
  *dev = ip->dev;
  *inum = ip->inum;
  iunlockput(ip);
  return 0;
}

void vfs_attach_boot(void) {
  struct vfs_mount m;
  uint dev, inum;
  int i;

  blockdev_publish_all();

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++) {
    struct vfs_mount *e = mtab.m[i];
    char name[VFS_NAMELEN];

    if (!e->used || e->on[0] != '/' || e->on[1] != 0)
      continue;
    if (blockdev_name(e->dev, name, sizeof(name)) == 0) {
      strlcpy(e->from, "/dev/", VFS_NAMELEN);
      strlcpy(e->from + 5, name, VFS_NAMELEN - 5);
    }
  }
  mtx_leave(&mtab.lock);

  if (cover_of("/dev", &dev, &inum) == 0) {
    memset(&m, 0, sizeof(m));
    m.dev = DEVFS_DEV;
    m.op = &devfs_vfsops;
    m.root_inum = DEVFS_ROOT_INO;
    m.cover_dev = dev;
    m.cover_inum = inum;
    strlcpy(m.from, "devfs", VFS_NAMELEN);
    strlcpy(m.on, "/dev", VFS_NAMELEN);
    strlcpy(m.type, m.op->name, VFS_TYPELEN);
    if (vfs_mount_add(&m) < 0)
      printf("vfs: cannot mount devfs on /dev\n");
  } else {
    printf("vfs: no /dev directory to mount the devices on\n");
  }

  mtx_enter(&mtab.lock);
  for (i = 0; i < mtab.n; i++) {
    struct vfs_mount *e = mtab.m[i];

    if (!e->used || e->cover_inum != 0 || e->on[0] != '/' || e->on[1] == 0)
      continue;
    mtx_leave(&mtab.lock);
    if (cover_of(e->on, &dev, &inum) == 0) {
      mtx_enter(&mtab.lock);
      e->cover_dev = dev;
      e->cover_inum = inum;
    } else {
      printf("vfs: %s has nowhere to attach\n", e->on);
      mtx_enter(&mtab.lock);
    }
  }
  mtx_leave(&mtab.lock);
}
