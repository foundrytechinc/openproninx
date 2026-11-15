// DEVFS

#include "defs.h"
#include "devfs.h"
#include "inc/stat.h"
#include "malloc.h"
#include "mutex.h"

static struct {
  struct mutex lock;
  struct devfs_node *node;
  int cap;
  uint next_inum;
  int ready;
} devfs;

void devfs_init(void) {
  mtx_init(&devfs.lock, "devfs");
  devfs.next_inum = DEVFS_ROOT_INO + 1;
  devfs.ready = 1;
}

// call with the lock held
static int grow(void) {
  int cap = devfs.cap ? devfs.cap * 2 : 64;
  struct devfs_node *n = malloc((size_t)cap * sizeof(*n), M_DEVBUF, M_NOWAIT | M_ZERO);

  if (n == 0)
    return -1;
  if (devfs.node != 0) {
    memmove(n, devfs.node, (size_t)devfs.cap * sizeof(*n));
    free(devfs.node, M_DEVBUF);
  }
  devfs.node = n;
  devfs.cap = cap;
  return 0;
}

static int name_is(const struct devfs_node *n, const char *p, int len) {
  int i;

  if (len > DEVFS_NAME_MAX)
    return 0;
  for (i = 0; i < len; i++)
    if (n->name[i] != p[i])
      return 0;
  return n->name[len] == 0;
}

// call with the lock held
static struct devfs_node *find(uint dir, const char *p, int len) {
  int i;

  for (i = 0; i < devfs.cap; i++)
    if (devfs.node[i].used && devfs.node[i].parent == dir &&
        name_is(&devfs.node[i], p, len))
      return &devfs.node[i];
  return 0;
}

// call with the lock held
static struct devfs_node *alloc(uint dir, const char *p, int len, short type,
                                short major, short minor, ushort mode) {
  struct devfs_node *n;
  int i, k;

  if (len <= 0 || len > DEVFS_NAME_MAX)
    return 0;
  for (i = 0; i < devfs.cap && devfs.node[i].used; i++)
    ;
  if (i == devfs.cap && grow() < 0)
    return 0;
  n = &devfs.node[i];
  for (k = 0; k < len; k++)
    n->name[k] = p[k];
  n->name[len] = 0;
  n->inum = devfs.next_inum++;
  n->parent = dir;
  n->type = type;
  n->major = major;
  n->minor = minor;
  n->mode = mode;
  n->uid = 0;
  n->gid = 0;
  n->used = 1;
  n->born = uptime_ns();
  return n;
}

static uint add(const char *name, short type, short major, short minor,
                ushort mode, int alias) {
  struct devfs_node *n;
  const char *p, *slash;
  uint dir = DEVFS_ROOT_INO;
  uint inum = 0;

  if (!devfs.ready || name == 0 || name[0] == 0)
    return 0;

  mtx_enter(&devfs.lock);
  p = name;
  for (;;) {
    for (slash = p; *slash != 0 && *slash != '/'; slash++)
      ;
    if (*slash == 0)
      break;
    n = find(dir, p, (int)(slash - p));
    if (n == 0)
      n = alloc(dir, p, (int)(slash - p), FNU_TYPE_DIR, 0, 0, 0555);
    if (n == 0 || n->type != FNU_TYPE_DIR) {
      mtx_leave(&devfs.lock);
      printf("devfs: cannot make the path above %s\n", name);
      return 0;
    }
    dir = n->inum;
    p = slash + 1;
  }

  n = find(dir, p, (int)strlen((char *)p));
  if (n != 0 && alias && (n->major != major || n->minor != minor)) {
    mtx_leave(&devfs.lock);
    return 0;
  }
  if (n != 0) {
    n->type = type;
    n->major = major;
    n->minor = minor;
    n->mode = mode;
    inum = n->inum;
  } else {
    n = alloc(dir, p, (int)strlen((char *)p), type, major, minor, mode);
    if (n != 0)
      inum = n->inum;
  }
  mtx_leave(&devfs.lock);

  if (inum == 0)
    printf("devfs: no room for %s\n", name);
  return inum;
}

uint devfs_add(const char *name, short type, short major, short minor,
               ushort mode) {
  return add(name, type, major, minor, mode, 0);
}

static void remove(const char *name, int alias, short major, short minor) {
  struct devfs_node *n;
  const char *p, *slash;
  uint dir = DEVFS_ROOT_INO;

  if (!devfs.ready || name == 0)
    return;
  mtx_enter(&devfs.lock);
  p = name;
  for (;;) {
    for (slash = p; *slash != 0 && *slash != '/'; slash++)
      ;
    if (*slash == 0)
      break;
    n = find(dir, p, (int)(slash - p));
    if (n == 0) {
      mtx_leave(&devfs.lock);
      return;
    }
    dir = n->inum;
    p = slash + 1;
  }
  n = find(dir, p, (int)strlen((char *)p));
  if (n != 0 && (!alias || (n->major == major && n->minor == minor)))
    n->used = 0;
  mtx_leave(&devfs.lock);
}

