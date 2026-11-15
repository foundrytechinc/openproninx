// INET SOCKETS
#include "defs.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "uio.h"
#include "socketvar.h"
#include "inet.h"
#include "inc/stat.h"

#define SOCKET_FOREVER 0x7fffffffu
#define SOCKET_CONNECT 10000
#define UDP_MAXDGRAM (0xffff - 8 - 20)

// zero is no timeout
static uint sotimeo(uint ms) { return ms != 0 ? ms : SOCKET_FOREVER; }

struct inpcb {
  struct tcp_sock *tcp;
  struct dgram_sock *dgram;
  struct fnu_network_endpoint peer;
  int haspeer;
  int reuse;
};

static void addr_of(const struct fnu_network_endpoint *ep, struct sockaddr_any *a) {
  struct fnu_inetaddr *out = &a->in;

  memset(out, 0, sizeof(*out));
  out->len = sizeof(*out);
  out->family = FNU_NET_INET;
  out->port = (uint16)((ep->port >> 8) | (ep->port << 8));
  memmove(out->addr, ep->address, 4);
}

static int endpoint_of(const struct sockaddr_any *a, struct fnu_network_endpoint *out) {
  if (a->sa.family != FNU_NET_INET)
    return FNU_ERR_NOFAMILY;
  if (a->sa.len < __offsetof(struct fnu_inetaddr, zero))
    return FNU_ERR_INVALID;
  memmove(out->address, a->in.addr, 4);
  out->port = (uint16)((a->in.port >> 8) | (a->in.port << 8));
  return 0;
}

static int in_attach(struct socket *so) {
  struct inpcb *inp = malloc(sizeof(*inp), M_FILE, M_WAITOK | M_ZERO);
  int r;

  if (so->type == FNU_SOCKET_RAW && curproc->p_p->uid != 0) {
    free(inp, M_FILE);
    return FNU_ERR_NOTPERMITTED;
  }
  r = so->type == FNU_SOCKET_STREAM ? proninx_tcp_open(&inp->tcp)
      : so->type == FNU_SOCKET_RAW  ? proninx_dgram_raw_open(&inp->dgram, so->protocol)
                                    : proninx_dgram_open(&inp->dgram);
  if (r < 0) {
    free(inp, M_FILE);
    return -r;
  }
  so->pcb = inp;
  return 0;
}

static void in_detach(struct socket *so) {
  struct inpcb *inp = so->pcb;

  if (inp->tcp != 0)
    proninx_tcp_close(inp->tcp);
  if (inp->dgram != 0)
    proninx_dgram_close(inp->dgram);
  free(inp, M_FILE);
}

static int in_bind(struct socket *so, const struct sockaddr_any *a) {
  struct inpcb *inp = so->pcb;
  struct fnu_network_endpoint ep;
  int error;

  if ((error = endpoint_of(a, &ep)) != 0)
    return error;
  return -(inp->tcp != 0 ? proninx_tcp_bind(inp->tcp, &ep)
                         : proninx_dgram_bind(inp->dgram, &ep));
}

static int in_listen(struct socket *so, int backlog) {
  struct inpcb *inp = so->pcb;

  if (inp->tcp == 0)
    return FNU_ERR_UNSUPPORTED;
  return -proninx_tcp_listen(inp->tcp, backlog);
}

static int in_connect(struct socket *so, const struct sockaddr_any *a, int nonblock) {
  struct inpcb *inp = so->pcb;
  struct fnu_network_endpoint ep;
  int error;

  if ((error = endpoint_of(a, &ep)) != 0)
    return error;
  if (inp->dgram != 0) {
    if ((error = proninx_dgram_connect(inp->dgram, &ep)) < 0)
      return -error;
    inp->peer = ep;
    inp->haspeer = 1;
    return 0;
  }
  return -proninx_tcp_connect(
      inp->tcp, &ep,
      nonblock ? 0 : (so->sndtimeo != 0 ? so->sndtimeo : SOCKET_CONNECT));
}

static int in_connect2(struct socket *a, struct socket *b) {
  (void)a;
  (void)b;
  return FNU_ERR_UNSUPPORTED;
}

static int in_accept(struct socket *so, int nonblock, struct socket **out) {
  struct inpcb *inp = so->pcb, *ninp;
  struct tcp_sock *t;
  struct socket *ns;
  int r;

  if (inp->tcp == 0)
    return FNU_ERR_UNSUPPORTED;
  if ((r = proninx_tcp_accept(inp->tcp, nonblock ? 0 : sotimeo(so->rcvtimeo), &t)) < 0)
    return -r;
  ns = soalloc(so->domain, so->type, so->usrreqs);
  ninp = malloc(sizeof(*ninp), M_FILE, M_WAITOK | M_ZERO);
  ninp->tcp = t;
  ns->pcb = ninp;
  *out = ns;
  return 0;
}

