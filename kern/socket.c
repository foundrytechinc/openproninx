// SOCKETS
#include "defs.h"
#include "file.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "uio.h"
#include "socketvar.h"
#include "inc/abi.h"
#include "inc/open.h"
#include "inc/signal.h"
#include "inc/stat.h"
#include "inc/syscall.h"

#define SOCKET_FOREVER 0x7fffffffu
#define SOCK_MAXFDS 253
#define SOCKET_FLAGS (FNU_SOCKET_NOWAIT | FNU_SOCKET_CLOSEEXEC)
#define SEND_FLAGS                                                             \
  (FNU_MSG_NOWAIT | FNU_SEND_NOSIGNAL | FNU_SEND_LOCALNET)
#define RECV_FLAGS                                                             \
  (FNU_RECV_PEEK | FNU_RECV_ALL | FNU_MSG_NOWAIT | FNU_RECV_FDCLOSEEXEC)

int somaxconn = 128;

void socketinit(void) {
  sysctl_add_int("kern.ipc.somaxconn", &somaxconn, FNU_CTLF_STDIO | FNU_CTLF_WRITE,
                 1, 65535);
  unp_init();
}

static const struct pr_usrreqs *protocol(int domain) {
  switch (domain) {
  case FNU_NET_LOCAL:
    return &unp_usrreqs;
  case FNU_NET_INET:
    return &in_usrreqs;
  default:
    return 0;
  }
}

struct socket *soalloc(int domain, int type, const struct pr_usrreqs *usrreqs) {
  struct socket *so = malloc(sizeof(*so), M_FILE, M_WAITOK | M_ZERO);

  so->domain = domain;
  so->type = type;
  so->usrreqs = usrreqs;
  return so;
}

void sofree(struct socket *so) {
  so->usrreqs->detach(so);
  free(so, M_FILE);
}

int so_ticks(uint ms) { return ms == 0 ? 0 : mstohz((int64_t)ms); }

int socreate(int domain, int type, int proto, struct socket **out) {
  const struct pr_usrreqs *pr = protocol(domain);
  struct socket *so;
  int error;

  if (pr == 0)
    return FNU_ERR_NOFAMILY;
  if (type != FNU_SOCKET_STREAM && type != FNU_SOCKET_DATAGRAM &&
      (type != FNU_SOCKET_RAW || domain != FNU_NET_INET))
    return FNU_ERR_WRONGTYPE;
  so = soalloc(domain, type, pr);
  so->protocol = proto;
  if ((error = pr->attach(so)) != 0) {
    free(so, M_FILE);
    return error;
  }
  *out = so;
  return 0;
}

void soclose(struct socket *so) {
  if (so != 0)
    sofree(so);
}

int sosend(struct socket *so, struct uio *uio, const struct sockaddr_any *to,
           struct sorights *rights, int flags) {
  int n;

  n = so->usrreqs->send(so, uio, to, rights, flags);
  if (n == -FNU_ERR_BROKENPIPE && (flags & FNU_SEND_NOSIGNAL) == 0)
    sig_post(curproc, FNU_SIG_BROKENPIPE, 0);
  return n;
}

int soreceive(struct socket *so, struct uio *uio, struct sockaddr_any *from,
              struct sorights **rights, int *flagsp) {
  if (rights != 0)
    *rights = 0;
  return so->usrreqs->receive(so, uio, from, rights, flagsp);
}

// what select would find
int soready(struct socket *so, int want) { return so->usrreqs->ready(so, want); }

int64_t soavail(struct socket *so, int write, int *eof) {
  return so->usrreqs->avail(so, write, eof);
}

// fstat(2) of a socket
void so_stat(struct socket *so, struct fnu_stat *st) {
  memset(st, 0, sizeof(*st));
  st->type = FNU_TYPE_SOCKET;
  st->mode = 0666;
  st->uid = curproc->p_p->uid;
  st->gid = curproc->p_p->gid;
  if (so->usrreqs->sense != 0)
    so->usrreqs->sense(so, st);
}

