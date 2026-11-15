#include "defs.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "uio.h"
#include "uvm.h"

int uiomove(void *buf, size_t n, struct uio *uio) {
  char *cp = buf;

  while (n > 0 && uio->uio_resid > 0) {
    struct kiovec *iov = uio->uio_iov;
    size_t cnt = iov->iov_len;

    if (cnt == 0) {
      if (uio->uio_iovcnt <= 1)
        break;
      uio->uio_iov++;
      uio->uio_iovcnt--;
      continue;
    }
    if (cnt > n)
      cnt = n;
    if (uio->uio_segflg == UIO_USERSPACE) {
      pte_t *pgdir = uio->uio_procp->p_p->pgdir;

      if (uio->uio_rw == UIO_READ) {
        if (copyout(pgdir, (uintptr_t)iov->iov_base, cp, cnt) < 0)
          return FNU_ERR_FAULT;
      } else {
        if (copyin(pgdir, cp, (uintptr_t)iov->iov_base, cnt) < 0)
          return FNU_ERR_FAULT;
      }
    } else if (uio->uio_rw == UIO_READ) {
      memmove(iov->iov_base, cp, cnt);
    } else {
      memmove(cp, iov->iov_base, cnt);
    }
    iov->iov_base += cnt;
    iov->iov_len -= cnt;
    uio->uio_resid -= cnt;
    uio->uio_offset += cnt;
    cp += cnt;
    n -= cnt;
  }
  return 0;
}

void uio_prepare(struct uio *uio, struct kiovec *iov, void *base, size_t len,
                 uint64_t off, enum uio_seg seg, enum uio_rw rw) {
  iov->iov_base = base;
  iov->iov_len = len;
  uio->uio_iov = iov;
  uio->uio_iovcnt = 1;
  uio->uio_offset = off;
  uio->uio_resid = len;
  uio->uio_segflg = seg;
  uio->uio_rw = rw;
  uio->uio_procp = seg == UIO_USERSPACE ? curproc : 0;
}

int iovec_copyin(uint64_t uiov, struct kiovec **iovp, struct kiovec *aiov,
                 int cnt, size_t *residp) {
  struct kiovec *iov;
  size_t resid = 0;
  int i;

  *iovp = 0;
  if (cnt <= 0 || cnt > UIO_MAXIOV)
    return FNU_ERR_INVALID;
  iov = cnt > UIO_SMALLIOV
            ? malloc((size_t)cnt * sizeof(*iov), M_IOV, M_WAITOK)
            : aiov;
  *iovp = iov;
  if (copyin(curproc->p_p->pgdir, iov, (uintptr_t)uiov,
             (size_t)cnt * sizeof(*iov)) < 0)
    return FNU_ERR_FAULT;
  for (i = 0; i < cnt; i++) {
    resid += iov[i].iov_len;
    if (iov[i].iov_len > 0x7fffffff || resid > 0x7fffffff)
      return FNU_ERR_INVALID;
  }
  if (residp != 0)
    *residp = resid;
  return 0;
}

void iovec_free(struct kiovec *iov, int cnt) {
  if (iov != 0 && cnt > UIO_SMALLIOV)
    free(iov, M_IOV);
}

// A TRANSFER UNDER A FILE'S LOCKS
int uio_hold(struct uio *uio) {
  struct proc *p = uio->uio_procp;
  size_t ok = 0;
  int i;

  if (uio->uio_segflg != UIO_USERSPACE)
    return 0;
  proc_swap_freeze(p);
  for (i = 0; i < uio->uio_iovcnt; i++) {
    struct kiovec *iov = &uio->uio_iov[i];
    uintptr_t va = (uintptr_t)iov->iov_base, a;

    for (a = PGROUNDDOWN(va); a < va + iov->iov_len; a += PGSIZE)
      if (va + iov->iov_len < va ||
          uva_prefault(p->p_p->pgdir, a, uio->uio_rw == UIO_READ) < 0) {
        iov->iov_len = a > va ? a - va : 0;
        uio->uio_iovcnt = i + 1;
        break;
      }
    ok += iov->iov_len;
  }
  uio->uio_resid = ok;
  if (ok == 0) {
    proc_swap_thaw(p);
    return FNU_ERR_FAULT;
  }
  p->nofault++;
  return 0;
}

void uio_unhold(struct uio *uio) {
  if (uio->uio_segflg != UIO_USERSPACE)
    return;
  uio->uio_procp->nofault--;
  proc_swap_thaw(uio->uio_procp);
}