static int send_stream(struct socket *so, const void *buf, uint len, int nonblock) {
  struct inpcb *inp = so->pcb;
  uint wait = nonblock ? 0 : sotimeo(so->sndtimeo);
  uint done = 0;
  int r;

  while (done < len) {
    r = proninx_tcp_send(inp->tcp, (const char *)buf + done, len - done, wait);
    if (r < 0)
      return done > 0 ? (int)done : r;
    done += (uint)r;
  }
  return (int)done;
}

static int in_send(struct socket *so, struct uio *uio, const struct sockaddr_any *to,
                   struct sorights *rights, int flags) {
  struct inpcb *inp = so->pcb;
  struct fnu_network_endpoint ep;
  size_t want = uio->uio_resid, done = 0;
  int nonblock = (flags & FNU_MSG_NOWAIT) != 0;
  char *buf;
  int n;

  if (rights != 0) {
    unp_dispose(rights);
    return -FNU_ERR_INVALID;
  }
  if (inp->dgram != 0) {
    if (want > UDP_MAXDGRAM)
      return -FNU_ERR_MSGTOOBIG;
    if (to != 0 && (n = endpoint_of(to, &ep)) != 0)
      return -n;
    if (to == 0 && !inp->haspeer)
      return -FNU_ERR_NEEDADDRESS;
    buf = malloc(want > 0 ? want : 1, M_TEMP, M_WAITOK);
    if (uiomove(buf, want, uio) != 0)
      n = -FNU_ERR_FAULT;
    else
      n = proninx_dgram_sendto(inp->dgram, buf, (uint)want, to != 0 ? &ep : 0,
                             (flags & FNU_SEND_LOCALNET) != 0);
    free(buf, M_TEMP);
    return n;
  }
  if (to != 0)
    return -FNU_ERR_CONNECTED;
  if (want == 0)
    return 0;
  if ((buf = page_alloc()) == 0)
    return -FNU_ERR_NOMEMORY;
  while (done < want) {
    size_t chunk = want - done > PGSIZE ? PGSIZE : want - done;

    if (uiomove(buf, chunk, uio) != 0) {
      page_free(buf);
      return done > 0 ? (int)done : -FNU_ERR_FAULT;
    }
    n = send_stream(so, buf, (uint)chunk, nonblock);
    if (n < 0) {
      page_free(buf);
      return done > 0 ? (int)done : n;
    }
    done += (size_t)n;
    if ((size_t)n != chunk)
      break;
  }
  page_free(buf);
  return (int)done;
}

static int in_receive(struct socket *so, struct uio *uio, struct sockaddr_any *from,
                      struct sorights **rights, int *flagsp) {
  struct inpcb *inp = so->pcb;
  int flags = *flagsp, peek = (flags & FNU_RECV_PEEK) != 0;
  uint wait = (flags & FNU_MSG_NOWAIT) ? 0 : sotimeo(so->rcvtimeo);
  size_t want = uio->uio_resid, done = 0, chunk;
  struct dgram *d;
  char *buf;
  int n = 0;

  (void)rights;
  *flagsp = 0;
  if (inp->dgram != 0) {
    if ((n = proninx_dgram_recv(inp->dgram, &d, wait, peek)) < 0)
      return n;
    n = (int)(want < d->len ? want : d->len);
    if (d->len > want)
      *flagsp |= FNU_RECV_CUT;
    if (n > 0 && uiomove(d->data, (size_t)n, uio) != 0)
      n = -FNU_ERR_FAULT;
    if (from != 0)
      addr_of(&d->peer, from);
    proninx_dgram_free(d);
    return n;
  }
  if (want == 0)
    return 0;
  if ((buf = page_alloc()) == 0)
    return -FNU_ERR_NOMEMORY;
  while (uio->uio_resid > 0) {
    chunk = uio->uio_resid > PGSIZE ? PGSIZE : uio->uio_resid;
    n = proninx_tcp_recv(inp->tcp, buf, (uint)chunk, peek ? (uint)done : 0,
                         done == 0 || (flags & FNU_RECV_ALL) ? wait : 0, peek);
    if (n > 0 && uiomove(buf, (size_t)n, uio) != 0)
      n = -FNU_ERR_FAULT;
    if (n <= 0)
      break;
    done += (size_t)n;
  }
  page_free(buf);
  if (n < 0 && (done == 0 || n == -FNU_ERR_FAULT))
    return n;
  return (int)done;
}

