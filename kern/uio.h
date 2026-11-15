#ifndef PRONINX_X86_64_UIO_H
#define PRONINX_X86_64_UIO_H

#include "inc/types.h"

struct proc;

// WHERE THE DATA IS AND WHICH WAY IT GOES
enum uio_rw { UIO_READ, UIO_WRITE };
enum uio_seg { UIO_USERSPACE, UIO_SYSSPACE };

struct kiovec {
  char *iov_base;
  size_t iov_len;
};

#define UIO_MAXIOV 1024
#define UIO_SMALLIOV 8

struct uio {
  struct kiovec *uio_iov;
  int uio_iovcnt;
  uint64_t uio_offset;
  size_t uio_resid;
  enum uio_seg uio_segflg;
  enum uio_rw uio_rw;
  struct proc *uio_procp;
};

int uiomove(void *buf, size_t n, struct uio *uio);
void uio_prepare(struct uio *uio, struct kiovec *iov, void *base, size_t len,
                 uint64_t off, enum uio_seg seg, enum uio_rw rw);
int iovec_copyin(uint64_t uiov, struct kiovec **iovp, struct kiovec *aiov,
                 int cnt, size_t *residp);
void iovec_free(struct kiovec *iov, int cnt);
int uio_hold(struct uio *uio);
void uio_unhold(struct uio *uio);

#endif
