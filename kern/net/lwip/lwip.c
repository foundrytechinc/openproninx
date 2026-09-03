// PRONINX netif binding for the unmodified lwIP vendor tree.
#include "adapter.h"
#include "defs.h"
#include "inc/abi.h"
#include "proc.h"
#include "spinlock.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#include "lwip/timeouts.h"
#include "lwip/prot/icmp.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "netif/ethernet.h"

static struct netif lwip_netif;
static struct proninx_net_interface *lwip_interface;
static int lwip_ready;
static char lwip_transmit_buffer[PRONINX_NET_FRAME_MAX];
static struct raw_pcb *ping_pcb;
static struct spinlock ping_lock;
static struct {
  ushort identifier;
  ushort sequence;
  uint started_at;
  uint round_trip_ms;
  int waiting;
  int received;
} ping_state;

#define UDP_SLOTS 8
#define UDP_QUEUE_DEPTH 4
#define UDP_DATAGRAM_MAX 1536

/*
 * UDP handles are deliberately not file descriptors yet: the VFS has no
 * socket file type.  They are nevertheless process-owned capabilities, so a
 * handle guessed by another process cannot be used to read or send traffic.
 * Keep a small, bounded packet queue per handle: network IRQ processing must
 * never allocate or block on user-space consumers.
 */
struct udp_datagram {
  char data[UDP_DATAGRAM_MAX];
  uint length;
  struct network_endpoint peer;
};
struct udp_slot {
  struct udp_pcb *pcb;
  pid_t owner;
  ushort bound_port;
  uint head, tail, count;
  uint dropped;
  struct udp_datagram queue[UDP_QUEUE_DEPTH];
  struct spinlock lock;
};
static struct udp_slot udp_slots[UDP_SLOTS];

#define TCP_SLOTS 8
#define TCP_QUEUE_DEPTH 4
#define TCP_SEGMENT_MAX 1536
enum tcp_slot_state { TCP_FREE, TCP_CONNECTING, TCP_CONNECTED, TCP_LISTENING, TCP_ERROR, TCP_CLOSED };
struct tcp_segment { char data[TCP_SEGMENT_MAX]; uint length, offset; };
struct tcp_slot {
  struct tcp_pcb *pcb;
  pid_t owner;
  enum tcp_slot_state state;
  ushort bound_port;
  uint head, tail, count;
  uint dropped;
  int accept_head, accept_tail, accept_count;
  int accepted[TCP_QUEUE_DEPTH];
  struct tcp_segment queue[TCP_QUEUE_DEPTH];
  struct spinlock lock;
};
static struct tcp_slot tcp_slots[TCP_SLOTS];
static struct {
  struct spinlock lock;
  int pending, complete, success;
  ip_addr_t address;
} dns_state;

int atoi(const char *text) {
  int value = 0;
  while (*text >= '0' && *text <= '9')
    value = value * 10 + (*text++ - '0');
  return value;
}

u32_t sys_now(void) {
  // PRONINX's APIC timer interrupt period is 10 ms.
  return ticks * 10;
}

static err_t lwip_linkoutput(struct netif *netif, struct pbuf *packet) {
  u16_t length;
  (void)netif;
  if (lwip_interface == 0 || packet->tot_len > sizeof(lwip_transmit_buffer))
    return ERR_BUF;
  length = pbuf_copy_partial(packet, lwip_transmit_buffer, packet->tot_len, 0);
  if (length != packet->tot_len)
    return ERR_BUF;
  return proninx_net_transmit(lwip_interface, lwip_transmit_buffer, length) < 0
             ? ERR_IF
             : ERR_OK;
}