static int in_shutdown(struct socket *so, int how) {
  struct inpcb *inp = so->pcb;

  if (inp->tcp == 0)
    return 0;
  return -proninx_tcp_shutdown(inp->tcp, (how & FNU_SHUTDOWN_READ) != 0,
                               (how & FNU_SHUTDOWN_WRITE) != 0);
}

static int in_ready(struct socket *so, int want) {
  struct inpcb *inp = so->pcb;
  int ready = 0;

  if (inp->tcp != 0) {
    if ((want & FNU_SEL_READ) && proninx_tcp_readable(inp->tcp))
      ready |= FNU_SEL_READ;
    if ((want & FNU_SEL_WRITE) && proninx_tcp_writable(inp->tcp))
      ready |= FNU_SEL_WRITE;
    return ready;
  }
  if ((want & FNU_SEL_READ) && proninx_dgram_readable(inp->dgram))
    ready |= FNU_SEL_READ;
  if (want & FNU_SEL_WRITE)
    ready |= FNU_SEL_WRITE;
  return ready;
}

static int64_t in_avail(struct socket *so, int write, int *eof) {
  struct inpcb *inp = so->pcb;

  if (inp->tcp != 0)
    return proninx_tcp_avail(inp->tcp, write, eof);
  *eof = 0;
  return write ? UDP_MAXDGRAM : proninx_dgram_avail(inp->dgram);
}

static int in_name(struct socket *so, struct sockaddr_any *a, int peer) {
  struct inpcb *inp = so->pcb;
  struct fnu_network_endpoint ep;
  int r;

  memset(&ep, 0, sizeof(ep));
  if (inp->tcp != 0) {
    if ((r = (peer ? proninx_tcp_peer : proninx_tcp_local)(inp->tcp, &ep)) < 0)
      return -r;
  } else if (peer) {
    if (!inp->haspeer)
      return FNU_ERR_NOTCONNECTED;
    ep = inp->peer;
  } else if ((r = proninx_dgram_local(inp->dgram, &ep)) < 0)
    return -r;
  addr_of(&ep, a);
  return 0;
}

static int in_sockaddr(struct socket *so, struct sockaddr_any *a) {
  return in_name(so, a, 0);
}

static int in_peeraddr(struct socket *so, struct sockaddr_any *a) {
  return in_name(so, a, 1);
}

static int in_ctloutput(struct socket *so, int set, int name, void *val, uint *len) {
  struct inpcb *inp = so->pcb;
  int v;

  if (name == FNU_SOCKOPT_REUSEADDR && set) {
    if (*len < sizeof(int))
      return FNU_ERR_INVALID;
    inp->reuse = *(int *)val != 0;
    return -(inp->tcp != 0 ? proninx_tcp_reuse(inp->tcp, inp->reuse)
                           : proninx_dgram_reuse(inp->dgram, inp->reuse));
  }
  if ((name == FNU_SOCKOPT_REUSEADDR || name == FNU_SOCKOPT_ERROR) && !set) {
    if (*len < sizeof(int))
      return FNU_ERR_INVALID;
    v = name == FNU_SOCKOPT_REUSEADDR ? inp->reuse
        : inp->tcp != 0          ? proninx_tcp_takeerror(inp->tcp)
                                 : 0;
    memmove(val, &v, sizeof(v));
    *len = sizeof(v);
    return 0;
  }
  return FNU_ERR_BADOPTION;
}

// the send buffer
static void in_sense(struct socket *so, struct fnu_stat *st) {
  struct inpcb *inp = so->pcb;

  if (inp != 0 && inp->tcp != 0)
    st->blksize = proninx_tcp_sndbuf(inp->tcp);
}

const struct pr_usrreqs in_usrreqs = {
    .attach = in_attach,
    .detach = in_detach,
    .bind = in_bind,
    .listen = in_listen,
    .connect = in_connect,
    .connect2 = in_connect2,
    .accept = in_accept,
    .send = in_send,
    .receive = in_receive,
    .shutdown = in_shutdown,
    .ready = in_ready,
    .avail = in_avail,
    .sockaddr = in_sockaddr,
    .peeraddr = in_peeraddr,
    .ctloutput = in_ctloutput,
    .sense = in_sense,
};
