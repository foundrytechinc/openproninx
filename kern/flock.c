// flock(2): advisory, whole file, held by the open file
#include "defs.h"
#include "file.h"
#include "fs.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"
#include "inc/abi.h"

static struct mutex flock_mtx = MUTEX_INITIALIZER("flock");

// flock_mtx held
static void release_locked(struct file *f) {
  struct inode *ip = f->ip;

  if (f->flocked == FNU_LOCK_SH && ip->flk_shared > 0)
    ip->flk_shared--;
  else if (f->flocked == FNU_LOCK_EX && ip->flk_excl == f)
    ip->flk_excl = 0;
  f->flocked = 0;
  wakeup(&ip->flk_shared);
}

void flock_release(struct file *f) {
  if (f->flocked == 0 || f->type != FD_INODE || f->ip == 0)
    return;
  mtx_enter(&flock_mtx);
  release_locked(f);
  mtx_leave(&flock_mtx);
}

int sys_flock(struct proc *p, const register_t *uap, register_t *retval __unused) {
  int op = (int)uap[1], want = op & (FNU_LOCK_SH | FNU_LOCK_EX), error = 0;
  struct inode *ip;
  struct file *f;

  if ((f = fd_getfile(p->p_p, (int)uap[0])) == 0)
    return FNU_ERR_BADHANDLE;
  if (f->type != FD_INODE || f->ip == 0) {
    FRELE(f);
    return FNU_ERR_UNSUPPORTED;
  }
  ip = f->ip;
  if (op & FNU_LOCK_UN) {
    flock_release(f);
    FRELE(f);
    return 0;
  }
  if (want != FNU_LOCK_SH && want != FNU_LOCK_EX) {
    FRELE(f);
    return FNU_ERR_INVALID;
  }
  mtx_enter(&flock_mtx);
  if (f->flocked == want)
    goto out;
  // a change of kind lets go first
  if (f->flocked != 0)
    release_locked(f);
  for (;;) {
    if (want == FNU_LOCK_SH && ip->flk_excl == 0) {
      ip->flk_shared++;
      f->flocked = FNU_LOCK_SH;
      break;
    }
    if (want == FNU_LOCK_EX && ip->flk_excl == 0 && ip->flk_shared == 0) {
      ip->flk_excl = f;
      f->flocked = FNU_LOCK_EX;
      break;
    }
    if (op & FNU_LOCK_NB) {
      error = FNU_ERR_WOULDBLOCK;
      break;
    }
    if ((error = msleep(&ip->flk_shared, &flock_mtx, PLOCK | PCATCH, "flock", 0)) != 0)
      break;
  }
out:
  mtx_leave(&flock_mtx);
  FRELE(f);
  return error;
}
