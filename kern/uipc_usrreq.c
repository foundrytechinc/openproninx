// LOCAL SOCKETS
#include "defs.h"
#include "file.h"
#include "fs.h"
#include "malloc.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"
#include "uio.h"
#include "unveil.h"
#include "socketvar.h"
#include "inc/signal.h"
#include "inc/stat.h"

struct unp_mark {
  struct unp_mark *next;
  uint64_t at;
  struct sorights *rights;
};

// ONE WAY OF A STREAM
struct unp_sb {
  struct mutex lock;
  char *buf;
  uint size, head, cc;
  uint64_t rpos, wpos;
  struct unp_mark *marks, **marktail;
  int rlock, wlock;
  int rwant, wwant;
  int cantrcv;
  int cantsend;             // the writer is
  int refs;
  struct selinfo rsel, wsel;
};

struct unp_dgram {
  struct unp_dgram *next;
  uint len;
  struct fnu_localaddr from;
  struct sorights *rights;
  char data[];
};

struct unpcb {
  struct socket *so;
  struct unpcb *anext;
  struct unpcb *bnext;
  struct inode *ip;
  uint dev, inum;
  struct fnu_localaddr addr;
  struct unpcb *conn;
  struct unpcb *refs, *rnext;
  struct unp_sb *rcv, *snd;      // a stream's two ways
  int rdshut, wrshut;
  int listening, backlog;
  struct unpcb *acc_head, **acc_tail, *acc_next;
  int acc_count;
  struct unp_dgram *dq, **dqtail;
  uint dqbytes;
  int dqlock, dqwant;
  struct fnu_peer cred;
  int hascred;
  struct fnu_peer mycred;
  int msgcount;
  int gcdead, gcrefs;            // the collector's
  struct selinfo sel;            // a datagram's or a listener's
};

static struct {
  struct mutex lock;
  struct unpcb *bound;
  struct unpcb *all;
} unp;

static struct {
  struct mutex lock;
  int nrights;
  int gcing, again;
} unpr;

static int unp_sendspace = 8192;
static int unp_maxdgram = 8 * 1024;
static int unp_recvspace = 16 * 1024;

extern int somaxconn;

static void unp_gc(void);

void unp_init(void) {
  mtx_init(&unp.lock, "unp");
  mtx_init(&unpr.lock, "unprights");
  sysctl_add_int("net.local.stream.sendspace", &unp_sendspace,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 512, 1 << 24);
  sysctl_add_int("net.local.dgram.maxdgram", &unp_maxdgram,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 1, 1 << 24);
  sysctl_add_int("net.local.dgram.recvspace", &unp_recvspace,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 1, 1 << 24);
  sysctl_add_int("net.local.inflight", &unpr.nrights, FNU_CTLF_STDIO, 0, 0);
}

// RIGHTS IN FLIGHT

static struct unpcb *fptounp(struct file *f) {
  if (f->type != FD_SOCKET || f->sock == 0 || f->sock->domain != FNU_NET_LOCAL)
    return 0;
  return f->sock->pcb;
}

int unp_internalize(const int *fds, int n, struct sorights **out) {
  struct proc *p = curproc;
  struct sorights *r;
  struct unpcb *t;
  int i, error = 0;

  *out = 0;
  if (n == 0)
    return 0;
  r = malloc(sizeof(*r) + (size_t)n * sizeof(r->fp[0]), M_TEMP,
             M_WAITOK | M_ZERO);
  for (i = 0; i < n && error == 0; i++) {
    if ((r->fp[i] = fd_getfile(p->p_p, fds[i])) == 0) {
      error = FNU_ERR_BADHANDLE;
      break;
    }
    r->n = i + 1;
    if (r->fp[i]->type == FD_EVENTQ) {
      error = FNU_ERR_INVALID;
      break;
    }
    error = pledge_passfd(p, r->fp[i], 0);
  }
  if (error == 0) {
    mtx_enter(&unpr.lock);
    if (unpr.nrights + n > maxfiles / 10)
      error = FNU_ERR_TOOMANYOPEN;
    else {
      unpr.nrights += n;
      for (i = 0; i < n; i++)
        if ((t = fptounp(r->fp[i])) != 0)
          t->msgcount++;
    }
    mtx_leave(&unpr.lock);
  }
  if (error != 0) {
    for (i = 0; i < r->n; i++)
      FRELE(r->fp[i]);
    free(r, M_TEMP);
    return error;
  }
  *out = r;
  return 0;
}

void unp_dispose(struct sorights *r) {
  struct unpcb *t;
  int i;

  mtx_enter(&unpr.lock);
  for (i = 0; i < r->n; i++)
    if ((t = fptounp(r->fp[i])) != 0)
      t->msgcount--;
  unpr.nrights -= r->n;
  mtx_leave(&unpr.lock);
  for (i = 0; i < r->n; i++)
    FRELE(r->fp[i]);
  free(r, M_TEMP);
}