// SYSTEM CALLS

static struct socket *sockof(struct file *f) {
  return f != 0 && f->type == FD_SOCKET ? f->sock : 0;
}

static int sock_nonblock(struct file *f) {
  return (f->flags & FNU_OPEN_NOWAIT) != 0;
}

static int sock_flags(struct file *f, int flags) {
  return sock_nonblock(f) ? flags | FNU_MSG_NOWAIT : flags;
}

static int sockaddr_in(struct proc *p, uint64_t uaddr, int len, struct sockaddr_any *a) {
  if (len < 2 || len > FNU_NETADDR_MAX)
    return FNU_ERR_INVALID;
  memset(a, 0, sizeof(*a));
  if (copyin(p->p_p->pgdir, a, (uintptr_t)uaddr, (uint)len) < 0)
    return FNU_ERR_FAULT;
  a->sa.len = (uint8)len;
  return 0;
}

static int sockaddr_out(struct proc *p, struct sockaddr_any *a, uint64_t uaddr,
                        uint64_t ulen) {
  int room, n;

  if (uaddr == 0 || ulen == 0)
    return 0;
  if (copyin(p->p_p->pgdir, &room, (uintptr_t)ulen, sizeof(room)) < 0)
    return FNU_ERR_FAULT;
  if (room < 0)
    return FNU_ERR_INVALID;
  n = a->sa.len < room ? a->sa.len : room;
  if (n > 0 && copyout(p->p_p->pgdir, (uintptr_t)uaddr, a, (uint)n) < 0)
    return FNU_ERR_FAULT;
  n = a->sa.len;
  return copyout(p->p_p->pgdir, (uintptr_t)ulen, &n, sizeof(n)) < 0 ? FNU_ERR_FAULT : 0;
}

static int sofile(struct proc *p, struct socket *so, int flags, int *fdp) {
  struct file *f;
  int fd;

  if ((f = filealloc()) == 0) {
    soclose(so);
    return FNU_ERR_SYSTEMFULL;
  }
  f->type = FD_SOCKET;
  f->sock = so;
  so->file = f;
  f->readable = 1;
  f->writable = 1;
  f->flags = FNU_OPEN_READWRITE | ((flags & FNU_SOCKET_NOWAIT) ? FNU_OPEN_NOWAIT : 0);
  if ((fd = fd_alloc_at(p->p_p, f, 0, (flags & FNU_SOCKET_CLOSEEXEC) ? FNU_FD_CLOSEEXEC : 0)) < 0) {
    fileclose(f);
    return FNU_ERR_TOOMANYOPEN;
  }
  *fdp = fd;
  return 0;
}

static int protocol_ok(int domain, int type, int protocol) {
  if (type == FNU_SOCKET_RAW)
    return domain == FNU_NET_INET && protocol == FNU_PROTO_ICMP;
  if (protocol == 0)
    return 1;
  return domain == FNU_NET_INET &&
         ((type == FNU_SOCKET_STREAM && protocol == FNU_PROTO_TCP) ||
          (type == FNU_SOCKET_DATAGRAM && protocol == FNU_PROTO_UDP));
}

int sys_socket(struct proc *p, const register_t *uap, register_t *retval) {
  struct socket *so;
  int domain, type, protocol, fd, error;

  domain = (int)uap[0];
  type = (int)uap[1];
  protocol = (int)uap[2];
  if (!protocol_ok(domain, type & ~SOCKET_FLAGS,
                   protocol))
    return FNU_ERR_NOPROTOCOL;
  if (pledge_socket(p, domain) < 0) {
    pledge_fail(p, SYS_socket);
    return FNU_ERR_NOTPERMITTED;
  }
  if ((error = socreate(domain, type & ~SOCKET_FLAGS, protocol, &so)) != 0)
    return error;
  if ((error = sofile(p, so, type, &fd)) != 0)
    return error;
  *retval = fd;
  return 0;
}