static err_t lwip_netif_init(struct netif *netif) {
  netif->name[0] = 'e';
  netif->name[1] = 'n';
  netif->output = etharp_output;
  netif->linkoutput = lwip_linkoutput;
  netif->hwaddr_len = sizeof(lwip_interface->hardware_address);
  memmove(netif->hwaddr, lwip_interface->hardware_address, netif->hwaddr_len);
  netif->mtu = lwip_interface->mtu;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

static int lwip_input(void *context, const void *frame, uint length) {
  struct netif *netif = context;
  struct pbuf *packet;
  err_t result;
  if (!lwip_ready || netif != &lwip_netif)
    return -1;
  packet = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
  if (packet == 0)
    return -1;
  if (pbuf_take(packet, frame, length) != ERR_OK) {
    pbuf_free(packet);
    return -1;
  }
  result = netif->input(packet, netif);
  if (result != ERR_OK) {
    pbuf_free(packet);
    return -1;
  }
  return 0;
}

static u8_t lwip_ping_input(void *context, struct raw_pcb *pcb,
                            struct pbuf *packet, const ip_addr_t *address) {
  struct icmp_echo_hdr header;
  (void)context;
  (void)pcb;
  (void)address;
  if (pbuf_copy_partial(packet, &header, sizeof(header), 0) != sizeof(header) ||
      ICMPH_TYPE(&header) != ICMP_ER)
    return 0;
  acquire(&ping_lock);
  if (ping_state.waiting && header.id == lwip_htons(ping_state.identifier) &&
      header.seqno == lwip_htons(ping_state.sequence)) {
    ping_state.round_trip_ms = sys_now() - ping_state.started_at;
    ping_state.received = 1;
    wakeup(&ping_state);
    wakeup(&dns_state);
  }
  release(&ping_lock);
  return 0;
}

void proninx_lwip_init(void) {
  ip4_addr_t any;
  int i;
  if (lwip_ready)
    return;
  lwip_interface = proninx_net_interface();
  if (lwip_interface == 0) {
    cprintf("NET: lwIP waiting for a network interface\n");
    return;
  }
  IP4_ADDR(&any, 0, 0, 0, 0);
  lwip_init();
  for (i = 0; i < UDP_SLOTS; i++)
    initlock(&udp_slots[i].lock, "udp");
  for (i = 0; i < TCP_SLOTS; i++)
    initlock(&tcp_slots[i].lock, "tcp");
  initlock(&dns_state.lock, "dns");
  if (netif_add(&lwip_netif, &any, &any, &any, 0, lwip_netif_init,
                ethernet_input) == 0 ||
      proninx_net_set_receive_handler(lwip_input, &lwip_netif) < 0) {
    cprintf("NET: lwIP interface setup failed\n");
    return;
  }
  netif_set_default(&lwip_netif);
  netif_set_up(&lwip_netif);
  netif_set_link_up(&lwip_netif);
  lwip_ready = 1;
  if (dhcp_start(&lwip_netif) != ERR_OK) {
    lwip_ready = 0;
    cprintf("NET: DHCP start failed\n");
    return;
  }
  initlock(&ping_lock, "ping");
  ping_pcb = raw_new(IP_PROTO_ICMP);
  if (ping_pcb == 0) {
    lwip_ready = 0;
    cprintf("NET: ICMP ping setup failed\n");
    return;
  }
  raw_recv(ping_pcb, lwip_ping_input, 0);
  cprintf("NET: lwIP 2.2.1 started on %s; requesting DHCP lease\n",
          lwip_interface->name);
}

void proninx_lwip_timers(void) {
  int i;
  if (lwip_ready) {
    sys_check_timeouts();
    /* Timed waits have no callout facility in this small kernel.  Waking the
       bounded set of network waiters once per tick makes deadlines reliable. */
    wakeup(&ping_state);
    for (i = 0; i < UDP_SLOTS; i++)
      wakeup(&udp_slots[i]);
    for (i = 0; i < TCP_SLOTS; i++)
      wakeup(&tcp_slots[i]);
  }
}

static struct udp_slot *udp_slot_for(int handle) {
  if (handle < 1 || handle > UDP_SLOTS)
    return 0;
  return &udp_slots[handle - 1];
}

static int udp_owned(struct udp_slot *slot) {
  return slot && slot->pcb && slot->owner == myproc()->pid;
}

static void proninx_udp_input(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port) {
  struct udp_slot *slot = &udp_slots[(int)(uintptr_t)arg];
  struct udp_datagram *packet;
  (void)pcb;
  acquire(&slot->lock);
  if (slot->pcb && slot->count < UDP_QUEUE_DEPTH &&
      p->tot_len <= UDP_DATAGRAM_MAX) {
    packet = &slot->queue[slot->tail];
    pbuf_copy_partial(p, packet->data, p->tot_len, 0);
    packet->length = p->tot_len;
    packet->peer.address[0] = ip4_addr1(ip_2_ip4(addr));
    packet->peer.address[1] = ip4_addr2(ip_2_ip4(addr));
    packet->peer.address[2] = ip4_addr3(ip_2_ip4(addr));
    packet->peer.address[3] = ip4_addr4(ip_2_ip4(addr));
    packet->peer.port = port;
    slot->tail = (slot->tail + 1) % UDP_QUEUE_DEPTH;
    slot->count++;
    wakeup(slot);
  } else {
    slot->dropped++;
  }
  release(&slot->lock);
  pbuf_free(p);
}

int proninx_udp_open(void) {
  int i;
  if (!lwip_ready)
    return -1;
  for (i = 0; i < UDP_SLOTS; i++) {
    acquire(&udp_slots[i].lock);
    if (!udp_slots[i].pcb) {
      udp_slots[i].pcb = udp_new();
      if (udp_slots[i].pcb) {
        udp_slots[i].owner = myproc()->pid;
        udp_slots[i].head = udp_slots[i].tail = udp_slots[i].count = 0;
        udp_slots[i].dropped = 0;
        udp_slots[i].bound_port = 0;
        udp_recv(udp_slots[i].pcb, proninx_udp_input, (void *)(uintptr_t)i);
        release(&udp_slots[i].lock);
        return i + 1;
      }
    }
    release(&udp_slots[i].lock);
  }
  return -1;
}

int proninx_udp_bind(int handle, ushort port) {
  struct udp_slot *slot = udp_slot_for(handle);
  ip_addr_t any;
  int result;
  if (!slot || port == 0)
    return -1;
  acquire(&slot->lock);
  if (!udp_owned(slot) || slot->bound_port) {
    release(&slot->lock);
    return -1;
  }
  IP_ADDR4(&any, 0, 0, 0, 0);
  result = udp_bind(slot->pcb, &any, port) == ERR_OK ? 0 : -1;
  if (!result)
    slot->bound_port = port;
  release(&slot->lock);
  return result;
}

int proninx_udp_sendto(int handle, const void *buffer, uint length,
                       const struct network_endpoint *endpoint) {
  struct udp_slot *slot = udp_slot_for(handle);
  struct pbuf *packet;
  ip_addr_t address;
  err_t result;
  if (!slot || !buffer || !endpoint || endpoint->port == 0 ||
      length > UDP_DATAGRAM_MAX)
    return -1;
  acquire(&slot->lock);
  if (!udp_owned(slot)) { release(&slot->lock); return -1; }
  packet = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
  if (!packet || pbuf_take(packet, buffer, length) != ERR_OK) {
    if (packet) pbuf_free(packet);
    release(&slot->lock);
    return -1;
  }
  IP_ADDR4(&address, endpoint->address[0], endpoint->address[1],
           endpoint->address[2], endpoint->address[3]);
  result = udp_sendto(slot->pcb, packet, &address, endpoint->port);
  pbuf_free(packet);
  release(&slot->lock);
  return result == ERR_OK ? (int)length : -1;
}

int proninx_udp_recvfrom(int handle, void *buffer, uint length,
                         struct network_endpoint *endpoint, uint timeout_ms) {
  struct udp_slot *slot = udp_slot_for(handle);
  struct udp_datagram *packet;
  uint deadline;
  if (!slot || !buffer || !endpoint || length == 0)
    return -1;
  deadline = sys_now() + timeout_ms;
  acquire(&slot->lock);
  if (!udp_owned(slot)) { release(&slot->lock); return -1; }
  while (!slot->count) {
    if (timeout_ms == 0 || (int)(sys_now() - deadline) >= 0) {
      release(&slot->lock);
      return -1;
    }
    sleep(slot, &slot->lock);
    if (!udp_owned(slot)) { release(&slot->lock); return -1; }
  }
  packet = &slot->queue[slot->head];
  if (length > packet->length)
    length = packet->length;
  memmove(buffer, packet->data, length);
  *endpoint = packet->peer;
  slot->head = (slot->head + 1) % UDP_QUEUE_DEPTH;
  slot->count--;
  release(&slot->lock);
  return length;
}

int proninx_udp_close(int handle) {
  struct udp_slot *slot = udp_slot_for(handle);
  if (!slot)
    return -1;
  acquire(&slot->lock);
  if (!udp_owned(slot)) { release(&slot->lock); return -1; }
  udp_remove(slot->pcb);
  slot->pcb = 0;
  slot->owner = 0;
  slot->bound_port = 0;
  slot->head = slot->tail = slot->count = 0;
  wakeup(slot);
  release(&slot->lock);
  return 0;
}

void proninx_udp_process_exit(pid_t pid) {
  int i;
  for (i = 0; i < UDP_SLOTS; i++) {
    acquire(&udp_slots[i].lock);
    if (udp_slots[i].pcb && udp_slots[i].owner == pid) {
      udp_remove(udp_slots[i].pcb);
      udp_slots[i].pcb = 0;
      udp_slots[i].owner = 0;
      udp_slots[i].bound_port = 0;
      udp_slots[i].head = udp_slots[i].tail = udp_slots[i].count = 0;
      wakeup(&udp_slots[i]);
    }
    release(&udp_slots[i].lock);
  }
}

static struct tcp_slot *tcp_slot_for(int handle) {
  if (handle < 1 || handle > TCP_SLOTS) return 0;
  return &tcp_slots[handle - 1];
}

static int tcp_owned(struct tcp_slot *slot) {
  return slot && slot->pcb && slot->owner == myproc()->pid;
}

static void tcp_install_callbacks(struct tcp_slot *slot, int index);

static err_t proninx_tcp_received(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                                  err_t error) {
  struct tcp_slot *slot = &tcp_slots[(int)(uintptr_t)arg];
  struct tcp_segment *segment;
  (void)pcb;
  acquire(&slot->lock);
  if (!p) {
    slot->state = error == ERR_OK ? TCP_CLOSED : TCP_ERROR;
    wakeup(slot);
    release(&slot->lock);
    return ERR_OK;
  }
  tcp_recved(slot->pcb, p->tot_len);
  if (error == ERR_OK && slot->state == TCP_CONNECTED &&
      slot->count < TCP_QUEUE_DEPTH && p->tot_len <= TCP_SEGMENT_MAX) {
    segment = &slot->queue[slot->tail];
    pbuf_copy_partial(p, segment->data, p->tot_len, 0);
    segment->length = p->tot_len;
    segment->offset = 0;
    slot->tail = (slot->tail + 1) % TCP_QUEUE_DEPTH;
    slot->count++;
    wakeup(slot);
  } else {
    slot->dropped++;
  }
  release(&slot->lock);
  pbuf_free(p);
  return ERR_OK;
}

static void proninx_tcp_error(void *arg, err_t error) {
  struct tcp_slot *slot = &tcp_slots[(int)(uintptr_t)arg];
  (void)error;
  acquire(&slot->lock);
  slot->pcb = 0; /* lwIP already freed it before invoking err(). */
  slot->state = TCP_ERROR;
  wakeup(slot);
  release(&slot->lock);
}

static err_t proninx_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t error) {
  struct tcp_slot *slot = &tcp_slots[(int)(uintptr_t)arg];
  (void)pcb;
  acquire(&slot->lock);
  slot->state = error == ERR_OK ? TCP_CONNECTED : TCP_ERROR;
  wakeup(slot);
  release(&slot->lock);
  return ERR_OK;
}