int unp_externalize(struct sorights *r, int *fds, int *n, int flags,
                    int *outflags) {
  struct process *pr = curproc->p_p;
  int i, fit, error = 0;

  fit = *n < r->n ? *n : r->n;
  if (fit < r->n)
    *outflags |= FNU_RECV_FDCUT;
  for (i = 0; i < r->n && error == 0; i++)
    error = pledge_passfd(curproc, r->fp[i], 1);
  for (i = 0; i < fit && error == 0; i++) {
    filedup(r->fp[i]);
    fds[i] = fd_alloc_at(pr, r->fp[i],
                         0, (flags & FNU_RECV_FDCLOSEEXEC) ? FNU_FD_CLOSEEXEC : 0);
    if (fds[i] < 0) {
      FRELE(r->fp[i]);
      while (--i >= 0)
        fd_untake(pr, fds[i], r->fp[i]);
      error = FNU_ERR_MSGTOOBIG;
    }
  }
  unp_dispose(r);
  *n = error != 0 ? 0 : fit;
  return error;
}

// THE COLLECTOR

static void unp_scan(struct unpcb *u, void (*fn)(struct unpcb *)) {
  struct unp_dgram *d;
  struct unp_mark *m;
  struct unpcb *t;
  int i;

  if (u->so->type == FNU_SOCKET_STREAM) {
    if (u->rcv == 0)
      return;
    for (m = u->rcv->marks; m != 0; m = m->next)
      for (i = 0; i < m->rights->n; i++)
        if ((t = fptounp(m->rights->fp[i])) != 0)
          fn(t);
    return;
  }
  for (d = u->dq; d != 0; d = d->next)
    if (d->rights != 0)
      for (i = 0; i < d->rights->n; i++)
        if ((t = fptounp(d->rights->fp[i])) != 0)
          fn(t);
}

static int gc_defer;

static void marks_dispose(struct unp_mark *m);

static void gc_remove(struct unpcb *t) {
  if (t->gcdead)
    t->gcrefs--;
}

static void gc_restore(struct unpcb *t) {
  if (t->gcdead) {
    t->gcrefs++;
    gc_defer++;
  }
}

static void gc_take(struct unpcb *u, struct unp_mark **marks,
                    struct unp_dgram **dgrams) {
  struct unp_dgram *d;
  struct unp_mark *m;

  if (u->so->type == FNU_SOCKET_STREAM) {
    if (u->rcv == 0)
      return;
    mtx_enter(&u->rcv->lock);
    mtx_enter(&unpr.lock);
    if ((m = u->rcv->marks) != 0) {
      *u->rcv->marktail = *marks;
      *marks = m;
      u->rcv->marks = 0;
      u->rcv->marktail = &u->rcv->marks;
    }
    mtx_leave(&unpr.lock);
    mtx_leave(&u->rcv->lock);
    return;
  }
  mtx_enter(&unpr.lock);
  if ((d = u->dq) != 0) {
    *u->dqtail = *dgrams;
    *dgrams = d;
    u->dq = 0;
    u->dqtail = &u->dq;
    u->dqbytes = 0;
  }
  mtx_leave(&unpr.lock);
}

static void unp_gc(void) {
  struct unp_dgram *dgrams, *d;
  struct unp_mark *marks;
  struct unpcb *u;
  int nunref;

  mtx_enter(&unpr.lock);
  if (unpr.gcing) {
    unpr.again = 1;
    mtx_leave(&unpr.lock);
    return;
  }
  unpr.gcing = 1;
  do {
    unpr.again = 0;
    mtx_leave(&unpr.lock);

    marks = 0;
    dgrams = 0;
    nunref = 0;
    mtx_enter(&unp.lock);
    mtx_enter(&unpr.lock);
    for (u = unp.all; u != 0; u = u->anext) {
      u->gcdead = 0;
      if (u->msgcount == 0 || u->so->file == 0)
        continue;
      if (file_refs(u->so->file) == u->msgcount) {
        u->gcdead = 1;
        u->gcrefs = u->msgcount;
        nunref++;
      }
    }
    for (u = unp.all; u != 0; u = u->anext)
      if (u->gcdead)
        unp_scan(u, gc_remove);
    do {
      gc_defer = 0;
      for (u = unp.all; u != 0; u = u->anext) {
        if (!u->gcdead || u->gcrefs == 0)
          continue;
        u->gcdead = 0;
        unp_scan(u, gc_restore);
        nunref--;
      }
    } while (gc_defer > 0);
    mtx_leave(&unpr.lock);
    if (nunref > 0)
      for (u = unp.all; u != 0; u = u->anext)
        if (u->gcdead)
          gc_take(u, &marks, &dgrams);
    mtx_leave(&unp.lock);

    marks_dispose(marks);
    while ((d = dgrams) != 0) {
      dgrams = d->next;
      if (d->rights != 0)
        unp_dispose(d->rights);
      free(d, M_TEMP);
    }
    mtx_enter(&unpr.lock);
  } while (unpr.again);
  unpr.gcing = 0;
  mtx_leave(&unpr.lock);
}

// STREAM BUFFERS

static struct unp_sb *sb_new(void) {
  struct unp_sb *sb = malloc(sizeof(*sb), M_FILE, M_WAITOK | M_ZERO);

  if (sb == 0)
    return 0;
  sb->size = (uint)unp_sendspace;
  if ((sb->buf = malloc(sb->size, M_FILE, M_WAITOK)) == 0) {
    free(sb, M_FILE);
    return 0;
  }
  mtx_init(&sb->lock, "unpsb");
  sb->marktail = &sb->marks;
  sb->refs = 2;
  return sb;
}