int sys_socketpair(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct socket *a, *b;
  struct file *fa;
  uint64_t ufds;
  int domain, type, protocol, fds[2], error;

  domain = (int)uap[0];
  type = (int)uap[1];
  protocol = (int)uap[2];
  ufds = uap[3];
  if (!protocol_ok(domain, type & ~SOCKET_FLAGS,
                   protocol))
    return FNU_ERR_NOPROTOCOL;
  if ((error = socreate(domain, type & ~SOCKET_FLAGS, 0, &a)) != 0)
    return error;
  if ((error = socreate(domain, type & ~SOCKET_FLAGS, 0, &b)) != 0) {
    soclose(a);
    return error;
  }
  if ((error = a->usrreqs->connect2(a, b)) != 0) {
    soclose(a);
    soclose(b);
    return error;
  }
  if ((error = sofile(p, a, type, &fds[0])) != 0) {
    soclose(b);
    return error;
  }
  fa = fd_getfile(p->p_p, fds[0]);
  if ((error = sofile(p, b, type, &fds[1])) != 0) {
    fd_untake(p->p_p, fds[0], fa);
    FRELE(fa);
    return error;
  }
  if (copyout(p->p_p->pgdir, (uintptr_t)ufds, fds, sizeof(fds)) < 0) {
    struct file *fb = fd_getfile(p->p_p, fds[1]);

    fd_untake(p->p_p, fds[0], fa);
    if (fb != 0) {
      fd_untake(p->p_p, fds[1], fb);
      FRELE(fb);
    }
    FRELE(fa);
    return FNU_ERR_FAULT;
  }
  FRELE(fa);
  return 0;
}

static int fd_getsock(struct proc *p, int fd, struct file **pf,
                      struct socket **pso) {
  if ((*pf = fd_getfile(p->p_p, fd)) == 0)
    return FNU_ERR_BADHANDLE;
  if ((*pso = sockof(*pf)) == 0) {
    FRELE(*pf);
    return FNU_ERR_NOTSOCKET;
  }
  return 0;
}

typedef int (*sockcall)(struct proc *p, const register_t *uap,
                        register_t *retval, struct file *f, struct socket *so);

static int withsock(struct proc *p, const register_t *uap, register_t *retval,
                    sockcall fn, int num) {
  struct socket *so;
  struct file *f;
  int error;

  if ((error = fd_getsock(p, (int)uap[0], &f, &so)) != 0)
    return error;
  if (num != 0 && pledge_socket(p, so->domain) < 0) {
    FRELE(f);
    pledge_fail(p, num);
    return FNU_ERR_NOTPERMITTED;
  }
  error = fn(p, uap, retval, f, so);
  FRELE(f);
  return error;
}

static int bind_sock(struct proc *p, const register_t *uap,
                     register_t *retval, struct file *f, struct socket *so) {
  struct sockaddr_any a;
  uint64_t uaddr;
  int len, error;

  (void)retval;
  (void)f;
  uaddr = uap[1];
  len = (int)uap[2];
  if ((error = sockaddr_in(p, uaddr, len, &a)) != 0)
    return error;
  return so->usrreqs->bind(so, &a);
}

static int listen_sock(struct proc *p, const register_t *uap,
                       register_t *retval, struct file *f, struct socket *so) {
  int backlog;

  (void)p;
  (void)retval;
  (void)f;
  backlog = (int)uap[1];
  if (backlog < 0)
    return FNU_ERR_INVALID;
  return so->usrreqs->listen(so, backlog);
}

static int connect_sock(struct proc *p, const register_t *uap,
                        register_t *retval, struct file *f, struct socket *so) {
  struct sockaddr_any a;
  uint64_t uaddr;
  int len, error;

  (void)retval;
  uaddr = uap[1];
  len = (int)uap[2];
  if ((error = sockaddr_in(p, uaddr, len, &a)) != 0)
    return error;
  return so->usrreqs->connect(so, &a, sock_nonblock(f));
}

