#ifndef _SYS_UIO_H_
#define _SYS_UIO_H_

#include <sys/types.h>
#include <limits.h>

#define UIO_MAXIOV IOV_MAX

struct iovec {
  void *iov_base;
  size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
ssize_t pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

#endif
