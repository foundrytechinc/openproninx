#ifndef PRONINX_NET_ADAPTER_H
#define PRONINX_NET_ADAPTER_H

#include "inc/types.h"

#define PRONINX_NET_NAME_MAX 16
#define PRONINX_NET_MTU 1500
#define PRONINX_NET_FRAME_MAX 1536

struct proninx_net_interface;

// A driver owns transmission. The adapter retains no packet after this call.
typedef int (*proninx_net_transmit_fn)(struct proninx_net_interface *,
                                       const void *, uint);

// This is deliberately a PRONINX type, rather than a FreeBSD ifnet. The
// compatibility layer will translate only at the networking boundary.
struct proninx_net_interface {
  char name[PRONINX_NET_NAME_MAX];
  uint mtu;
  proninx_net_transmit_fn transmit;
  void *driver_context;
};

struct proninx_net_stats {
  uint64_t received_frames;
  uint64_t received_bytes;
  uint64_t dropped_frames;
  uint64_t transmitted_frames;
  uint64_t transmitted_bytes;
};

void proninx_net_init(void);
int proninx_net_register(struct proninx_net_interface *);
int proninx_net_receive(struct proninx_net_interface *, const void *, uint);
int proninx_net_transmit(struct proninx_net_interface *, const void *, uint);
void proninx_net_stats(struct proninx_net_stats *);

#endif
