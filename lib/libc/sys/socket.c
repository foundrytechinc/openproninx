// sockets: address and length go as given, come back cut to the room
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

_Static_assert(sizeof(struct iovec) == sizeof(struct fnu_segment),
               "iovec is the kernel's");

// family, type, flags: the system's way
int __family_to_native(int af) {
  switch (af) {
  case AF_UNSPEC: return FNU_NET_ANY;
  case AF_UNIX: return FNU_NET_LOCAL;
  case AF_INET: return FNU_NET_INET;
  case AF_INET6: return FNU_NET_INET6;
  default: return -1;
  }
}

static int family_of_native(int f) {
  switch (f) {
  case FNU_NET_LOCAL: return AF_UNIX;
  case FNU_NET_INET: return AF_INET;
  case FNU_NET_INET6: return AF_INET6;
  default: return AF_UNSPEC;
  }
}

int __socktype_to_native(int t) {
  int n;

  switch (t & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
  case SOCK_STREAM: n = FNU_SOCKET_STREAM; break;
  case SOCK_DGRAM: n = FNU_SOCKET_DATAGRAM; break;
  default: return -1;
  }
  return n | ((t & SOCK_NONBLOCK) ? FNU_SOCKET_NOWAIT : 0) |
         ((t & SOCK_CLOEXEC) ? FNU_SOCKET_CLOSEEXEC : 0);
}

// address in: copied with the native family; out: turned back in place
const void *__addr_in(const void *sa, unsigned int len, void *room) {
  unsigned char *b = room;
  int f;

  if (sa == 0 || len < 2 || len > sizeof(struct sockaddr_storage))
    return sa;
  memcpy(b, sa, len);
  if ((f = __family_to_native(b[1])) >= 0)
    b[1] = (unsigned char)f;
  return b;
}

void __addr_out(void *sa, unsigned int len) {
  unsigned char *b = sa;

  if (sa != 0 && len >= 2)
    b[1] = (unsigned char)family_of_native(b[1]);
}

// most one message carries, as the kernel takes it
#define MAXFDS 253

// MSG_ to FNU_MSG_; OOB and EOR: no protocol here has them
static int msg_flags_in(int flags, long *bad) {
  static const int map[][2] = {
      {MSG_PEEK, FNU_RECV_PEEK},           {MSG_WAITALL, FNU_RECV_ALL},
      {MSG_DONTWAIT, FNU_MSG_NOWAIT},   {MSG_NOSIGNAL, FNU_SEND_NOSIGNAL},
      {MSG_DONTROUTE, FNU_SEND_LOCALNET}, {MSG_CMSG_CLOEXEC, FNU_RECV_FDCLOSEEXEC},
  };
  int out = 0;
  size_t i;

  *bad = (flags & (MSG_OOB | MSG_EOR)) ? -EOPNOTSUPP : 0;
  for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (flags & map[i][0]) {
      out |= map[i][1];
      flags &= ~map[i][0];
    }
  if (flags & ~(MSG_OOB | MSG_EOR))
    *bad = -EINVAL;
  return out;
}

static int msg_flags_out(unsigned int flags) {
  return ((flags & FNU_RECV_CUT) ? MSG_TRUNC : 0) |
         ((flags & FNU_RECV_FDCUT) ? MSG_CTRUNC : 0);
}

static ssize_t sendone(int s, const void *buf, size_t len, int flags,
                       const void *to, socklen_t tolen) {
  struct fnu_segment iov = {(void *)buf, len};
  struct sockaddr_storage room;
  struct fnu_msg m = {&iov, 1, 0, (void *)__addr_in(to, tolen, &room),
                      to != 0 ? tolen : 0, 0, 0};
  long bad;
  int f = msg_flags_in(flags, &bad);

  return __posix(bad != 0 ? bad : fnu_send(s, &m, f));
}

static ssize_t recvone(int s, void *buf, size_t len, int flags, void *from,
                       socklen_t *fromlen) {
  struct fnu_segment iov = {buf, len};
  struct fnu_msg m = {&iov, 1, 0, fromlen != 0 ? from : 0,
                      fromlen != 0 ? *fromlen : 0, 0, 0};
  long bad, n;
  int f = msg_flags_in(flags, &bad);

  if ((n = __posix(bad != 0 ? bad : fnu_recv(s, &m, f))) >= 0 && fromlen != 0) {
    __addr_out(from, *fromlen < m.addrlen ? *fromlen : m.addrlen);
    *fromlen = m.addrlen;
  }
  return n;
}

int bind(int s, const struct sockaddr *name, socklen_t namelen) {
  struct sockaddr_storage room;

  return (int)__posix(fnu_bind(s, __addr_in(name, namelen, &room), (int)namelen));
}

int connect(int s, const struct sockaddr *name, socklen_t namelen) {
  struct sockaddr_storage room;

  return (int)__posix(
      fnu_connect(s, __addr_in(name, namelen, &room), (int)namelen));
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
  int len = addrlen != 0 ? (int)*addrlen : 0;
  int fd = (int)__posix(fnu_accept(s, addrlen != 0 ? addr : 0, addrlen != 0 ? &len : 0));

  if (fd >= 0 && addrlen != 0) {
    __addr_out(addr, *addrlen < (socklen_t)len ? *addrlen : (socklen_t)len);
    *addrlen = (socklen_t)len;
  }
  return fd;
}

