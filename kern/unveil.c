// UNVEIL(2)
#include "defs.h"
#include "file.h"
#include "fs.h"
#include "inc/dir.h"
#include "inc/error.h"
#include "inc/stat.h"
#include "malloc.h"
#include "param.h"
#include "pathname.h"
#include "proc.h"
#include "unveil.h"
#include "vfs.h"

struct unvname {
  struct unvname *next;
  uchar flags;
  char name[];
};

struct unveil {
  struct inode *dir;
  int cover;
  uchar flags;
  struct unvname *names;
};

static struct unvname *name_find(struct unveil *uv, const char *name) {
  struct unvname *n;

  for (n = __atomic_load_n(&uv->names, __ATOMIC_ACQUIRE); n != 0; n = n->next)
    if (strcmp(n->name, name) == 0)
      return n;
  return 0;
}

static int entry_of(struct process *pr, struct inode *dp) {
  int i, n = __atomic_load_n(&pr->uvcount, __ATOMIC_ACQUIRE);

  for (i = 0; i < n; i++)
    if (pr->uv[i].dir == dp)
      return i;
  return -1;
}

static struct unveil *lookup(struct process *pr, struct inode *dp) {
  int i = entry_of(pr, dp);

  return i < 0 ? 0 : &pr->uv[i];
}

static int is_root(struct inode *ip) {
  const struct vfs_mount *r = vfs_root();

  return r != 0 && ip->dev == r->dev && ip->inum == r->root_inum;
}

static int find_cover(struct process *pr, struct inode *dp) {
  struct inode *ip = idup(dp), *parent;
  int depth, i = -1;

  for (depth = 0; depth < MAXPATHDEPTH && !is_root(ip); depth++) {
    if ((parent = inode_dotdot(ip)) == 0)
      break;
    if (parent == ip) {
      iput(parent);
      break;
    }
    iput(ip);
    ip = parent;
    if ((i = entry_of(pr, ip)) >= 0)
      break;
  }
  iput(ip);
  return i;
}

static int parse(const char *s, uchar *flags) {
  *flags = UV_USERSET;
  for (; *s != 0; s++)
    switch (*s) {
    case 'r': *flags |= UV_READ; break;
    case 'w': *flags |= UV_WRITE; break;
    case 'x': *flags |= UV_EXEC; break;
    case 'c': *flags |= UV_CREATE; break;
    default: return -1;
    }
  return 0;
}

static struct unveil *add_dir(struct process *pr, struct inode *dp) {
  struct unveil *uv = &pr->uv[pr->uvcount];
  int i;

  uv->dir = dp;
  uv->flags = 0;
  uv->names = 0;
  uv->cover = find_cover(pr, dp);
  __atomic_store_n(&pr->uvcount, pr->uvcount + 1, __ATOMIC_RELEASE);
  for (i = 0; i < pr->uvcount - 1; i++)
    if (pr->uv[i].cover == uv->cover)
      pr->uv[i].cover = find_cover(pr, pr->uv[i].dir);
  return uv;
}

static int may_touch(struct inode *ip) {
  uint uid = curproc->p_p->uid;
  int ok;

  ilock(ip);
  ok = inode_access(ip, uid, 4) || inode_access(ip, uid, 2) ||
       inode_access(ip, uid, 1);
  iunlock(ip);
  return ok;
}

static int resolve(char *path, struct inode **dirp, struct inode **dp, char *name) {
  struct inode *parent, *ip;
  char target[MAXPATHLEN];
  int error, n, links;

  *dirp = 0;
  *dp = 0;
  for (links = 0;; links++) {
    if ((error = nameiparent_at(0, path, name, UV_NONE, &parent)) != 0) {
      if (error != FNU_ERR_ISDIR)
        return error;
      if ((error = namei_at(0, path, UV_NONE, &ip)) != 0)
        return error;
      *dirp = ip;
      return 0;
    }
    ilock(parent);
    ip = dirlookup(parent, name);
    iunlock(parent);
    ip = mount_enter(ip);
    if (ip == 0) {
      *dp = parent;
      return 0;
    }
    ilock(ip);
    if (ip->type == FNU_TYPE_LINK && links < MAXSYMLINKS &&
        (n = readlinki(ip, target, sizeof(target) - 1)) > 0) {
      iunlockput(ip);
      target[n] = 0;
      if (target[0] != '/') {
        char full[MAXPATHLEN];

        char where[MAXPATHLEN];

        if (inode_path(parent, where, sizeof(where)) < 0 ||
            snprintf(full, sizeof(full), "%s/%s", where, target) >= (int)sizeof(full)) {
          iput(parent);
          return FNU_ERR_NAMETOOLONG;
        }
        strlcpy(target, full, sizeof(target));
      }
      iput(parent);
      strlcpy(path, target, MAXPATHLEN);
      continue;
    }
    if (ip->type == FNU_TYPE_DIR) {
      iunlock(ip);
      iput(parent);
      *dirp = ip;
      return 0;
    }
    iunlockput(ip);
    *dp = parent;
    return 0;
  }
}