static int accept_sock(struct proc *p, const register_t *uap,
                       register_t *retval, struct file *f, struct socket *so) {
  struct sockaddr_any a;
  struct socket *ns;
  struct file *nf;
  uint64_t uaddr, ulen;
  int fd, error, flags;

  uaddr = uap[1];
  ulen = uap[2];
  flags = sock_nonblock(f) ? FNU_SOCKET_NOWAIT : 0;
  if ((error = so->usrreqs->accept(so, sock_nonblock(f), &ns)) != 0)
    return error;
  memset(&a, 0, sizeof(a));
  if (uaddr != 0)
    ns->usrreqs->peeraddr(ns, &a);
  if ((error = sofile(p, ns, flags, &fd)) != 0)
    return error;
  if (uaddr != 0 && (error = sockaddr_out(p, &a, uaddr, ulen)) != 0) {
    if ((nf = fd_getfile(p->p_p, fd)) != 0) {
      fd_untake(p->p_p, fd, nf);
      FRELE(nf);
    }
    return error;
  }
  *retval = fd;
  return 0;
}

static int shutdown_sock(struct proc *p, const register_t *uap,
                         register_t *retval, struct file *f, struct socket *so) {
  int how;

  (void)p;
  (void)retval;
  (void)f;
  how = (int)uap[1];
  if (how == 0 || (how & ~FNU_SHUTDOWN_BOTH))
    return FNU_ERR_INVALID;
  return so->usrreqs->shutdown(so, how);
}

static int sockaddr_sock(struct proc *p, const register_t *uap,
                         register_t *retval, struct file *f, struct socket *so) {
  struct sockaddr_any a;
  int which = (int)uap[1], error;

  (void)retval;
  (void)f;
  if (which != FNU_SOCKADDR_LOCAL && which != FNU_SOCKADDR_PEER)
    return FNU_ERR_INVALID;
  memset(&a, 0, sizeof(a));
  if ((error = (which == FNU_SOCKADDR_PEER ? so->usrreqs->peeraddr
                                           : so->usrreqs->sockaddr)(so, &a)) != 0)
    return error;
  return sockaddr_out(p, &a, uap[2], uap[3]);
}

static int msg_uio(const struct fnu_msg *m, struct uio *uio,
                   struct kiovec **iovp, struct kiovec *aiov, enum uio_rw rw) {
  size_t resid = 0;
  int error;

  *iovp = 0;
  if (m->iovcnt > UIO_MAXIOV)
    return FNU_ERR_MSGTOOBIG;
  if (m->iovcnt != 0 &&
      (error = iovec_copyin((uint64_t)(uintptr_t)m->iov, iovp, aiov,
                            (int)m->iovcnt, &resid)) != 0)
    return error;
  uio->uio_iov = *iovp != 0 ? *iovp : aiov;
  uio->uio_iovcnt = (int)m->iovcnt;
  uio->uio_offset = 0;
  uio->uio_resid = resid;
  uio->uio_segflg = UIO_USERSPACE;
  uio->uio_rw = rw;
  uio->uio_procp = curproc;
  return 0;
}

static int msg_rights(struct proc *p, struct socket *so, const struct fnu_msg *m,
                      struct sorights **rights) {
  int *fds, error;

  *rights = 0;
  if (m->nfds == 0)
    return 0;
  if (so->domain != FNU_NET_LOCAL || m->nfds > SOCK_MAXFDS)
    return FNU_ERR_INVALID;
  fds = malloc(m->nfds * sizeof(int), M_TEMP, M_WAITOK);
  if (copyin(p->p_p->pgdir, fds, (uintptr_t)m->fds, m->nfds * sizeof(int)) < 0)
    error = FNU_ERR_FAULT;
  else
    error = unp_internalize(fds, (int)m->nfds, rights);
  free(fds, M_TEMP);
  return error;
}