static err_t proninx_tcp_accepted(void *arg, struct tcp_pcb *pcb, err_t error) {
  struct tcp_slot *listener = &tcp_slots[(int)(uintptr_t)arg];
  struct tcp_slot *child = 0;
  int i;
  if (error != ERR_OK) return error;
  for (i = 0; i < TCP_SLOTS; i++) {
    acquire(&tcp_slots[i].lock);
    if (!tcp_slots[i].pcb) { child = &tcp_slots[i]; break; }
    release(&tcp_slots[i].lock);
  }
  if (!child) return ERR_MEM;
  acquire(&listener->lock);
  if (listener->state != TCP_LISTENING || listener->accept_count == TCP_QUEUE_DEPTH) {
    release(&listener->lock); release(&child->lock); return ERR_MEM;
  }
  child->pcb = pcb;
  child->owner = listener->owner;
  child->state = TCP_CONNECTED;
  child->head = child->tail = child->count = 0;
  tcp_install_callbacks(child, i);
  listener->accepted[listener->accept_tail] = i + 1;
  listener->accept_tail = (listener->accept_tail + 1) % TCP_QUEUE_DEPTH;
  listener->accept_count++;
  wakeup(listener);
  release(&listener->lock);
  release(&child->lock);
  return ERR_OK;
}