int unveil_add(struct proc *p, char *path, const char *perms) {
  struct process *pr = p->p_p;
  struct inode *dir, *dp, *held;
  struct unveil *uv;
  struct unvname *n;
  char name[NAMEBUFSZ];
  uchar flags;
  int error;

  if (parse(perms, &flags) < 0)
    return FNU_ERR_INVALID;
  if ((error = resolve(path, &dir, &dp, name)) != 0)
    return error;
  held = dir != 0 ? dir : dp;
  if (!may_touch(held)) {
    iput(held);
    return FNU_ERR_NOTPERMITTED;
  }
  acquiresleep(&pr->uvlock);
  if (pr->uv == 0)
    pr->uv = malloc(UNVEIL_MAX_DIRS * sizeof(*pr->uv), M_PROC, M_WAITOK | M_ZERO);
  if ((uv = lookup(pr, held)) != 0) {
    iput(held);
    if (dir != 0) {
      uv->flags = flags;
      goto out;
    }
    if ((n = name_find(uv, name)) != 0) {
      n->flags = flags;
      goto out;
    }
  } else if (pr->uvcount >= UNVEIL_MAX_DIRS || pr->uvnames >= UNVEIL_MAX_NAMES) {
    iput(held);
    error = FNU_ERR_ARGSTOOLONG;
    goto out;
  } else {
    uv = add_dir(pr, held);
    if (dir != 0) {
      uv->flags = flags;
      goto out;
    }
  }
  if (pr->uvnames >= UNVEIL_MAX_NAMES) {
    error = FNU_ERR_ARGSTOOLONG;
    goto out;
  }
  n = malloc(sizeof(*n) + strlen(name) + 1, M_PROC, M_WAITOK);
  n->flags = flags;
  strlcpy(n->name, name, strlen(name) + 1);
  n->next = uv->names;
  __atomic_store_n(&uv->names, n, __ATOMIC_RELEASE);
  pr->uvnames++;
out:
  releasesleep(&pr->uvlock);
  return error;
}

void unveil_destroy(struct process *pr) {
  struct unvname *n, *next;
  int i;

  for (i = 0; i < pr->uvcount; i++) {
    for (n = pr->uv[i].names; n != 0; n = next) {
      next = n->next;
      free(n, M_PROC);
    }
    iput(pr->uv[i].dir);
  }
  if (pr->uv != 0)
    free(pr->uv, M_PROC);
  pr->uv = 0;
  pr->uvcount = 0;
  pr->uvnames = 0;
}

void unveil_copy(struct process *from, struct process *to) {
  struct unvname *n, *c;
  int i;

  to->uvdone = from->uvdone;
  to->uv = 0;
  to->uvcount = 0;
  to->uvnames = 0;
  if (from->uv == 0)
    return;
  to->uv = malloc(UNVEIL_MAX_DIRS * sizeof(*to->uv), M_PROC, M_WAITOK | M_ZERO);
  acquiresleep(&from->uvlock);
  for (i = 0; i < from->uvcount; i++) {
    to->uv[i].dir = idup(from->uv[i].dir);
    to->uv[i].cover = from->uv[i].cover;
    to->uv[i].flags = from->uv[i].flags;
    for (n = from->uv[i].names; n != 0; n = n->next) {
      c = malloc(sizeof(*c) + strlen(n->name) + 1, M_PROC, M_WAITOK);
      c->flags = n->flags;
      strlcpy(c->name, n->name, strlen(n->name) + 1);
      c->next = to->uv[i].names;
      to->uv[i].names = c;
      to->uvnames++;
    }
  }
  to->uvcount = from->uvcount;
  releasesleep(&from->uvlock);
}

