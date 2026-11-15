// POSIX sockets; the addressed ones are in socket.c
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <fnu/sys.h>
#include "posix.h"

// timeval for POSIX, timespec for us
static int is_timeo(int level, int name) {
  return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO);
}

// SO_TYPE is answered in the system's numbers
_Static_assert(SOCK_STREAM == FNU_SOCKET_STREAM && SOCK_DGRAM == FNU_SOCKET_DATAGRAM,
               "socket types");

// option by number; -1 if unknown
static int sockopt_of(int level, int name) {
  if (level != SOL_SOCKET)
    return -1;
  switch (name) {
  case SO_REUSEADDR: return FNU_SOCKOPT_REUSEADDR;
  case SO_TYPE: return FNU_SOCKOPT_TYPE;
  case SO_ERROR: return FNU_SOCKOPT_ERROR;
  case SO_RCVTIMEO: return FNU_SOCKOPT_RECVTIME;
  case SO_SNDTIMEO: return FNU_SOCKOPT_SENDTIME;
  case SO_PEERCRED: return FNU_SOCKOPT_PEER;
  default: return -1;
  }
}

int socket(int domain, int type, int protocol) {
  if ((domain = __family_to_native(domain)) < 0 ||
      (type = __socktype_to_native(type)) < 0) {
    errno = domain < 0 ? EAFNOSUPPORT : EPROTOTYPE;
    return -1;
  }
  return (int)__posix(fnu_socket(domain, type, protocol));
}

int listen(int fd, int backlog) {
  return (int)__posix(fnu_listen(fd, backlog));
}

int shutdown(int fd, int how) {
  int n = how == SHUT_RD     ? FNU_SHUTDOWN_READ
          : how == SHUT_WR   ? FNU_SHUTDOWN_WRITE
          : how == SHUT_RDWR ? FNU_SHUTDOWN_BOTH
                             : 0;

  if (n == 0) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_shutdown(fd, n));
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
  const struct timeval *tv = val;
  struct fnu_timespec ts;
  int opt = sockopt_of(level, name);

  if (opt < 0) {
    errno = ENOPROTOOPT;
    return -1;
  }

  if (is_timeo(level, name)) {
    if (len < sizeof(*tv) || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
      errno = EINVAL;
      return -1;
    }
    ts.sec = tv->tv_sec;
    ts.nsec = (int32_t)(tv->tv_usec * 1000);
    ts.pad = 0;
    return (int)__posix(fnu_sockopt_set(fd, opt, &ts, sizeof(ts)));
  }
  return (int)__posix(fnu_sockopt_set(fd, opt, val, len));
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
  struct timeval *tv = val;
  struct fnu_timespec ts;
  uint32_t n = sizeof(ts);
  int opt = sockopt_of(level, name);

  if (opt < 0) {
    errno = ENOPROTOOPT;
    return -1;
  }

  if (is_timeo(level, name)) {
    if (*len < sizeof(*tv)) {
      errno = EINVAL;
      return -1;
    }
    if (__posix(fnu_sockopt_get(fd, opt, &ts, &n)) < 0)
      return -1;
    tv->tv_sec = ts.sec;
    tv->tv_usec = ts.nsec / 1000;
    *len = sizeof(*tv);
    return 0;
  }
  if (__posix(fnu_sockopt_get(fd, opt, val, (uint32_t *)len)) < 0)
    return -1;
  // the pending error is the system's; POSIX numbers it differently
  if (level == SOL_SOCKET && name == SO_ERROR && *len >= sizeof(int) &&
      *(int *)val != 0)
    *(int *)val = __errno_of(*(int *)val);
  return 0;
}