static void tcp_install_callbacks(struct tcp_slot *slot, int index) {
  tcp_arg(slot->pcb, (void *)(uintptr_t)index);
  tcp_recv(slot->pcb, proninx_tcp_received);
  tcp_err(slot->pcb, proninx_tcp_error);
  tcp_sent(slot->pcb, 0);
}

int proninx_tcp_open(void) {
  int i;
  if (!lwip_ready) return -1;
  for (i = 0; i < TCP_SLOTS; i++) {
    acquire(&tcp_slots[i].lock);
    if (!tcp_slots[i].pcb && (tcp_slots[i].pcb = tcp_new())) {
      tcp_slots[i].owner = myproc()->pid;
      tcp_slots[i].state = TCP_CLOSED;
      tcp_slots[i].bound_port = 0;
      tcp_slots[i].head = tcp_slots[i].tail = tcp_slots[i].count = 0;
      tcp_slots[i].accept_head = tcp_slots[i].accept_tail = tcp_slots[i].accept_count = 0;
      tcp_slots[i].dropped = 0;
      tcp_install_callbacks(&tcp_slots[i], i);
      release(&tcp_slots[i].lock);
      return i + 1;
    }
    release(&tcp_slots[i].lock);
  }
  return -1;
}

int proninx_tcp_bind(int handle, ushort port) {
  struct tcp_slot *slot = tcp_slot_for(handle); ip_addr_t any; int result;
  if (!slot || !port) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot) || slot->state != TCP_CLOSED || slot->bound_port) { release(&slot->lock); return -1; }
  IP_ADDR4(&any, 0, 0, 0, 0);
  result = tcp_bind(slot->pcb, &any, port) == ERR_OK ? 0 : -1;
  if (!result) slot->bound_port = port;
  release(&slot->lock);
  return result;
}