static void marks_dispose(struct unp_mark *m) {
  struct unp_mark *next;

  for (; m != 0; m = next) {
    next = m->next;
    unp_dispose(m->rights);
    free(m, M_TEMP);
  }
}

static void sb_shut(struct unp_sb *sb, int reader) {
  struct unp_mark *m = 0;

  mtx_enter(&sb->lock);
  if (reader) {
    sb->cantrcv = 1;
    mtx_enter(&unpr.lock);
    m = sb->marks;
    sb->marks = 0;
    sb->marktail = &sb->marks;
    mtx_leave(&unpr.lock);
  } else
    sb->cantsend = 1;
  wakeup(&sb->cc);
  mtx_leave(&sb->lock);
  marks_dispose(m);
  selwakeup(&sb->rsel);
  selwakeup(&sb->wsel);
}

static void sb_rele(struct unp_sb *sb) {
  int last;

  mtx_enter(&sb->lock);
  last = --sb->refs == 0;
  mtx_leave(&sb->lock);
  if (!last)
    return;
  marks_dispose(sb->marks);
  free(sb->buf, M_FILE);
  free(sb, M_FILE);
}

// lock held
static int sblock(struct unp_sb *sb, int *held, int *want, int flags) {
  int r;

  while (*held) {
    if (flags & FNU_MSG_NOWAIT)
      return FNU_ERR_WOULDBLOCK;
    *want = 1;
    if ((r = msleep(held, &sb->lock, PSOCK | PCATCH, "sblock", 0)) != 0)
      return r;
  }
  *held = 1;
  return 0;
}

static void sbunlock(int *held, int *want) {
  *held = 0;
  if (*want) {
    *want = 0;
    wakeup(held);
  }
}

static void sb_mark(struct unp_sb *sb, struct unp_mark *m) {
  mtx_enter(&unpr.lock);
  m->at = sb->wpos;
  m->next = 0;
  *sb->marktail = m;
  sb->marktail = &m->next;
  mtx_leave(&unpr.lock);
}

static int stream_send(struct socket *so, struct uio *uio,
                       struct sorights *rights, int flags) {
  struct unpcb *u = so->pcb;
  struct unp_sb *sb = u->snd;
  struct unp_mark *m = 0;
  size_t done = 0, n;
  uint tail;
  int r;

  if (sb == 0) {
    if (rights != 0)
      unp_dispose(rights);
    return -FNU_ERR_NOTCONNECTED;
  }
  if (rights != 0) {
    m = malloc(sizeof(*m), M_TEMP, M_WAITOK);
    m->rights = rights;
  }
  mtx_enter(&sb->lock);
  if ((r = sblock(sb, &sb->wlock, &sb->wwant, flags)) == 0) {
    for (;;) {
      if (uio->uio_resid == 0 && m == 0)
        break;
      if (sb->cantrcv || u->wrshut) {
        r = FNU_ERR_BROKENPIPE;
        break;
      }
      if (uio->uio_resid == 0) {
        sb_mark(sb, m);
        m = 0;
        wakeup(&sb->cc);
        selwakeup(&sb->rsel);
        break;
      }
      if (sb->cc == sb->size) {
        if (flags & FNU_MSG_NOWAIT) {
          r = FNU_ERR_WOULDBLOCK;
          break;
        }
        if ((r = msleep(&sb->cc, &sb->lock, PSOCK | PCATCH, "unpsnd",
                        so_ticks(so->sndtimeo))) != 0)
          break;
        continue;
      }
      tail = (sb->head + sb->cc) % sb->size;
      n = sb->size - sb->cc;
      if (n > sb->size - tail)
        n = sb->size - tail;
      if (n > uio->uio_resid)
        n = uio->uio_resid;
      mtx_leave(&sb->lock);
      r = uiomove(sb->buf + tail, n, uio);
      mtx_enter(&sb->lock);
      if (r != 0)
        break;
      if (m != 0) {
        sb_mark(sb, m);
        m = 0;
      }
      sb->cc += (uint)n;
      sb->wpos += n;
      done += n;
      wakeup(&sb->cc);
      selwakeup(&sb->rsel);
    }
    sbunlock(&sb->wlock, &sb->wwant);
  }
  mtx_leave(&sb->lock);
  if (m != 0) {
    unp_dispose(m->rights);
    free(m, M_TEMP);
  }
  if (done > 0 && (r == FNU_ERR_INTERRUPTED || r == FNU_ERR_WOULDBLOCK))
    r = 0;
  return r != 0 ? -r : (int)done;
}

