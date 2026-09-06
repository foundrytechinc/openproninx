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
typedef int (*proninx_net_receive_fn)(void *, const void *, uint);

// This is a PRONINX-native interface type used by all NIC drivers.
struct proninx_net_interface {
  char name[PRONINX_NET_NAME_MAX];
  uint mtu;
  uchar hardware_address[6];
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
int proninx_net_set_receive_handler(proninx_net_receive_fn, void *);
struct proninx_net_interface *proninx_net_interface(void);
void proninx_net_stats(struct proninx_net_stats *);

#endif