static int send_sock(struct proc *p, const register_t *uap, register_t *retval,
                     struct file *f, struct socket *so) {
  struct kiovec aiov[UIO_SMALLIOV], *iov;
  struct sorights *rights;
  struct sockaddr_any to;
  struct fnu_msg m;
  struct uio uio;
  int flags = (int)uap[2], n, error;

  if (flags & ~SEND_FLAGS)
    return FNU_ERR_INVALID;
  if (copyin(p->p_p->pgdir, &m, (uintptr_t)uap[1], sizeof(m)) < 0)
    return FNU_ERR_FAULT;
  if (m.addr != 0 &&
      (error = sockaddr_in(p, (uint64_t)(uintptr_t)m.addr, (int)m.addrlen,
                           &to)) != 0)
    return error;
  if ((error = msg_uio(&m, &uio, &iov, aiov, UIO_WRITE)) != 0 ||
      (error = msg_rights(p, so, &m, &rights)) != 0) {
    iovec_free(iov, (int)m.iovcnt);
    return error;
  }
  n = sosend(so, &uio, m.addr != 0 ? &to : 0, rights, sock_flags(f, flags));
  iovec_free(iov, (int)m.iovcnt);
  if (n < 0)
    return -n;
  *retval = n;
  return 0;
}

static int recv_sock(struct proc *p, const register_t *uap, register_t *retval,
                     struct file *f, struct socket *so) {
  struct kiovec aiov[UIO_SMALLIOV], *iov;
  struct sorights *rights = 0;
  struct sockaddr_any from;
  struct fnu_msg m;
  struct uio uio;
  int flags = (int)uap[2], want, n, nfds, error = 0;
  int *fds;
  uint len;

  if (flags & ~RECV_FLAGS)
    return FNU_ERR_INVALID;
  if (copyin(p->p_p->pgdir, &m, (uintptr_t)uap[1], sizeof(m)) < 0)
    return FNU_ERR_FAULT;
  if ((error = msg_uio(&m, &uio, &iov, aiov, UIO_READ)) != 0) {
    iovec_free(iov, (int)m.iovcnt);
    return error;
  }
  memset(&from, 0, sizeof(from));
  want = flags = sock_flags(f, flags);
  n = soreceive(so, &uio, m.addr != 0 ? &from : 0, &rights, &flags);
  iovec_free(iov, (int)m.iovcnt);
  if (n < 0)
    return -n;
  m.flags = (uint32)(flags & (FNU_RECV_CUT | FNU_RECV_FDCUT));
  if (m.addr != 0) {
    len = from.sa.len < m.addrlen ? from.sa.len : m.addrlen;
    if (len > 0 && copyout(p->p_p->pgdir, (uintptr_t)m.addr, &from, len) < 0)
      error = FNU_ERR_FAULT;
    m.addrlen = from.sa.len;
  }
  nfds = 0;
  if (rights != 0) {
    nfds = m.fds != 0 && m.nfds < (uint32)rights->n ? (int)m.nfds
           : m.fds != 0                              ? rights->n
                                                     : 0;
    fds = malloc((size_t)(nfds > 0 ? nfds : 1) * sizeof(int), M_TEMP, M_WAITOK);
    if ((error = unp_externalize(rights, fds, &nfds, want, (int *)&m.flags)) == 0 &&
        nfds > 0 && copyout(p->p_p->pgdir, (uintptr_t)m.fds, fds,
                            (size_t)nfds * sizeof(int)) < 0)
      error = FNU_ERR_FAULT;
    free(fds, M_TEMP);
  }
  m.nfds = (uint32)nfds;
  if (error == 0 && copyout(p->p_p->pgdir, (uintptr_t)uap[1], &m, sizeof(m)) < 0)
    error = FNU_ERR_FAULT;
  if (error != 0)
    return error;
  *retval = n;
  return 0;
}

static int sosetopt(struct socket *so, int name, void *val, uint len) {
  const struct fnu_timespec *ts = val;
  uint ms;

  if (name == FNU_SOCKOPT_RECVTIME || name == FNU_SOCKOPT_SENDTIME) {
    if (len < sizeof(*ts) || ts->sec < 0 || ts->nsec < 0 ||
        ts->nsec >= 1000000000)
      return FNU_ERR_INVALID;
    if (ts->sec > SOCKET_FOREVER / 1000)
      ms = SOCKET_FOREVER;
    else
      ms = (uint)(ts->sec * 1000 + (ts->nsec + 999999) / 1000000);
    if (name == FNU_SOCKOPT_RECVTIME)
      so->rcvtimeo = ms;
    else
      so->sndtimeo = ms;
    return 0;
  }
  return so->usrreqs->ctloutput(so, 1, name, val, &len);
}