ssize_t sendto(int s, const void *msg, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen) {
  return sendone(s, msg, len, flags, to, tolen);
}

ssize_t recvfrom(int s, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen) {
  return recvone(s, buf, len, flags, from, fromlen);
}

ssize_t send(int s, const void *msg, size_t len, int flags) {
  return sendone(s, msg, len, flags, 0, 0);
}

ssize_t recv(int s, void *buf, size_t len, int flags) {
  return recvone(s, buf, len, flags, 0, 0);
}

// fds of every SCM_RIGHTS message, one array for the kernel
ssize_t sendmsg(int s, const struct msghdr *msg, int flags) {
  struct sockaddr_storage room;
  struct fnu_msg m = {(struct fnu_segment *)msg->msg_iov, msg->msg_iovlen, 0,
                      (void *)__addr_in(msg->msg_name, msg->msg_namelen, &room),
                      msg->msg_name != 0 ? msg->msg_namelen : 0, 0, 0};
  struct cmsghdr *c;
  int fds[MAXFDS], f;
  long bad;
  size_t n;

  f = msg_flags_in(flags, &bad);
  if (bad != 0)
    return __posix(bad);
  for (c = msg->msg_controllen != 0 ? CMSG_FIRSTHDR(msg) : 0; c != 0;
       c = CMSG_NXTHDR(msg, c)) {
    if (c->cmsg_len < CMSG_LEN(0) || c->cmsg_level != SOL_SOCKET ||
        c->cmsg_type != SCM_RIGHTS)
      return __posix(-EINVAL);
    n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    if (m.nfds + n > MAXFDS)
      return __posix(-EINVAL);
    memcpy(fds + m.nfds, CMSG_DATA(c), n * sizeof(int));
    m.nfds += (uint32_t)n;
  }
  m.fds = m.nfds != 0 ? fds : 0;
  return __posix(fnu_send(s, &m, f));
}

// and back into one SCM_RIGHTS message, as much as fits
ssize_t recvmsg(int s, struct msghdr *msg, int flags) {
  struct fnu_msg m = {(struct fnu_segment *)msg->msg_iov, msg->msg_iovlen, 0,
                      msg->msg_name, msg->msg_name != 0 ? msg->msg_namelen : 0,
                      0, 0};
  struct cmsghdr *c;
  int fds[MAXFDS], f;
  long bad, r;

  f = msg_flags_in(flags, &bad);
  if (bad != 0)
    return __posix(bad);
  if (msg->msg_control != 0 && msg->msg_controllen >= CMSG_LEN(sizeof(int))) {
    m.nfds = (uint32_t)((msg->msg_controllen - CMSG_LEN(0)) / sizeof(int));
    if (m.nfds > MAXFDS)
      m.nfds = MAXFDS;
    m.fds = fds;
  }
  if ((r = __posix(fnu_recv(s, &m, f))) < 0)
    return r;
  msg->msg_flags = msg_flags_out(m.flags);
  if (msg->msg_name != 0) {
    __addr_out(msg->msg_name,
               msg->msg_namelen < m.addrlen ? msg->msg_namelen : m.addrlen);
    msg->msg_namelen = m.addrlen;
  }
  if (m.nfds == 0) {
    msg->msg_controllen = 0;
    return r;
  }
  c = CMSG_FIRSTHDR(msg);
  c->cmsg_len = CMSG_LEN(m.nfds * sizeof(int));
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(c), fds, m.nfds * sizeof(int));
  if (msg->msg_controllen > CMSG_SPACE(m.nfds * sizeof(int)))
    msg->msg_controllen = CMSG_SPACE(m.nfds * sizeof(int));
  return r;
}

static int name(int which, int s, struct sockaddr *sa, socklen_t *salen) {
  int len = (int)*salen;

  if (__posix(fnu_sockaddr(s, which, sa, &len)) < 0)
    return -1;
  __addr_out(sa, *salen < (socklen_t)len ? *salen : (socklen_t)len);
  *salen = (socklen_t)len;
  return 0;
}

int getsockname(int s, struct sockaddr *sa, socklen_t *salen) {
  return name(FNU_SOCKADDR_LOCAL, s, sa, salen);
}

int getpeername(int s, struct sockaddr *sa, socklen_t *salen) {
  return name(FNU_SOCKADDR_PEER, s, sa, salen);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
  if ((domain = __family_to_native(domain)) < 0 ||
      (type = __socktype_to_native(type)) < 0) {
    errno = domain < 0 ? EAFNOSUPPORT : EPROTOTYPE;
    return -1;
  }
  return (int)__posix(fnu_socketpair(domain, type, protocol, sv));
}

// getpeereid(2) is SO_PEERCRED
int getpeereid(int s, uid_t *euid, gid_t *egid) {
  struct sockpeercred cred;
  socklen_t len = sizeof(cred);

  if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
    return -1;
  *euid = cred.uid;
  *egid = cred.gid;
  return 0;
}
