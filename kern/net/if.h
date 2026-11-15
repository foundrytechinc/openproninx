#ifndef PRONINX_NET_IF_H
#define PRONINX_NET_IF_H

#include "inc/types.h"

#define IFNAMSIZ 16
#define ETHER_ADDR_LEN 6
#define ETHER_HDR_LEN 14
#define ETHERMTU 1500
#define ETHER_MAX_LEN 1518

#define LINK_STATE_UNKNOWN 0
#define LINK_STATE_DOWN 2
#define LINK_STATE_UP 4
#define LINK_STATE_FULL_DUPLEX 6
#define LINK_STATE_IS_UP(s) ((s) >= LINK_STATE_UP || (s) == LINK_STATE_UNKNOWN)

struct ifnet {
  struct ifnet *if_next;
  char if_xname[IFNAMSIZ];
  int if_index;
  uint if_mtu;
  uchar if_lladdr[ETHER_ADDR_LEN];
  volatile int if_link_state;
  uint64_t if_baudrate;
  int (*if_transmit)(struct ifnet *ifp, const void *frame, uint len);
  void *if_softc;
  void *if_stack; // lwIP's netif for it
  uint64_t if_ipackets, if_ibytes, if_iqdrops;
  uint64_t if_opackets, if_obytes, if_oerrors;
};

struct fnu_ifinfo;
struct fnu_ifconfig;

void ifinit(void);
void if_attach(struct ifnet *ifp);
void if_input(struct ifnet *ifp, const void *frame, uint len);
int if_output(struct ifnet *ifp, const void *frame, uint len);
void if_link_state_change(struct ifnet *ifp, int state, uint64_t baudrate);
struct ifnet *ifunit(const char *name);
struct ifnet *if_first(void);
int if_info_all(struct fnu_ifinfo *buf, int capacity);

// the stack's side, in lwip.c
void net_init(void);
void netisr_start(void);
void net_stack_attach(struct ifnet *ifp);
void net_stack_input(struct ifnet *ifp, const void *frame, uint len);
void net_stack_link(void);
void net_stack_ifinfo(struct ifnet *ifp, struct fnu_ifinfo *out);
int net_stack_config(struct ifnet *ifp, const struct fnu_ifconfig *config);

#endif