int proninx_tcp_listen(int handle) {
  struct tcp_slot *slot = tcp_slot_for(handle); struct tcp_pcb *listener;
  if (!slot) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot) || slot->state != TCP_CLOSED || !slot->bound_port) { release(&slot->lock); return -1; }
  listener = tcp_listen(slot->pcb);
  if (!listener) { release(&slot->lock); return -1; }
  slot->pcb = listener; slot->state = TCP_LISTENING;
  tcp_arg(listener, (void *)(uintptr_t)(handle - 1));
  tcp_accept(listener, proninx_tcp_accepted);
  tcp_err(listener, proninx_tcp_error);
  release(&slot->lock);
  return 0;
}

int proninx_tcp_connect(int handle, const struct network_endpoint *endpoint, uint timeout_ms) {
  struct tcp_slot *slot = tcp_slot_for(handle); ip_addr_t address; uint deadline; err_t result;
  if (!slot || !endpoint || !endpoint->port || !timeout_ms) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot) || slot->state != TCP_CLOSED) { release(&slot->lock); return -1; }
  IP_ADDR4(&address, endpoint->address[0], endpoint->address[1], endpoint->address[2], endpoint->address[3]);
  slot->state = TCP_CONNECTING;
  result = tcp_connect(slot->pcb, &address, endpoint->port, proninx_tcp_connected);
  if (result != ERR_OK) { slot->state = TCP_ERROR; release(&slot->lock); return -1; }
  deadline = sys_now() + timeout_ms;
  while (slot->state == TCP_CONNECTING && (int)(sys_now() - deadline) < 0) sleep(slot, &slot->lock);
  result = slot->state == TCP_CONNECTED ? ERR_OK : ERR_TIMEOUT;
  release(&slot->lock);
  return result == ERR_OK ? 0 : -1;
}