static int stream_receive(struct socket *so, struct uio *uio,
                          struct sorights **rightsp, int *flagsp) {
  struct unpcb *u = so->pcb;
  struct unp_sb *sb = u->rcv;
  struct unp_mark *m, *took = 0, *passed = 0;
  int flags = *flagsp, peek = flags & FNU_RECV_PEEK, r = 0;
  uint64_t at, end;
  size_t done = 0, n;
  uint pos;

  *flagsp = 0;
  if (sb == 0)
    return -FNU_ERR_NOTCONNECTED;
  mtx_enter(&sb->lock);
  if ((r = sblock(sb, &sb->rlock, &sb->rwant, flags)) != 0) {
    mtx_leave(&sb->lock);
    return -r;
  }
  at = sb->rpos;
  for (;;) {
    for (m = sb->marks; m != 0 && (m->at < at || m == passed); m = m->next)
      ;
    if (m != 0 && m->at == at) {
      if (done > 0 || took != 0 || passed != 0)
        break;
      if (peek)
        passed = m;
      else {
        mtx_enter(&unpr.lock);
        if ((sb->marks = m->next) == 0)
          sb->marktail = &sb->marks;
        mtx_leave(&unpr.lock);
        took = m;
      }
      continue;
    }
    end = sb->rpos + sb->cc;
    if (m != 0 && m->at < end)
      end = m->at;
    if (end > at) {
      pos = (uint)((sb->head + (at - sb->rpos)) % sb->size);
      n = (size_t)(end - at);
      if (n > sb->size - pos)
        n = sb->size - pos;
      if (n > uio->uio_resid)
        n = uio->uio_resid;
      mtx_leave(&sb->lock);
      r = uiomove(sb->buf + pos, n, uio);
      mtx_enter(&sb->lock);
      if (r != 0)
        break;
      if (!peek) {
        sb->head = (sb->head + (uint)n) % sb->size;
        sb->cc -= (uint)n;
        sb->rpos += n;
        wakeup(&sb->cc);
        selwakeup(&sb->wsel);
      }
      at += n;
      done += n;
      if (uio->uio_resid == 0)
        break;
      continue;
    }
    if (uio->uio_resid == 0 || (m != 0 && m->at == at))
      break;
    if ((took != 0 || passed != 0) && done == 0)
      break;
    if (done > 0 && (flags & FNU_RECV_ALL) == 0)
      break;
    if (sb->cantsend || u->rdshut)
      break;
    if (peek && at - sb->rpos >= sb->size)
      break;
    if (flags & FNU_MSG_NOWAIT) {
      if (done == 0)
        r = FNU_ERR_WOULDBLOCK;
      break;
    }
    if ((r = msleep(&sb->cc, &sb->lock, PSOCK | PCATCH, "unprcv",
                    so_ticks(so->rcvtimeo))) != 0)
      break;
  }
  sbunlock(&sb->rlock, &sb->rwant);
  mtx_leave(&sb->lock);
  if (r != 0 && (done > 0 || took != 0) && r != FNU_ERR_FAULT)
    r = 0;
  if (took != 0) {
    if (rightsp != 0 && r == 0)
      *rightsp = took->rights;
    else
      unp_dispose(took->rights);
    free(took, M_TEMP);
  }
  return r != 0 ? -r : (int)done;
}

// SOCKETS

static int path_of(const struct sockaddr_any *a, char *path) {
  int n = (int)a->sa.len - (int)__offsetof(struct fnu_localaddr, path), i;

  if (a->sa.family != FNU_NET_LOCAL)
    return FNU_ERR_NOFAMILY;
  if (n <= 0)
    return FNU_ERR_INVALID;
  if (n > (int)sizeof(a->un.path))
    n = (int)sizeof(a->un.path);
  for (i = 0; i < n && a->un.path[i] != 0; i++)
    path[i] = a->un.path[i];
  if (i == 0)
    return FNU_ERR_INVALID;
  path[i] = 0;
  return 0;
}

static void noname(struct sockaddr_any *a) {
  memset(&a->un, 0, sizeof(a->un));
  a->un.len = (uint8)__offsetof(struct fnu_localaddr, path);
  a->un.family = FNU_NET_LOCAL;
}

static void mycred(struct fnu_peer *c) {
  struct process *pr = curproc->p_p;

  c->uid = pr->uid;
  c->gid = pr->gid;
  c->pid = pr->pid;
}

static int unp_attach(struct socket *so) {
  struct unpcb *u;

  if (so->type != FNU_SOCKET_STREAM && so->type != FNU_SOCKET_DATAGRAM)
    return FNU_ERR_WRONGTYPE;
  u = malloc(sizeof(*u), M_FILE, M_WAITOK | M_ZERO);
  u->so = so;
  u->acc_tail = &u->acc_head;
  u->dqtail = &u->dq;
  so->pcb = u;
  mtx_enter(&unp.lock);
  u->anext = unp.all;
  unp.all = u;
  mtx_leave(&unp.lock);
  return 0;
}

// lock held
static struct unpcb *bound_at(uint dev, uint inum) {
  struct unpcb *u;

  for (u = unp.bound; u != 0; u = u->bnext)
    if (u->dev == dev && u->inum == inum)
      return u;
  return 0;
}

// lock held
static void dgram_disconnect(struct unpcb *u) {
  struct unpcb **pp;

  if (u->conn == 0)
    return;
  for (pp = &u->conn->refs; *pp != 0; pp = &(*pp)->rnext)
    if (*pp == u) {
      *pp = u->rnext;
      break;
    }
  u->conn = 0;
  u->rnext = 0;
}