static int sogetopt(struct socket *so, int name, void *val, uint *len) {
  struct fnu_timespec ts;
  uint ms;
  int v;

  if (name == FNU_SOCKOPT_TYPE) {
    if (*len < sizeof(int))
      return FNU_ERR_INVALID;
    v = so->type;
    memmove(val, &v, sizeof(v));
    *len = sizeof(v);
    return 0;
  }
  if (name == FNU_SOCKOPT_RECVTIME || name == FNU_SOCKOPT_SENDTIME) {
    if (*len < sizeof(ts))
      return FNU_ERR_INVALID;
    ms = name == FNU_SOCKOPT_RECVTIME ? so->rcvtimeo : so->sndtimeo;
    memset(&ts, 0, sizeof(ts));
    ts.sec = ms / 1000;
    ts.nsec = (int32_t)((ms % 1000) * 1000000);
    memmove(val, &ts, sizeof(ts));
    *len = sizeof(ts);
    return 0;
  }
  return so->usrreqs->ctloutput(so, 0, name, val, len);
}

union sockopt_val {
  struct fnu_timespec ts;
  struct fnu_peer cred;
  int i;
};

static int setsockopt_sock(struct proc *p, const register_t *uap,
                           register_t *retval, struct file *f, struct socket *so) {
  union sockopt_val v;
  uint64_t uval;
  int name, len;

  (void)retval;
  (void)f;
  name = (int)uap[1];
  uval = uap[2];
  len = (int)uap[3];
  if (len <= 0 || len > (int)sizeof(v))
    return FNU_ERR_INVALID;
  memset(&v, 0, sizeof(v));
  if (copyin(p->p_p->pgdir, &v, (uintptr_t)uval, (uint)len) < 0)
    return FNU_ERR_FAULT;
  return sosetopt(so, name, &v, (uint)len);
}

static int getsockopt_sock(struct proc *p, const register_t *uap,
                           register_t *retval, struct file *f, struct socket *so) {
  union sockopt_val v;
  uint64_t uval, ulen;
  uint len;
  int name, want, error;

  (void)retval;
  (void)f;
  name = (int)uap[1];
  uval = uap[2];
  ulen = uap[3];
  if (copyin(p->p_p->pgdir, &want, (uintptr_t)ulen, sizeof(want)) < 0)
    return FNU_ERR_FAULT;
  if (want <= 0)
    return FNU_ERR_INVALID;
  len = want > (int)sizeof(v) ? (uint)sizeof(v) : (uint)want;
  memset(&v, 0, sizeof(v));
  if ((error = sogetopt(so, name, &v, &len)) != 0)
    return error;
  if (copyout(p->p_p->pgdir, (uintptr_t)uval, &v, len) < 0)
    return FNU_ERR_FAULT;
  want = (int)len;
  return copyout(p->p_p->pgdir, (uintptr_t)ulen, &want, sizeof(want)) < 0 ? FNU_ERR_FAULT : 0;
}

int sys_bind(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, bind_sock, SYS_bind);
}
int sys_listen(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, listen_sock, SYS_listen);
}
int sys_connect(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, connect_sock, SYS_connect);
}
int sys_accept(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, accept_sock, SYS_accept);
}
int sys_shutdown(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, shutdown_sock, 0);
}
int sys_send(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, send_sock, 0);
}
int sys_recv(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, recv_sock, 0);
}
int sys_sockaddr(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, sockaddr_sock, 0);
}
int sys_sockopt_set(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, setsockopt_sock, 0);
}
int sys_sockopt_get(struct proc *p, const register_t *uap, register_t *rv) {
  return withsock(p, uap, rv, getsockopt_sock, 0);
}