int proninx_tcp_accept(int handle, uint timeout_ms) {
  struct tcp_slot *slot = tcp_slot_for(handle); uint deadline; int child;
  if (!slot) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot) || slot->state != TCP_LISTENING) { release(&slot->lock); return -1; }
  deadline = sys_now() + timeout_ms;
  while (!slot->accept_count) {
    if (!timeout_ms || (int)(sys_now() - deadline) >= 0) { release(&slot->lock); return -1; }
    sleep(slot, &slot->lock);
  }
  child = slot->accepted[slot->accept_head];
  slot->accept_head = (slot->accept_head + 1) % TCP_QUEUE_DEPTH; slot->accept_count--;
  release(&slot->lock);
  return child;
}

int proninx_tcp_send(int handle, const void *buffer, uint length) {
  struct tcp_slot *slot = tcp_slot_for(handle); err_t result;
  if (!slot || !buffer || !length || length > TCP_SEGMENT_MAX) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot) || slot->state != TCP_CONNECTED) { release(&slot->lock); return -1; }
  result = tcp_write(slot->pcb, buffer, length, TCP_WRITE_FLAG_COPY);
  if (result == ERR_OK) result = tcp_output(slot->pcb);
  release(&slot->lock);
  return result == ERR_OK ? (int)length : -1;
}

int proninx_tcp_recv(int handle, void *buffer, uint length, uint timeout_ms) {
  struct tcp_slot *slot = tcp_slot_for(handle); struct tcp_segment *segment; uint deadline, available;
  if (!slot || !buffer || !length) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot)) { release(&slot->lock); return -1; }
  deadline = sys_now() + timeout_ms;
  while (!slot->count) {
    if (slot->state != TCP_CONNECTED || !timeout_ms || (int)(sys_now() - deadline) >= 0) { release(&slot->lock); return -1; }
    sleep(slot, &slot->lock);
  }
  segment = &slot->queue[slot->head]; available = segment->length - segment->offset;
  if (length > available) length = available;
  memmove(buffer, segment->data + segment->offset, length); segment->offset += length;
  if (segment->offset == segment->length) { slot->head = (slot->head + 1) % TCP_QUEUE_DEPTH; slot->count--; }
  release(&slot->lock);
  return length;
}

int proninx_tcp_close(int handle) {
  struct tcp_slot *slot = tcp_slot_for(handle);
  if (!slot) return -1;
  acquire(&slot->lock);
  if (!tcp_owned(slot)) { release(&slot->lock); return -1; }
  tcp_arg(slot->pcb, 0); tcp_recv(slot->pcb, 0); tcp_err(slot->pcb, 0);
  tcp_abort(slot->pcb);
  slot->pcb = 0; slot->owner = 0; slot->state = TCP_FREE; slot->bound_port = 0; slot->count = slot->accept_count = 0;
  wakeup(slot); release(&slot->lock);
  return 0;
}

