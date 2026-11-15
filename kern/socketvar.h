#ifndef PRONINX_SOCKETVAR_H
#define PRONINX_SOCKETVAR_H

#include "inc/types.h"
#include "inc/abi.h"

struct file;
struct uio;
struct socket;

// SCM_RIGHTS
struct sorights {
  int n;
  struct file *fp[];
};

struct sockaddr_any {
  union {
    struct {
      uint8 len;
      uint8 family;
    } sa;
    struct fnu_inetaddr in;
    struct fnu_localaddr un;
    uint8 raw[FNU_NETADDR_MAX + 1];
  };
};

struct pr_usrreqs {
  int (*attach)(struct socket *so);
  void (*detach)(struct socket *so);
  int (*bind)(struct socket *so, const struct sockaddr_any *a);
  int (*listen)(struct socket *so, int backlog);
  int (*connect)(struct socket *so, const struct sockaddr_any *a, int nonblock);
  int (*connect2)(struct socket *a, struct socket *b);
  int (*accept)(struct socket *so, int nonblock, struct socket **out);
  int (*send)(struct socket *so, struct uio *uio, const struct sockaddr_any *to,
              struct sorights *rights, int flags);
  int (*receive)(struct socket *so, struct uio *uio, struct sockaddr_any *from,
                 struct sorights **rights, int *flagsp);
  int (*shutdown)(struct socket *so, int how);
  int (*ready)(struct socket *so, int want);
  int64_t (*avail)(struct socket *so, int write, int *eof);
  int (*sockaddr)(struct socket *so, struct sockaddr_any *a);
  int (*peeraddr)(struct socket *so, struct sockaddr_any *a);
  int (*ctloutput)(struct socket *so, int set, int name, void *val, uint *len);
  void (*sense)(struct socket *so, struct fnu_stat *st);
};

struct socket {
  int domain;
  int type;
  int protocol;
  const struct pr_usrreqs *usrreqs;
  void *pcb;
  struct file *file;
  uint rcvtimeo; // ms, 0 for ever
  uint sndtimeo;
};

struct socket *soalloc(int domain, int type, const struct pr_usrreqs *usrreqs);
void sofree(struct socket *so);
int so_ticks(uint ms);

extern const struct pr_usrreqs in_usrreqs;
extern const struct pr_usrreqs unp_usrreqs;
void unp_init(void);
int unp_internalize(const int *fds, int n, struct sorights **out);
int unp_externalize(struct sorights *rights, int *fds, int *n, int flags,
                    int *outflags);
void unp_dispose(struct sorights *rights);

#endif