int unveil_active(struct process *pr) {
  return __atomic_load_n(&pr->uv, __ATOMIC_ACQUIRE) != 0;
}

static int flagmatch(int want, uchar flags) {
  if (flags == 0)
    return 0;
  return (want & UV_MASK & ~flags) == 0;
}

static struct unveil *covered(struct process *pr, struct unveil *uv,
                              struct inode *dp) {
  if (uv == 0 || uv->dir != dp)
    return uv;
  if (is_root(dp))
    return uv;
  return uv->cover >= 0 ? &pr->uv[uv->cover] : 0;
}

void unveil_start(struct lookup_unveil *lu, struct inode *dp) {
  struct process *pr = curproc->p_p;
  int i;

  lu->match = 0;
  if (lu->want == UV_NONE || !unveil_active(pr))
    return;
  if ((lu->match = lookup(pr, dp)) == 0 && (i = find_cover(pr, dp)) >= 0)
    lu->match = &pr->uv[i];
}

void unveil_component(struct lookup_unveil *lu, struct inode *dp, int dotdot) {
  struct process *pr = curproc->p_p;
  struct unveil *uv;

  if (lu->want == UV_NONE || !unveil_active(pr))
    return;
  if (dotdot) {
    uv = covered(pr, lu->match, dp);
    if (lu->match != 0 && lu->match->dir == dp)
      lu->match = 0;
  } else
    uv = lookup(pr, dp);
  if (uv != 0)
    lu->match = uv;
}

static int refused(struct unveil *uv) {
  return (uv->flags & UV_MASK) ? FNU_ERR_DENIED : FNU_ERR_NOTFOUND;
}

int unveil_final(struct lookup_unveil *lu, struct inode *ip, struct inode *dp,
                 const char *name) {
  struct process *pr = curproc->p_p;
  struct unveil *uv, *next;
  struct unvname *n;

  if (lu->want == UV_NONE || !unveil_active(pr))
    return 0;
  if (ip != 0 && ip->type == FNU_TYPE_DIR) {
    uv = lookup(pr, ip);
    if (uv == 0 || !(uv->flags & UV_USERSET)) {
      if (uv != 0)
        lu->match = uv;
      goto chain;
    }
    return flagmatch(lu->want, uv->flags) ? 0 : refused(uv);
  }
  if (dp == 0 || (uv = lookup(pr, dp)) == 0)
    goto chain;
  if ((n = name_find(uv, name)) == 0) {
    if (flagmatch(lu->want, uv->flags))
      return 0;
    if (uv->flags & UV_USERSET)
      return refused(uv);
    lu->match = uv;
    goto chain;
  }
  return flagmatch(lu->want, n->flags) ? 0 : FNU_ERR_DENIED;

chain:
  for (uv = lu->match; uv != 0; uv = next) {
    if (flagmatch(lu->want, uv->flags))
      return 0;
    if (uv->flags & UV_USERSET)
      return refused(uv);
    if ((next = covered(pr, uv, uv->dir)) == uv)
      break;
  }
  return FNU_ERR_NOTFOUND;
}

int sys_unveil(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct process *pr = p->p_p;
  struct pathname path;
  char perms[5];
  int error;

  if (uap[0] == 0 && uap[1] == 0) {
    pr->uvdone = 1;
    return 0;
  }
  if (pr->uvdone)
    return FNU_ERR_NOTPERMITTED;
  if (uap[0] == 0 || uap[1] == 0)
    return FNU_ERR_INVALID;
  if ((error = copyinstr(pr->pgdir, perms, (uintptr_t)uap[1], sizeof(perms), 0)) != 0)
    return error;
  if ((error = pn_get((uintptr_t)uap[0], &path)) != 0)
    return error;
  if (path.pn_buf[0] == 0)
    error = FNU_ERR_INVALID;
  else
    error = unveil_add(p, path.pn_buf, perms);
  pn_free(&path);
  return error;
}