void proninx_tcp_process_exit(pid_t pid) {
  int i;
  for (i = 0; i < TCP_SLOTS; i++) {
    acquire(&tcp_slots[i].lock);
    if (tcp_slots[i].pcb && tcp_slots[i].owner == pid) {
      tcp_arg(tcp_slots[i].pcb, 0); tcp_recv(tcp_slots[i].pcb, 0); tcp_err(tcp_slots[i].pcb, 0);
      tcp_abort(tcp_slots[i].pcb); tcp_slots[i].pcb = 0; tcp_slots[i].owner = 0; tcp_slots[i].state = TCP_FREE; tcp_slots[i].bound_port = 0;
      tcp_slots[i].count = tcp_slots[i].accept_count = 0; wakeup(&tcp_slots[i]);
    }
    release(&tcp_slots[i].lock);
  }
}

static void proninx_dns_found(const char *name, const ip_addr_t *address, void *arg) {
  (void)name; (void)arg;
  acquire(&dns_state.lock);
  dns_state.success = address && IP_IS_V4(address);
  if (dns_state.success) ip_addr_copy(dns_state.address, *address);
  dns_state.complete = 1;
  dns_state.pending = 0;
  wakeup(&dns_state);
  release(&dns_state.lock);
}

int proninx_dns_resolve(struct network_dns_request *request) {
  ip_addr_t address;
  uint deadline;
  err_t result;
  int i;
  if (!lwip_ready || !request || !request->timeout_ms || request->timeout_ms > 30000)
    return -1;
  for (i = 0; i < NETWORK_DNS_NAME_MAX; i++) if (!request->name[i]) break;
  if (!i || i == NETWORK_DNS_NAME_MAX) return -1;
  acquire(&dns_state.lock);
  if (dns_state.pending) { release(&dns_state.lock); return -1; }
  dns_state.pending = 1; dns_state.complete = dns_state.success = 0;
  result = dns_gethostbyname(request->name, &address, proninx_dns_found, 0);
  if (result == ERR_OK) {
    dns_state.pending = 0;
    dns_state.success = IP_IS_V4(&address);
    if (dns_state.success) ip_addr_copy(dns_state.address, address);
    dns_state.complete = 1;
  } else if (result != ERR_INPROGRESS) {
    dns_state.pending = 0;
    release(&dns_state.lock);
    return -1;
  }
  deadline = sys_now() + request->timeout_ms;
  while (!dns_state.complete && (int)(sys_now() - deadline) < 0)
    sleep(&dns_state, &dns_state.lock);
  if (!dns_state.complete || !dns_state.success) { release(&dns_state.lock); return -1; }
  request->address[0] = ip4_addr1(ip_2_ip4(&dns_state.address));
  request->address[1] = ip4_addr2(ip_2_ip4(&dns_state.address));
  request->address[2] = ip4_addr3(ip_2_ip4(&dns_state.address));
  request->address[3] = ip4_addr4(ip_2_ip4(&dns_state.address));
  release(&dns_state.lock);
  return 0;
}

int proninx_lwip_status(struct network_status *status) {
  const ip4_addr_t *address;
  const ip4_addr_t *gateway;
  const ip_addr_t *dns;
  struct proninx_net_stats statistics;
  if (!lwip_ready || status == 0)
    return -1;
  memset(status, 0, sizeof(*status));
  safestrcpy(status->interface_name, lwip_interface->name,
             sizeof(status->interface_name));
  memmove(status->hardware_address, lwip_interface->hardware_address, 6);
  address = netif_ip4_addr(&lwip_netif);
  gateway = netif_ip4_gw(&lwip_netif);
  status->address[0] = ip4_addr1(address); status->address[1] = ip4_addr2(address);
  status->address[2] = ip4_addr3(address); status->address[3] = ip4_addr4(address);
  status->gateway[0] = ip4_addr1(gateway); status->gateway[1] = ip4_addr2(gateway);
  status->gateway[2] = ip4_addr3(gateway); status->gateway[3] = ip4_addr4(gateway);
  dns = dns_getserver(0);
  if (dns && IP_IS_V4(dns)) {
    status->dns_server[0] = ip4_addr1(ip_2_ip4(dns)); status->dns_server[1] = ip4_addr2(ip_2_ip4(dns));
    status->dns_server[2] = ip4_addr3(ip_2_ip4(dns)); status->dns_server[3] = ip4_addr4(ip_2_ip4(dns));
  }
  status->dhcp_bound = dhcp_supplied_address(&lwip_netif);
  proninx_net_stats(&statistics);
  status->received_frames = statistics.received_frames;
  status->dropped_frames = statistics.dropped_frames;
  status->transmitted_frames = statistics.transmitted_frames;
  return 0;
}

