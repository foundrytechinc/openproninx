// PRONINX-owned packet I/O boundary for the vendored networking stack.
// No FreeBSD source is included here: the future compatibility layer maps its
// mbufs and ifnet callbacks onto these deliberately small interfaces.
#include "adapter.h"
#include "defs.h"
#include "spinlock.h"

static struct spinlock net_lock;
static struct proninx_net_interface *registered_interface;
static proninx_net_receive_fn receive_handler;
static void *receive_context;
static struct proninx_net_stats statistics;

void proninx_net_init(void) {
  initlock(&net_lock, "net");
  cprintf("NET: PRONINX adapter ready; no network interface attached\n");
}

int proninx_net_register(struct proninx_net_interface *interface) {
  if (interface == 0 || interface->transmit == 0 || interface->mtu == 0 ||
      interface->mtu > PRONINX_NET_MTU)
    return -1;

  acquire(&net_lock);
  if (registered_interface != 0) {
    release(&net_lock);
    return -1;
  }
  registered_interface = interface;
  release(&net_lock);
  cprintf("NET: interface %s registered (MTU %d)\n", interface->name,
          interface->mtu);
  return 0;
}

int proninx_net_receive(struct proninx_net_interface *interface,
                        const void *frame, uint length) {
  proninx_net_receive_fn handler;
  void *context;
  int result = -1;
  if (interface == 0 || frame == 0 || length == 0 ||
      length > PRONINX_NET_FRAME_MAX)
    return -1;

  acquire(&net_lock);
  if (interface != registered_interface) {
    release(&net_lock);
    return -1;
  }
  statistics.received_frames++;
  statistics.received_bytes += length;
  handler = receive_handler;
  context = receive_context;
  release(&net_lock);
  if (handler != 0)
    result = handler(context, frame, length);
  if (result < 0) {
    acquire(&net_lock);
    statistics.dropped_frames++;
    release(&net_lock);
  }
  return result;
}

int proninx_net_transmit(struct proninx_net_interface *interface,
                         const void *frame, uint length) {
  int result;
  if (interface == 0 || frame == 0 || length == 0 ||
      length > PRONINX_NET_FRAME_MAX)
    return -1;

  acquire(&net_lock);
  if (interface != registered_interface) {
    release(&net_lock);
    return -1;
  }
  release(&net_lock);
  result = interface->transmit(interface, frame, length);
  if (result < 0)
    return result;
  acquire(&net_lock);
  statistics.transmitted_frames++;
  statistics.transmitted_bytes += length;
  release(&net_lock);
  return 0;
}

void proninx_net_stats(struct proninx_net_stats *out) {
  if (out == 0)
    return;
  acquire(&net_lock);
  *out = statistics;
  release(&net_lock);
}

int proninx_net_set_receive_handler(proninx_net_receive_fn handler,
                                    void *context) {
  if (handler == 0)
    return -1;
  acquire(&net_lock);
  if (registered_interface == 0 || receive_handler != 0) {
    release(&net_lock);
    return -1;
  }
  receive_handler = handler;
  receive_context = context;
  release(&net_lock);
  return 0;
}

struct proninx_net_interface *proninx_net_interface(void) {
  struct proninx_net_interface *interface;
  acquire(&net_lock);
  interface = registered_interface;
  release(&net_lock);
  return interface;
}