static void unlink_from(struct unpcb **pp, struct unpcb *u, int bound) {
  for (; *pp != 0; pp = bound ? &(*pp)->bnext : &(*pp)->anext)
    if (*pp == u) {
      *pp = bound ? u->bnext : u->anext;
      return;
    }
}

static void unp_detach(struct socket *so) {
  struct unpcb *u = so->pcb, *pending, *r;
  struct unp_dgram *d;
  struct inode *ip;
  int inflight;

  mtx_enter(&unp.lock);
  unlink_from(&unp.bound, u, 1);
  unlink_from(&unp.all, u, 0);
  ip = u->ip;
  u->ip = 0;
  if (so->type == FNU_SOCKET_DATAGRAM) {
    dgram_disconnect(u);
    while ((r = u->refs) != 0) {
      u->refs = r->rnext;
      r->conn = 0;
      r->rnext = 0;
    }
  } else if (u->conn != 0) {
    u->conn->conn = 0;
    u->conn = 0;
  }
  pending = u->acc_head;
  u->acc_head = 0;
  u->acc_tail = &u->acc_head;
  mtx_leave(&unp.lock);

  if (u->rcv != 0) {
    if (!u->rdshut)
      sb_shut(u->rcv, 1);
    sb_rele(u->rcv);
  }
  if (u->snd != 0) {
    if (!u->wrshut)
      sb_shut(u->snd, 0);
    sb_rele(u->snd);
  }
  while (pending != 0) {
    struct unpcb *next = pending->acc_next;

    sofree(pending->so);
    pending = next;
  }
  while ((d = u->dq) != 0) {
    u->dq = d->next;
    if (d->rights != 0)
      unp_dispose(d->rights);
    free(d, M_TEMP);
  }
  if (ip != 0)
    iput(ip);
  free(u, M_FILE);
  mtx_enter(&unpr.lock);
  inflight = unpr.nrights;
  mtx_leave(&unpr.lock);
  if (inflight > 0)
    unp_gc();
}

static int unp_bind(struct socket *so, const struct sockaddr_any *a) {
  struct unpcb *u = so->pcb;
  char path[sizeof(a->un.path) + 1];
  struct inode *ip;
  int error;

  if ((error = path_of(a, path)) != 0)
    return error;
  if (u->addr.len != 0)
    return FNU_ERR_INVALID;
  if ((ip = vn_mksock(path, &error)) == 0)
    return error == FNU_ERR_EXISTS ? FNU_ERR_ADDRINUSE : error;
  iunlock(ip);
  mtx_enter(&unp.lock);
  if (u->addr.len != 0) {
    mtx_leave(&unp.lock);
    iput(ip);
    return FNU_ERR_INVALID;
  }
  u->ip = ip;
  u->dev = ip->dev;
  u->inum = ip->inum;
  memset(&u->addr, 0, sizeof(u->addr));
  u->addr.family = FNU_NET_LOCAL;
  strlcpy(u->addr.path, path, sizeof(u->addr.path));
  u->addr.len = (uint8)(__offsetof(struct fnu_localaddr, path) + strlen(path) + 1);
  u->bnext = unp.bound;
  unp.bound = u;
  mtx_leave(&unp.lock);
  return 0;
}

static int unp_listen(struct socket *so, int backlog) {
  struct unpcb *u = so->pcb;

  if (so->type != FNU_SOCKET_STREAM)
    return FNU_ERR_UNSUPPORTED;
  if (u->addr.len == 0 || u->conn != 0 || u->rcv != 0)
    return FNU_ERR_INVALID;
  if (backlog < 1 || backlog > somaxconn)
    backlog = somaxconn;
  mtx_enter(&unp.lock);
  u->listening = 1;
  u->backlog = backlog;
  mycred(&u->mycred);
  mtx_leave(&unp.lock);
  return 0;
}

static int lookup(const struct sockaddr_any *a, uint *dev, uint *inum) {
  char path[sizeof(a->un.path) + 1];
  struct inode *ip;
  int error;

  if ((error = path_of(a, path)) != 0)
    return error;
  if ((error = namei_at(0, path, UV_WRITE, &ip)) != 0)
    return error;
  ilock(ip);
  if (ip->type != FNU_TYPE_SOCKET)
    error = FNU_ERR_NOTSOCKET;
  else if (!inode_access(ip, curproc->p_p->uid, 2))
    error = FNU_ERR_DENIED;
  *dev = ip->dev;
  *inum = ip->inum;
  iunlockput(ip);
  return error;
}

// lock held
static void stream_join(struct unpcb *client, struct unpcb *server,
                        struct unp_sb *up, struct unp_sb *down) {
  client->snd = up;
  server->rcv = up;
  client->rcv = down;
  server->snd = down;
  client->conn = server;
  server->conn = client;
}

static void sb_unmade(struct unp_sb *up, struct unp_sb *down) {
  if (up != 0) {
    free(up->buf, M_FILE);
    free(up, M_FILE);
  }
  if (down != 0) {
    free(down->buf, M_FILE);
    free(down, M_FILE);
  }
}