int proninx_lwip_configure(const struct network_ipv4_config *config) {
  ip4_addr_t address, netmask, gateway;
  ip_addr_t dns;
  uint mask, inverse_mask;
  if (!lwip_ready || !config)
    return -1;
  if (config->dhcp) {
    netif_set_addr(&lwip_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    return dhcp_start(&lwip_netif) == ERR_OK ? 0 : -1;
  }
  /* Reject a zero address and non-contiguous masks: these values otherwise
     produce routes that fail in surprising ways on server networks. */
  if (!config->address[0] && !config->address[1] && !config->address[2] && !config->address[3])
    return -1;
  mask = ((uint)config->netmask[0] << 24) |
         ((uint)config->netmask[1] << 16) |
         ((uint)config->netmask[2] << 8) | config->netmask[3];
  inverse_mask = ~mask;
  if (inverse_mask & (inverse_mask + 1))
    return -1;
  IP4_ADDR(&address, config->address[0], config->address[1], config->address[2], config->address[3]);
  IP4_ADDR(&netmask, config->netmask[0], config->netmask[1], config->netmask[2], config->netmask[3]);
  IP4_ADDR(&gateway, config->gateway[0], config->gateway[1], config->gateway[2], config->gateway[3]);
  IP_ADDR4(&dns, config->dns_server[0], config->dns_server[1], config->dns_server[2], config->dns_server[3]);
  dhcp_stop(&lwip_netif);
  netif_set_addr(&lwip_netif, &address, &netmask, &gateway);
  dns_setserver(0, &dns);
  return 0;
}

int proninx_lwip_ping(struct network_ping_request *request) {
  struct icmp_echo_hdr header;
  struct pbuf *packet;
  ip_addr_t destination;
  uint deadline;
  err_t result;
  if (!lwip_ready || ping_pcb == 0 || request->timeout_ms == 0 ||
      request->timeout_ms > 10000)
    return -1;
  IP_ADDR4(&destination, request->address[0], request->address[1],
           request->address[2], request->address[3]);
  packet = pbuf_alloc(PBUF_IP, sizeof(header), PBUF_RAM);
  if (packet == 0)
    return -1;
  acquire(&ping_lock);
  if (ping_state.waiting) {
    release(&ping_lock);
    pbuf_free(packet);
    return -1;
  }
  ping_state.identifier++;
  ping_state.sequence++;
  ping_state.started_at = sys_now();
  ping_state.waiting = 1;
  ping_state.received = 0;
  header.type = ICMP_ECHO;
  header.code = 0;
  header.chksum = 0;
  header.id = lwip_htons(ping_state.identifier);
  header.seqno = lwip_htons(ping_state.sequence);
  pbuf_take(packet, &header, sizeof(header));
  header.chksum = inet_chksum(packet, sizeof(header));
  pbuf_take(packet, &header, sizeof(header));
  result = raw_sendto(ping_pcb, packet, &destination);
  pbuf_free(packet);
  if (result != ERR_OK) {
    ping_state.waiting = 0;
    release(&ping_lock);
    return -1;
  }
  deadline = ping_state.started_at + request->timeout_ms;
  while (!ping_state.received && (int)(sys_now() - deadline) < 0)
    sleep(&ping_state, &ping_lock);
  if (!ping_state.received) {
    ping_state.waiting = 0;
    release(&ping_lock);
    return -1;
  }
  request->round_trip_ms = ping_state.round_trip_ms;
  ping_state.waiting = 0;
  release(&ping_lock);
  return 0;
}