void devfs_remove(const char *name) { remove(name, 0, 0, 0); }

static int alias_path(char *path, int size, const char *dir,
                      const char *what) {
  int n = 0, i;

  if (dir == 0 || what == 0 || what[0] == 0)
    return -1;
  for (i = 0; dir[i] != 0 && n < size - 2; i++)
    path[n++] = dir[i];
  path[n++] = '/';
  for (i = 0; what[i] != 0 && n < size - 1; i++)
    path[n++] = what[i];
  path[n] = 0;
  return 0;
}

uint devfs_add_alias(const char *dir, const char *what, short type,
                     short major, short minor, ushort mode) {
  char path[DEVFS_NAME_MAX * 2 + 2];

  if (alias_path(path, sizeof(path), dir, what) < 0)
    return 0;
  return add(path, type, major, minor, mode, 1);
}

void devfs_remove_alias(const char *dir, const char *what, short major,
                        short minor) {
  char path[DEVFS_NAME_MAX * 2 + 2];

  if (alias_path(path, sizeof(path), dir, what) == 0)
    remove(path, 1, major, minor);
}

static int owned(const char *dir, short major, short minor, int drop) {
  struct devfs_node *d;
  int i, n = 0;

  if (!devfs.ready || dir == 0)
    return 0;
  mtx_enter(&devfs.lock);
  if ((d = find(DEVFS_ROOT_INO, dir, (int)strlen((char *)dir))) != 0)
    for (i = 0; i < devfs.cap; i++)
      if (devfs.node[i].used && devfs.node[i].parent == d->inum &&
          devfs.node[i].type != FNU_TYPE_DIR && devfs.node[i].major == major &&
          devfs.node[i].minor == minor) {
        n++;
        if (drop)
          devfs.node[i].used = 0;
      }
  mtx_leave(&devfs.lock);
  return n;
}

int devfs_has_alias(const char *dir, short major, short minor) {
  return owned(dir, major, minor, 0) > 0;
}

void devfs_remove_aliases(const char *dir, short major, short minor) {
  owned(dir, major, minor, 1);
}

int devfs_lookup(uint dir, const char *name, struct devfs_node *out) {
  struct devfs_node *n;

  if (!devfs.ready || name == 0)
    return -1;
  mtx_enter(&devfs.lock);
  n = find(dir, name, (int)strlen((char *)name));
  if (n != 0 && out != 0)
    *out = *n;
  mtx_leave(&devfs.lock);
  return n != 0 ? 0 : -1;
}

int devfs_by_inum(uint inum, struct devfs_node *out) {
  int i, found = -1;

  if (!devfs.ready)
    return -1;
  mtx_enter(&devfs.lock);
  for (i = 0; i < devfs.cap; i++)
    if (devfs.node[i].used && devfs.node[i].inum == inum) {
      if (out != 0)
        *out = devfs.node[i];
      found = 0;
      break;
    }
  mtx_leave(&devfs.lock);
  return found;
}

int devfs_nth(uint dir, uint index, struct devfs_node *out) {
  uint seen = 0;
  int i, found = -1;

  if (!devfs.ready)
    return -1;
  mtx_enter(&devfs.lock);
  for (i = 0; i < devfs.cap; i++)
    if (devfs.node[i].used && devfs.node[i].parent == dir) {
      if (seen == index) {
        if (out != 0)
          *out = devfs.node[i];
        found = 0;
        break;
      }
      seen++;
    }
  mtx_leave(&devfs.lock);
  return found;
}

int devfs_setattr(uint inum, ushort mode, uint uid, uint gid) {
  int i, found = -1;

  mtx_enter(&devfs.lock);
  for (i = 0; i < devfs.cap; i++)
    if (devfs.node[i].used && devfs.node[i].inum == inum) {
      devfs.node[i].mode = mode;
      devfs.node[i].uid = uid;
      devfs.node[i].gid = gid;
      found = 0;
      break;
    }
  mtx_leave(&devfs.lock);
  return found;
}

uint devfs_parent(uint inum) {
  struct devfs_node n;

  if (inum == DEVFS_ROOT_INO)
    return DEVFS_ROOT_INO;
  return devfs_by_inum(inum, &n) == 0 ? n.parent : DEVFS_ROOT_INO;
}