static int unp_connect(struct socket *so, const struct sockaddr_any *a, int nonblock) {
  struct unpcb *u = so->pcb, *l, *s;
  struct unp_sb *up = 0, *down = 0;
  struct socket *ns = 0;
  uint dev, inum;
  int error;

  (void)nonblock;
  if ((error = lookup(a, &dev, &inum)) != 0)
    return error;
  if (so->type == FNU_SOCKET_STREAM) {
    if (u->listening)
      return FNU_ERR_UNSUPPORTED;
    ns = soalloc(FNU_NET_LOCAL, FNU_SOCKET_STREAM, &unp_usrreqs);
    if ((error = unp_attach(ns)) != 0 || (up = sb_new()) == 0 ||
        (down = sb_new()) == 0) {
      sb_unmade(up, down);
      if (error == 0)
        unp_detach(ns);
      free(ns, M_FILE);
      return error != 0 ? error : FNU_ERR_NOBUFFERS;
    }
  }
  mtx_enter(&unp.lock);
  if ((l = bound_at(dev, inum)) == 0)
    error = FNU_ERR_REFUSED;
  else if (l->so->type != so->type)
    error = FNU_ERR_WRONGTYPE;
  else if (so->type == FNU_SOCKET_DATAGRAM) {
    dgram_disconnect(u);
    u->conn = l;
    u->rnext = l->refs;
    l->refs = u;
  } else if (u->conn != 0 || u->rcv != 0)
    error = FNU_ERR_CONNECTED;
  else if (!l->listening || l->acc_count >= l->backlog)
    error = FNU_ERR_REFUSED;
  else {
    s = ns->pcb;
    stream_join(u, s, up, down);
    s->addr = l->addr;
    mycred(&s->cred);
    s->hascred = 1;
    u->cred = l->mycred;
    u->hascred = 1;
    *l->acc_tail = s;
    l->acc_tail = &s->acc_next;
    l->acc_count++;
    wakeup(l);
    selwakeup(&l->sel);
    ns = 0;
  }
  mtx_leave(&unp.lock);
  if (ns != 0) {
    sb_unmade(up, down);
    unp_detach(ns);
    free(ns, M_FILE);
  }
  return error;
}

static int unp_connect2(struct socket *a, struct socket *b) {
  struct unpcb *ua = a->pcb, *ub = b->pcb;
  struct unp_sb *up = 0, *down = 0;

  if (a->type == FNU_SOCKET_STREAM &&
      ((up = sb_new()) == 0 || (down = sb_new()) == 0)) {
    sb_unmade(up, down);
    return FNU_ERR_NOBUFFERS;
  }
  mtx_enter(&unp.lock);
  if (a->type == FNU_SOCKET_STREAM)
    stream_join(ua, ub, up, down);
  else {
    ua->conn = ub;
    ua->rnext = ub->refs;
    ub->refs = ua;
    ub->conn = ua;
    ub->rnext = ua->refs;
    ua->refs = ub;
  }
  mycred(&ua->cred);
  ub->cred = ua->cred;
  ua->hascred = ub->hascred = 1;
  mtx_leave(&unp.lock);
  return 0;
}

static int unp_accept(struct socket *so, int nonblock, struct socket **out) {
  struct unpcb *u = so->pcb, *s;
  int r;

  if (!u->listening)
    return FNU_ERR_INVALID;
  mtx_enter(&unp.lock);
  while ((s = u->acc_head) == 0) {
    if (nonblock) {
      mtx_leave(&unp.lock);
      return FNU_ERR_WOULDBLOCK;
    }
    r = msleep(u, &unp.lock, PSOCK | PCATCH, "accept", so_ticks(so->rcvtimeo));
    if (r != 0) {
      mtx_leave(&unp.lock);
      return r;
    }
  }
  if ((u->acc_head = s->acc_next) == 0)
    u->acc_tail = &u->acc_head;
  s->acc_next = 0;
  u->acc_count--;
  mtx_leave(&unp.lock);
  *out = s->so;
  return 0;
}

// DATAGRAMS

static int send_dgram(struct socket *so, struct uio *uio, const struct sockaddr_any *to,
                      struct sorights *rights) {
  struct unpcb *u = so->pcb, *t;
  struct unp_dgram *d;
  size_t len = uio->uio_resid;
  uint dev = 0, inum = 0;
  int error;

  if (u->wrshut)
    error = FNU_ERR_BROKENPIPE;
  else if (len > (size_t)unp_maxdgram)
    error = FNU_ERR_MSGTOOBIG;
  else
    error = to != 0 ? lookup(to, &dev, &inum) : 0;
  if (error != 0) {
    if (rights != 0)
      unp_dispose(rights);
    return -error;
  }
  d = malloc(sizeof(*d) + (len > 0 ? len : 1), M_TEMP, M_WAITOK);
  d->next = 0;
  d->len = (uint)len;
  d->rights = rights;
  if (uiomove(d->data, len, uio) != 0) {
    error = FNU_ERR_FAULT;
    goto fail;
  }
  mtx_enter(&unp.lock);
  if (u->addr.len != 0)
    d->from = u->addr;
  else {
    memset(&d->from, 0, sizeof(d->from));
    d->from.len = (uint8)__offsetof(struct fnu_localaddr, path);
    d->from.family = FNU_NET_LOCAL;
  }
  t = to != 0 ? bound_at(dev, inum) : u->conn;
  if (t == 0 || t->so->type != FNU_SOCKET_DATAGRAM)
    error = to != 0 ? (t == 0 ? FNU_ERR_REFUSED : FNU_ERR_WRONGTYPE) : FNU_ERR_NOTCONNECTED;
  else if (t->dqbytes + d->len > (uint)unp_recvspace)
    error = FNU_ERR_NOBUFFERS;
  else {
    mtx_enter(&unpr.lock);
    *t->dqtail = d;
    t->dqtail = &d->next;
    mtx_leave(&unpr.lock);
    t->dqbytes += d->len;
    wakeup(t);
    selwakeup(&t->sel);
    d = 0;
  }
  mtx_leave(&unp.lock);
  if (d == 0)
    return (int)len;
fail:
  if (d->rights != 0)
    unp_dispose(d->rights);
  free(d, M_TEMP);
  return -error;
}

