#ifndef _SYS_SOCKET_H_
#define _SYS_SOCKET_H_

#include <sys/types.h>
#include <sys/uio.h>
#include "inc/abi.h"

// libc maps these to the system's own
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define AF_INET6 24
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_LOCAL
#define PF_INET AF_INET
#define PF_INET6 AF_INET6
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 0x4000
#define SOCK_CLOEXEC 0x8000
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008
#define SO_PEERCRED 0x1022
#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_DONTROUTE 0x4
#define MSG_EOR 0x8
#define MSG_TRUNC 0x10
#define MSG_CTRUNC 0x20
#define MSG_WAITALL 0x40
#define MSG_DONTWAIT 0x80
#define MSG_NOSIGNAL 0x400
#define MSG_CMSG_CLOEXEC 0x800
#define SCM_RIGHTS 0x01

struct sockaddr {
  uint8_t sa_len;
  sa_family_t sa_family;
  char sa_data[14];
};

// room for an address of any family
struct sockaddr_storage {
  uint8_t ss_len;
  sa_family_t ss_family;
  unsigned char __ss_pad[256 - 2];
};

// sendmsg(2) and recvmsg(2)
struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  unsigned int msg_iovlen;
  void *msg_control;
  socklen_t msg_controllen;
  int msg_flags;
};

struct cmsghdr {
  socklen_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

#define __CMSG_ALIGN(n) (((n) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_DATA(cmsg)                                                        \
  ((unsigned char *)(cmsg) + __CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_NXTHDR(mhdr, cmsg)                                                \
  (((char *)(cmsg) + __CMSG_ALIGN((cmsg)->cmsg_len) +                          \
        __CMSG_ALIGN(sizeof(struct cmsghdr)) >                                 \
    ((char *)(mhdr)->msg_control) + (mhdr)->msg_controllen)                    \
       ? (struct cmsghdr *)0                                                   \
       : (struct cmsghdr *)((char *)(cmsg) + __CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_FIRSTHDR(mhdr)                                                    \
  ((mhdr)->msg_controllen >= sizeof(struct cmsghdr)                            \
       ? (struct cmsghdr *)(mhdr)->msg_control                                 \
       : (struct cmsghdr *)0)
#define CMSG_SPACE(len)                                                        \
  (__CMSG_ALIGN(sizeof(struct cmsghdr)) + __CMSG_ALIGN(len))
#define CMSG_LEN(len) (__CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))

// SO_PEERCRED
struct sockpeercred {
  uid_t uid;
  gid_t gid;
  pid_t pid;
};

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sv[2]);
int getpeereid(int s, uid_t *euid, gid_t *egid);
int bind(int s, const struct sockaddr *name, socklen_t namelen);
int listen(int s, int backlog);
int accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int connect(int s, const struct sockaddr *name, socklen_t namelen);
ssize_t send(int s, const void *msg, size_t len, int flags);
ssize_t recv(int s, void *buf, size_t len, int flags);
ssize_t sendto(int s, const void *msg, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int s, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
ssize_t sendmsg(int s, const struct msghdr *msg, int flags);
ssize_t recvmsg(int s, struct msghdr *msg, int flags);
int shutdown(int s, int how);
int getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int getpeername(int s, struct sockaddr *name, socklen_t *namelen);
int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen);

#endif
