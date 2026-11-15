#ifndef PRONINX_NET_INET_H
#define PRONINX_NET_INET_H

#include "inc/types.h"
#include "inc/abi.h"

// the sockets' side of lwip.c

struct dgram_sock;
struct tcp_sock;
struct dgram {
  struct dgram *next;
  uint len;
  struct fnu_network_endpoint peer;
  char data[];
};
int proninx_dgram_open(struct dgram_sock **);
int proninx_dgram_raw_open(struct dgram_sock **, int protocol);
int64_t proninx_dgram_avail(struct dgram_sock *u);
int64_t proninx_tcp_avail(struct tcp_sock *t, int write, int *eof);
int proninx_dgram_bind(struct dgram_sock *, const struct fnu_network_endpoint *);
int proninx_dgram_connect(struct dgram_sock *, const struct fnu_network_endpoint *);
int proninx_dgram_reuse(struct dgram_sock *, int);
int proninx_dgram_sendto(struct dgram_sock *, const void *, uint,
                       const struct fnu_network_endpoint *, int);
int proninx_dgram_recv(struct dgram_sock *, struct dgram **, uint, int);
void proninx_dgram_free(struct dgram *);
void proninx_dgram_close(struct dgram_sock *);
int proninx_dgram_readable(struct dgram_sock *);
int proninx_dgram_local(struct dgram_sock *, struct fnu_network_endpoint *);
int proninx_tcp_open(struct tcp_sock **);
int proninx_tcp_bind(struct tcp_sock *, const struct fnu_network_endpoint *);
int proninx_tcp_reuse(struct tcp_sock *, int);
int proninx_tcp_listen(struct tcp_sock *, int);
int proninx_tcp_accept(struct tcp_sock *, uint, struct tcp_sock **);
int proninx_tcp_connect(struct tcp_sock *, const struct fnu_network_endpoint *, uint);
int proninx_tcp_send(struct tcp_sock *, const void *, uint, uint);
int proninx_tcp_recv(struct tcp_sock *, void *, uint, uint, uint, int);
int proninx_tcp_shutdown(struct tcp_sock *, int, int);
void proninx_tcp_close(struct tcp_sock *);
int proninx_tcp_readable(struct tcp_sock *);
int proninx_tcp_writable(struct tcp_sock *);
int proninx_tcp_takeerror(struct tcp_sock *);
int proninx_tcp_local(struct tcp_sock *, struct fnu_network_endpoint *);
int proninx_tcp_peer(struct tcp_sock *, struct fnu_network_endpoint *);
uint proninx_tcp_sndbuf(struct tcp_sock *);

#endif