static int receive_dgram(struct socket *so, struct uio *uio, struct sockaddr_any *from,
                         struct sorights **rightsp, int *flagsp) {
  struct unpcb *u = so->pcb;
  int flags = *flagsp, peek = flags & FNU_RECV_PEEK, r = 0, n = 0;
  struct unp_dgram *d = 0;

  *flagsp = 0;
  mtx_enter(&unp.lock);
  while (u->dqlock) {
    if (flags & FNU_MSG_NOWAIT) {
      mtx_leave(&unp.lock);
      return -FNU_ERR_WOULDBLOCK;
    }
    u->dqwant = 1;
    if ((r = msleep(&u->dqlock, &unp.lock, PSOCK | PCATCH, "sblock", 0)) != 0) {
      mtx_leave(&unp.lock);
      return -r;
    }
  }
  u->dqlock = 1;
  while ((d = u->dq) == 0) {
    if (u->rdshut)
      break;
    if (flags & FNU_MSG_NOWAIT) {
      r = FNU_ERR_WOULDBLOCK;
      break;
    }
    if ((r = msleep(u, &unp.lock, PSOCK | PCATCH, "unpdgram",
                    so_ticks(so->rcvtimeo))) != 0)
      break;
  }
  if (d != 0 && !peek) {
    mtx_enter(&unpr.lock);
    if ((u->dq = d->next) == 0)
      u->dqtail = &u->dq;
    mtx_leave(&unpr.lock);
    u->dqbytes -= d->len;
  }
  mtx_leave(&unp.lock);
  if (d != 0) {
    n = (int)(uio->uio_resid < d->len ? uio->uio_resid : d->len);
    if (d->len > uio->uio_resid)
      *flagsp |= FNU_RECV_CUT;
    if (n > 0 && uiomove(d->data, (size_t)n, uio) != 0)
      r = FNU_ERR_FAULT;
    if (from != 0)
      memmove(&from->un, &d->from, sizeof(d->from));
  }
  mtx_enter(&unp.lock);
  u->dqlock = 0;
  if (u->dqwant) {
    u->dqwant = 0;
    wakeup(&u->dqlock);
  }
  mtx_leave(&unp.lock);
  if (d != 0 && !peek) {
    if (d->rights != 0) {
      if (rightsp != 0 && r == 0)
        *rightsp = d->rights;
      else
        unp_dispose(d->rights);
    }
    free(d, M_TEMP);
  }
  return r != 0 ? -r : n;
}

static int unp_send(struct socket *so, struct uio *uio, const struct sockaddr_any *to,
                    struct sorights *rights, int flags) {
  struct unpcb *u = so->pcb;

  if (so->type == FNU_SOCKET_DATAGRAM)
    return send_dgram(so, uio, to, rights);
  if (to != 0) {
    if (rights != 0)
      unp_dispose(rights);
    return u->rcv != 0 ? -FNU_ERR_CONNECTED : -FNU_ERR_UNSUPPORTED;
  }
  return stream_send(so, uio, rights, flags);
}

static int unp_receive(struct socket *so, struct uio *uio, struct sockaddr_any *from,
                       struct sorights **rightsp, int *flagsp) {
  struct unpcb *u = so->pcb;

  if (so->type == FNU_SOCKET_DATAGRAM)
    return receive_dgram(so, uio, from, rightsp, flagsp);
  if (from != 0)
    noname(from);
  if (u->rcv == 0)
    return -FNU_ERR_NOTCONNECTED;
  if (u->rdshut) {
    *flagsp = 0;
    return 0;
  }
  return stream_receive(so, uio, rightsp, flagsp);
}

static int unp_shutdown(struct socket *so, int how) {
  struct unpcb *u = so->pcb;

  if (so->type == FNU_SOCKET_STREAM && u->rcv == 0)
    return FNU_ERR_NOTCONNECTED;
  if ((how & FNU_SHUTDOWN_READ) && !u->rdshut) {
    u->rdshut = 1;
    if (u->rcv != 0)
      sb_shut(u->rcv, 1);
  }
  if ((how & FNU_SHUTDOWN_WRITE) && !u->wrshut) {
    u->wrshut = 1;
    if (u->snd != 0)
      sb_shut(u->snd, 0);
  }
  mtx_enter(&unp.lock);
  wakeup(u);
  selwakeup(&u->sel);
  mtx_leave(&unp.lock);
  return 0;
}

static int unp_ready(struct socket *so, int want) {
  struct unpcb *u = so->pcb;
  int ready = 0;

  if (so->type == FNU_SOCKET_DATAGRAM) {
    mtx_enter(&unp.lock);
    if ((want & FNU_SEL_READ) && (u->dq != 0 || u->rdshut))
      ready |= FNU_SEL_READ;
    else if (want & FNU_SEL_READ)
      selrecord(&u->sel);
    mtx_leave(&unp.lock);
    return ready | (want & FNU_SEL_WRITE);
  }
  if (u->listening) {
    mtx_enter(&unp.lock);
    if ((want & FNU_SEL_READ) && u->acc_head != 0)
      ready |= FNU_SEL_READ;
    else if (want & FNU_SEL_READ)
      selrecord(&u->sel);
    mtx_leave(&unp.lock);
    return ready;
  }
  if (u->rcv == 0)
    return want & FNU_SEL_WRITE;
  if (want & FNU_SEL_READ) {
    mtx_enter(&u->rcv->lock);
    if (u->rcv->cc > 0 || u->rcv->marks != 0 || u->rcv->cantsend || u->rdshut)
      ready |= FNU_SEL_READ;
    else
      selrecord(&u->rcv->rsel);
    mtx_leave(&u->rcv->lock);
  }
  if (want & FNU_SEL_WRITE) {
    mtx_enter(&u->snd->lock);
    if (u->snd->cc < u->snd->size || u->snd->cantrcv || u->wrshut)
      ready |= FNU_SEL_WRITE;
    else
      selrecord(&u->snd->wsel);
    mtx_leave(&u->snd->lock);
  }
  return ready;
}

static int64_t unp_avail(struct socket *so, int write, int *eof) {
  struct unpcb *u = so->pcb;
  int64_t n = 0;

  *eof = 0;
  if (so->type == FNU_SOCKET_DATAGRAM) {
    if (write)
      return unp_maxdgram;
    mtx_enter(&unp.lock);
    n = u->dq != 0 ? u->dq->len : 0;
    *eof = u->rdshut;
    mtx_leave(&unp.lock);
    return n;
  }
  if (u->listening) {
    mtx_enter(&unp.lock);
    n = write ? 0 : u->acc_count;
    mtx_leave(&unp.lock);
    return n;
  }
  if (u->rcv == 0)
    return 0;
  if (write) {
    mtx_enter(&u->snd->lock);
    n = u->snd->size - u->snd->cc;
    *eof = u->snd->cantrcv || u->wrshut;
    mtx_leave(&u->snd->lock);
  } else {
    mtx_enter(&u->rcv->lock);
    n = u->rcv->cc;
    *eof = u->rcv->cantsend || u->rdshut;
    mtx_leave(&u->rcv->lock);
  }
  return n;
}

static int unp_sockaddr(struct socket *so, struct sockaddr_any *a) {
  struct unpcb *u = so->pcb;

  mtx_enter(&unp.lock);
  if (u->addr.len != 0)
    memmove(&a->un, &u->addr, sizeof(u->addr));
  else
    noname(a);
  mtx_leave(&unp.lock);
  return 0;
}

static int unp_peeraddr(struct socket *so, struct sockaddr_any *a) {
  struct unpcb *u = so->pcb;
  int error = 0;

  mtx_enter(&unp.lock);
  if (u->conn == 0)
    error = FNU_ERR_NOTCONNECTED;
  else if (u->conn->addr.len != 0)
    memmove(&a->un, &u->conn->addr, sizeof(u->conn->addr));
  else
    noname(a);
  mtx_leave(&unp.lock);
  return error;
}

// SO_PEERCRED
static int unp_ctloutput(struct socket *so, int set, int name, void *val, uint *len) {
  struct unpcb *u = so->pcb;
  int error = 0;

  if (name != FNU_SOCKOPT_PEER || set)
    return FNU_ERR_BADOPTION;
  if (*len < sizeof(struct fnu_peer))
    return FNU_ERR_INVALID;
  mtx_enter(&unp.lock);
  if (!u->hascred)
    error = FNU_ERR_NOTCONNECTED;
  else {
    memmove(val, &u->cred, sizeof(u->cred));
    *len = sizeof(u->cred);
  }
  mtx_leave(&unp.lock);
  return error;
}

// a stream's send buffer
static void unp_sense(struct socket *so, struct fnu_stat *st) {
  struct unpcb *u = so->pcb;

  if (so->type != FNU_SOCKET_DATAGRAM)
    st->blksize = u != 0 && u->snd != 0 ? u->snd->size : (uint)unp_sendspace;
}

const struct pr_usrreqs unp_usrreqs = {
    .attach = unp_attach,
    .detach = unp_detach,
    .bind = unp_bind,
    .listen = unp_listen,
    .connect = unp_connect,
    .connect2 = unp_connect2,
    .accept = unp_accept,
    .send = unp_send,
    .receive = unp_receive,
    .shutdown = unp_shutdown,
    .ready = unp_ready,
    .avail = unp_avail,
    .sockaddr = unp_sockaddr,
    .peeraddr = unp_peeraddr,
    .ctloutput = unp_ctloutput,
    .sense = unp_sense,
};
