#include "if.h"
#include "inet.h"
#include "callout.h"
#include "defs.h"
#include "inc/abi.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"

#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
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
#include "lwip/priv/tcp_priv.h"
#include "netif/ethernet.h"

// NETLOCK

static struct mutex netlock;

struct netisr_frame {
  struct pbuf_custom pc;
  struct netisr_frame *next;
  struct ifnet *ifp;
  uint len;
  uchar data[];
};

static struct {
  struct mutex lock;
  struct netisr_frame *head, **tail;
  int len;
  int kick;      // wanted before its time
  int links;
  u32_t wake_at;
} niq;

static int netisr_maxqlen = 256;
static uint64_t netisr_drops;

static int udp_recvspace = 41600;

// buffer sizes; ceilings are lwIP's window
static int tcp_sendspace = 32 * 1024, tcp_recvspace = 64 * 1024;
static int tcp_sendbuf_auto = 1, tcp_recvbuf_auto = 1;
static int tcp_sendbuf_max = TCP_SND_BUF, tcp_recvbuf_max = TCP_WND;
static u8_t tcp_synack_id;
extern int somaxconn;

int atoi(const char *text) {
  int value = 0;
  while (*text >= '0' && *text <= '9')
    value = value * 10 + (*text++ - '0');
  return value;
}

u32_t sys_now(void) { return (u32_t)(ticks * (1000 / HZ)); }

static void netisr_kick(void) {
  mtx_enter(&niq.lock);
  niq.kick = 1;
  wakeup(&niq);
  mtx_leave(&niq.lock);
}

// under netlock
static void net_timer_check(void) {
  u32_t t = sys_timeouts_sleeptime();

  if (t != SYS_TIMEOUTS_SLEEPTIME_INFINITE &&
      (s32_t)(sys_now() + t - niq.wake_at) < 0) {
    niq.wake_at = sys_now() + t;
    netisr_kick();
  }
}

// a looped frame goes to netisr at once. ???
static void net_loop_check(void) {
  struct netif *n;

  NETIF_FOREACH(n) {
    if (n->loop_first != 0) {
      netisr_kick();
      return;
    }
  }
}

static void net_enter(void) { mtx_enter(&netlock); }

static void net_leave(void) {
  net_timer_check();
  net_loop_check();
  mtx_leave(&netlock);
}

static int netsleep(void *chan, u32_t deadline) {
  s32_t left = (s32_t)(deadline - sys_now());

  if (left <= 0)
    return -1;
  net_timer_check();
  net_loop_check();
  return msleep(chan, &netlock, PSOCK | PCATCH, "netio", mstohz((u32_t)left)) ==
                 FNU_ERR_INTERRUPTED
             ? -1
             : 0;
}

static u32_t deadline_of(uint timeout_ms) { return sys_now() + timeout_ms; }

static void netwake(void *chan, struct selinfo *rsel, struct selinfo *wsel) {
  wakeup(chan);
  if (rsel != 0)
    selwakeup(rsel);
  if (wsel != 0)
    selwakeup(wsel);
}

static int waiterr(void) { return proc_interrupted() ? -FNU_ERR_INTERRUPTED : -FNU_ERR_WOULDBLOCK; }

static int errno_of(err_t err) {
  switch (err) {
  case ERR_OK: return 0;
  case ERR_MEM: return FNU_ERR_NOMEMORY;
  case ERR_BUF: return FNU_ERR_NOBUFFERS;
  case ERR_TIMEOUT: return FNU_ERR_WOULDBLOCK;
  case ERR_RTE: return FNU_ERR_HOSTUNREACH;
  case ERR_INPROGRESS: return FNU_ERR_INPROGRESS;
  case ERR_VAL: return FNU_ERR_INVALID;
  case ERR_WOULDBLOCK: return FNU_ERR_WOULDBLOCK;
  case ERR_USE: return FNU_ERR_ADDRINUSE;
  case ERR_ALREADY: return FNU_ERR_ALREADY;
  case ERR_ISCONN: return FNU_ERR_CONNECTED;
  case ERR_CONN: return FNU_ERR_NOTCONNECTED;
  case ERR_IF: return FNU_ERR_NETDOWN;
  case ERR_ABRT: return FNU_ERR_ABORTED;
  case ERR_RST: return FNU_ERR_RESET;
  case ERR_CLSD: return FNU_ERR_NOTCONNECTED;
  default: return FNU_ERR_IO;
  }
}

void *proninx_lwip_malloc(size_t size) { return malloc(size, M_DEVBUF, M_NOWAIT); }
void *proninx_lwip_calloc(size_t n, size_t size) {
  return n != 0 && size > (size_t)-1 / n ? 0 : malloc(n * size, M_DEVBUF, M_NOWAIT | M_ZERO);
}
void proninx_lwip_free(void *p) { free(p, M_DEVBUF); }

// INTERFACES

struct stack_if {
  struct netif netif;
  struct stack_if *next;
  struct ifnet *ifp;
  char *txbuf;
};

static struct stack_if *stack_ifs, **stack_ifs_tail = &stack_ifs;

static err_t stack_linkoutput(struct netif *netif, struct pbuf *p) {
  struct stack_if *s = (struct stack_if *)netif;

  if (p->tot_len > ETHER_MAX_LEN)
    return ERR_BUF;
  if (p->next == 0)
    return if_output(s->ifp, p->payload, p->len) < 0 ? ERR_IF : ERR_OK;
  if (pbuf_copy_partial(p, s->txbuf, p->tot_len, 0) != p->tot_len)
    return ERR_BUF;
  return if_output(s->ifp, s->txbuf, p->tot_len) < 0 ? ERR_IF : ERR_OK;
}

static err_t stack_netif_init(struct netif *netif) {
  struct ifnet *ifp = netif->state;

  netif->name[0] = 'e';
  netif->name[1] = 'n';
  netif->output = etharp_output;
  netif->linkoutput = stack_linkoutput;
  netif->hwaddr_len = ETHER_ADDR_LEN;
  memmove(netif->hwaddr, ifp->if_lladdr, ETHER_ADDR_LEN);
  netif->mtu = (u16_t)ifp->if_mtu;
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
  return ERR_OK;
}

static int routable(struct netif *n) {
  return netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany(netif_ip4_gw(n));
}

static void route_default(void) {
  struct stack_if *s;

  if (netif_default != 0 && routable(netif_default))
    return;
  for (s = stack_ifs; s != 0; s = s->next)
    if (routable(&s->netif)) {
      netif_set_default(&s->netif);
      return;
    }
  if (netif_default == 0 && stack_ifs != 0)
    netif_set_default(&stack_ifs->netif);
}

static void stack_status(struct netif *netif) {
  struct ifnet *ifp = netif->state;
  const ip4_addr_t *a = netif_ip4_addr(netif), *g = netif_ip4_gw(netif);
  uint32_t mask = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(netif)));
  int bits = 0;

  route_default();
  if (ip4_addr_isany(a))
    return;
  while (mask & 0x80000000u) {
    bits++;
    mask <<= 1;
  }
  printf("%s: inet %d.%d.%d.%d/%d via %d.%d.%d.%d%s\n", ifp->if_xname,
          ip4_addr1(a), ip4_addr2(a), ip4_addr3(a), ip4_addr4(a), bits,
          ip4_addr1(g), ip4_addr2(g), ip4_addr3(g), ip4_addr4(g),
          dhcp_supplied_address(netif) ? ", leased" : "");
}

void net_stack_attach(struct ifnet *ifp) {
  struct stack_if *s = malloc(sizeof(*s), M_DEVBUF, M_WAITOK | M_ZERO);

  s->ifp = ifp;
  s->txbuf = malloc(ETHER_MAX_LEN, M_DEVBUF, M_WAITOK);
  net_enter();
  if (netif_add(&s->netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, ifp,
                stack_netif_init, ethernet_input) == 0) {
    net_leave();
    printf("%s: lwIP would not take it\n", ifp->if_xname);
    free(s->txbuf, M_DEVBUF);
    free(s, M_DEVBUF);
    return;
  }
  *stack_ifs_tail = s;
  stack_ifs_tail = &s->next;
  netif_set_status_callback(&s->netif, stack_status);
  netif_set_up(&s->netif);
  if (LINK_STATE_IS_UP(ifp->if_link_state))
    netif_set_link_up(&s->netif);
  if (dhcp_start(&s->netif) != ERR_OK)
    printf("%s: DHCP did not start\n", ifp->if_xname);
  route_default();
  __atomic_store_n(&ifp->if_stack, &s->netif, __ATOMIC_RELEASE);
  net_leave();
}

// link changes to lwIP
static void stack_links(void) {
  struct stack_if *s;
  int up;

  for (s = stack_ifs; s != 0; s = s->next) {
    up = LINK_STATE_IS_UP(s->ifp->if_link_state);
    if (up && !netif_is_link_up(&s->netif))
      netif_set_link_up(&s->netif);
    else if (!up && netif_is_link_up(&s->netif))
      netif_set_link_down(&s->netif);
  }
  route_default();
}

void net_stack_link(void) {
  mtx_enter(&niq.lock);
  niq.links = 1;
  wakeup(&niq);
  mtx_leave(&niq.lock);
}

static void put4(uint8 *out, const ip4_addr_t *a) {
  out[0] = ip4_addr1(a);
  out[1] = ip4_addr2(a);
  out[2] = ip4_addr3(a);
  out[3] = ip4_addr4(a);
}

void net_stack_ifinfo(struct ifnet *ifp, struct fnu_ifinfo *e) {
  struct netif *n = __atomic_load_n(&ifp->if_stack, __ATOMIC_ACQUIRE);

  if (n == 0)
    return;
  net_enter();
  put4(e->address, netif_ip4_addr(n));
  put4(e->netmask, netif_ip4_netmask(n));
  put4(e->gateway, netif_ip4_gw(n));
  if (dhcp_supplied_address(n))
    e->flags |= FNU_IFF_DHCP;
  if (netif_default == n)
    e->flags |= FNU_IFF_DEFAULT;
  net_leave();
}

#define DNS_SERVERS 4

static uint8_t dns_server[DNS_SERVERS][4];
static int ndns;

int net_stack_config(struct ifnet *ifp, const struct fnu_ifconfig *c) {
  struct netif *n = __atomic_load_n(&ifp->if_stack, __ATOMIC_ACQUIRE);
  ip4_addr_t address, netmask, gateway;
  uint mask, inverse;
  int r;

  if (n == 0)
    return FNU_ERR_NETDOWN;
  if (c->dhcp) {
    net_enter();
    netif_set_addr(n, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    r = errno_of(dhcp_start(n));
    route_default();
    net_leave();
    return r;
  }
  if (!c->address[0] && !c->address[1] && !c->address[2] && !c->address[3])
    return FNU_ERR_INVALID;
  mask = ((uint)c->netmask[0] << 24) | ((uint)c->netmask[1] << 16) |
         ((uint)c->netmask[2] << 8) | c->netmask[3];
  inverse = ~mask;
  if (inverse & (inverse + 1))
    return FNU_ERR_INVALID;
  IP4_ADDR(&address, c->address[0], c->address[1], c->address[2], c->address[3]);
  IP4_ADDR(&netmask, c->netmask[0], c->netmask[1], c->netmask[2], c->netmask[3]);
  IP4_ADDR(&gateway, c->gateway[0], c->gateway[1], c->gateway[2], c->gateway[3]);
  net_enter();
  dhcp_release_and_stop(n);
  netif_set_addr(n, &address, &netmask, &gateway);
  if (c->dns_server[0] | c->dns_server[1] | c->dns_server[2] | c->dns_server[3]) {
    memmove(dns_server[0], c->dns_server, 4);
    ndns = 1;
  }
  route_default();
  net_leave();
  return 0;
}

static int dns_servers(void *arg, char *out, size_t max) {
  size_t n = 0;
  int i;

  (void)arg;
  out[0] = 0;
  net_enter();
  for (i = 0; i < ndns && n < max; i++)
    n += (size_t)snprintf(out + n, max - n, "%s%d.%d.%d.%d", n ? " " : "",
                          dns_server[i][0], dns_server[i][1], dns_server[i][2],
                          dns_server[i][3]);
  net_leave();
  if (n >= max)
    n = max - 1;
  return (int)n;
}

// netlock held
void proninx_dhcp_option(u8_t msg_type, u8_t option, u8_t len, struct pbuf *p,
                         u16_t offset) {
  uint8_t got[DNS_SERVERS * 4];
  int n;

  if (option != DHCP_OPTION_DNS_SERVER || msg_type != DHCP_ACK || len % 4 != 0)
    return;
  n = len / 4 < DNS_SERVERS ? len / 4 : DNS_SERVERS;
  if (pbuf_copy_partial(p, got, (u16_t)(n * 4), offset) != n * 4)
    return;
  memmove(dns_server, got, (size_t)n * 4);
  ndns = n;
}

static int64_t netisr_drops_get(void *arg) {
  (void)arg;
  return (int64_t)__atomic_load_n(&netisr_drops, __ATOMIC_RELAXED);
}

// NETISR

static void netisr_free(struct pbuf *p) { free(p, M_DEVBUF); }

void net_stack_input(struct ifnet *ifp, const void *frame, uint len) {
  struct netisr_frame *f;

  if (__atomic_load_n(&niq.len, __ATOMIC_RELAXED) >= netisr_maxqlen ||
      (f = malloc(sizeof(*f) + len, M_DEVBUF, M_NOWAIT)) == 0)
    goto drop;
  f->next = 0;
  f->ifp = ifp;
  f->len = len;
  memmove(f->data, frame, len);
  mtx_enter(&niq.lock);
  if (niq.len >= netisr_maxqlen) {
    mtx_leave(&niq.lock);
    free(f, M_DEVBUF);
    goto drop;
  }
  *niq.tail = f;
  niq.tail = &f->next;
  niq.len++;
  wakeup(&niq);
  mtx_leave(&niq.lock);
  return;
drop:
  __atomic_fetch_add(&ifp->if_iqdrops, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&netisr_drops, 1, __ATOMIC_RELAXED);
}

static void netisr_deliver(struct netisr_frame *f) {
  struct netif *n = __atomic_load_n(&f->ifp->if_stack, __ATOMIC_ACQUIRE);
  struct pbuf *p;

  f->pc.custom_free_function = netisr_free;
  p = pbuf_alloced_custom(PBUF_RAW, (u16_t)f->len, PBUF_REF, &f->pc, f->data,
                          (u16_t)f->len);
  if (n == 0 || p == 0) {
    free(f, M_DEVBUF);
    return;
  }
  if (n->input(p, n) != ERR_OK)
    pbuf_free(p);
}

static void netisr(void *arg) {
  struct netisr_frame *f, *next;
  int links;
  u32_t t;

  (void)arg;
  for (;;) {
    net_enter();
    niq.wake_at = sys_now();
    mtx_enter(&niq.lock);
    f = niq.head;
    niq.head = 0;
    niq.tail = &niq.head;
    niq.len = 0;
    links = niq.links;
    niq.links = 0;
    mtx_leave(&niq.lock);
    for (; f != 0; f = next) {
      next = f->next;
      netisr_deliver(f);
    }
    if (links)
      stack_links();
    sys_check_timeouts();
    netif_poll_all();
    net_loop_check();
    t = sys_timeouts_sleeptime();
    niq.wake_at = sys_now() + (t == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? 0x7fffffffu : t);
    mtx_leave(&netlock);

    mtx_enter(&niq.lock);
    if (niq.head == 0 && !niq.kick && !niq.links)
      msleep(&niq, &niq.lock, PSOCK, "netisr",
             t == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? 0 : mstohz(t));
    niq.kick = 0;
    mtx_leave(&niq.lock);
  }
}

void netisr_start(void) {
  if (kthread_create(netisr, 0, 0, "netisr") != 0)
    panic("no netisr thread");
}

void net_init(void) {
  mtx_init(&netlock, "net");
  mtx_init(&niq.lock, "netisr");
  niq.tail = &niq.head;
  ifinit();
  net_enter();
  lwip_init();
  tcp_synack_id = tcp_ext_arg_alloc_id();
  net_leave();
  sysctl_add_int("net.isr.maxqlen", &netisr_maxqlen, FNU_CTLF_STDIO | FNU_CTLF_WRITE, 1,
                 1 << 20);
  sysctl_add_quad_fn("net.isr.drops", netisr_drops_get, 0);
  sysctl_add_int("net.inet.udp.recvspace", &udp_recvspace,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 1, 1 << 24);
  sysctl_add_int("net.inet.tcp.sendspace", &tcp_sendspace, FNU_CTLF_STDIO | FNU_CTLF_WRITE,
                 TCP_MSS, TCP_SND_BUF);
  sysctl_add_int("net.inet.tcp.recvspace", &tcp_recvspace, FNU_CTLF_STDIO | FNU_CTLF_WRITE,
                 0xffff, TCP_WND);
  sysctl_add_int("net.inet.tcp.sendbuf_auto", &tcp_sendbuf_auto,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("net.inet.tcp.recvbuf_auto", &tcp_recvbuf_auto,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("net.inet.tcp.sendbuf_max", &tcp_sendbuf_max, FNU_CTLF_STDIO, 0, 0);
  sysctl_add_int("net.inet.tcp.recvbuf_max", &tcp_recvbuf_max, FNU_CTLF_STDIO, 0, 0);
  sysctl_add_string_fn("net.inet.dns.servers", dns_servers, 0, FNU_PLEDGE_DNS);
  printf("NET: lwIP %s\n", LWIP_VERSION_STRING);
}

static void endpoint_from(struct fnu_network_endpoint *ep, const ip_addr_t *addr,
                          u16_t port) {
  put4(ep->address, ip_2_ip4(addr));
  ep->port = port;
}

static void endpoint_to(const struct fnu_network_endpoint *ep, ip_addr_t *addr) {
  IP_ADDR4(addr, ep->address[0], ep->address[1], ep->address[2],
           ep->address[3]);
}

// DATAGRAMS

struct dgram_sock {
  struct udp_pcb *pcb;
  struct raw_pcb *raw;
  struct dgram *head, **tail;
  uint qbytes;
  struct selinfo rsel;
};

static void dgram_queue(struct dgram_sock *u, struct pbuf *p, const ip_addr_t *addr,
                        u16_t port) {
  struct dgram *d;

  if (u->qbytes + p->tot_len <= (uint)udp_recvspace &&
      (d = malloc(sizeof(*d) + p->tot_len, M_DEVBUF, M_NOWAIT)) != 0) {
    pbuf_copy_partial(p, d->data, p->tot_len, 0);
    d->len = p->tot_len;
    d->next = 0;
    endpoint_from(&d->peer, addr, port);
    *u->tail = d;
    u->tail = &d->next;
    u->qbytes += d->len;
    netwake(u, &u->rsel, 0);
  }
}

static void so_udp_input(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr, u16_t port) {
  (void)pcb;
  dgram_queue(arg, p, addr, port);
  pbuf_free(p);
}

static u8_t so_raw_input(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                         const ip_addr_t *addr) {
  (void)pcb;
  dgram_queue(arg, p, addr, 0);
  return 0;
}

int proninx_dgram_open(struct dgram_sock **out) {
  struct dgram_sock *u = malloc(sizeof(*u), M_DEVBUF, M_WAITOK | M_ZERO);

  u->tail = &u->head;
  net_enter();
  if ((u->pcb = udp_new()) == 0) {
    net_leave();
    free(u, M_DEVBUF);
    return -FNU_ERR_NOBUFFERS;
  }
  udp_recv(u->pcb, so_udp_input, u);
  net_leave();
  *out = u;
  return 0;
}

int proninx_dgram_raw_open(struct dgram_sock **out, int protocol) {
  struct dgram_sock *u = malloc(sizeof(*u), M_DEVBUF, M_WAITOK | M_ZERO);

  u->tail = &u->head;
  net_enter();
  if ((u->raw = raw_new((u8_t)protocol)) == 0) {
    net_leave();
    free(u, M_DEVBUF);
    return -FNU_ERR_NOBUFFERS;
  }
  raw_recv(u->raw, so_raw_input, u);
  net_leave();
  *out = u;
  return 0;
}

int proninx_dgram_reuse(struct dgram_sock *u, int on) {
  net_enter();
  if (u->raw != 0)
    ;
  else if (on)
    ip_set_option(u->pcb, SOF_REUSEADDR);
  else
    ip_reset_option(u->pcb, SOF_REUSEADDR);
  net_leave();
  return 0;
}

int proninx_dgram_bind(struct dgram_sock *u, const struct fnu_network_endpoint *local) {
  ip_addr_t address;
  int result;

  net_enter();
  endpoint_to(local, &address);
  if (u->raw != 0)
    result = -errno_of(raw_bind(u->raw, &address));
  else if (u->pcb->local_port != 0)
    result = -FNU_ERR_INVALID;
  else
    result = -errno_of(udp_bind(u->pcb, &address, local->port));
  net_leave();
  return result;
}

int proninx_dgram_connect(struct dgram_sock *u, const struct fnu_network_endpoint *peer) {
  ip_addr_t address;
  int result = 0;

  net_enter();
  if (peer != 0)
    endpoint_to(peer, &address);
  if (u->raw != 0) {
    if (peer == 0)
      raw_disconnect(u->raw);
    else
      result = -errno_of(raw_connect(u->raw, &address));
  } else if (peer == 0)
    udp_disconnect(u->pcb);
  else
    result = -errno_of(udp_connect(u->pcb, &address, peer->port));
  net_leave();
  return result;
}

static struct netif *onlink(const ip_addr_t *dst) {
  struct netif *n;

  NETIF_FOREACH(n)
  if (netif_is_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n)) &&
      ip4_addr_net_eq(ip_2_ip4(dst), netif_ip4_addr(n), netif_ip4_netmask(n)))
    return n;
  return 0;
}

int proninx_dgram_sendto(struct dgram_sock *u, const void *buffer, uint length,
                       const struct fnu_network_endpoint *endpoint,
                       int dontroute) {
  struct pbuf *packet;
  struct netif *n = 0;
  ip_addr_t address;
  err_t result;

  if (length > 0xffff - 8 - 20)
    return -FNU_ERR_MSGTOOBIG;
  net_enter();
  packet = pbuf_alloc(u->raw != 0 ? PBUF_IP : PBUF_TRANSPORT, (u16_t)length, PBUF_RAM);
  if (!packet || pbuf_take(packet, buffer, (u16_t)length) != ERR_OK) {
    if (packet)
      pbuf_free(packet);
    net_leave();
    return -FNU_ERR_NOBUFFERS;
  }
  if (endpoint != 0)
    endpoint_to(endpoint, &address);
  else
    ip_addr_copy(address, u->raw != 0 ? u->raw->remote_ip : u->pcb->remote_ip);
  if (dontroute && (n = onlink(&address)) == 0)
    result = ERR_RTE;
  else if (u->raw != 0 && n != 0)
    result = raw_sendto_if_src(u->raw, packet, &address, n, netif_ip_addr4(n));
  else if (u->raw != 0)
    result = raw_sendto(u->raw, packet, &address);
  else if (n != 0)
    result = udp_sendto_if(u->pcb, packet, &address,
                           endpoint != 0 ? endpoint->port : u->pcb->remote_port, n);
  else if (endpoint != 0)
    result = udp_sendto(u->pcb, packet, &address, endpoint->port);
  else
    result = udp_send(u->pcb, packet);
  pbuf_free(packet);
  netif_poll_all();
  net_leave();
  return result == ERR_OK ? (int)length : -errno_of(result);
}

int proninx_dgram_recv(struct dgram_sock *u, struct dgram **out, uint timeout_ms,
                     int peek) {
  struct dgram *d, *c;
  u32_t deadline;
  uint len;

  deadline = deadline_of(timeout_ms);
  for (;;) {
    net_enter();
    while (u->head == 0)
      if (timeout_ms == 0 || netsleep(u, deadline) < 0) {
        net_leave();
        return waiterr();
      }
    d = u->head;
    if (!peek) {
      if ((u->head = d->next) == 0)
        u->tail = &u->head;
      u->qbytes -= d->len;
      net_leave();
      *out = d;
      return 0;
    }
    len = d->len;
    net_leave();
    c = malloc(sizeof(*c) + len, M_DEVBUF, M_WAITOK);
    net_enter();
    if ((d = u->head) != 0 && d->len <= len) {
      memmove(c, d, sizeof(*d) + d->len);
      c->next = 0;
      net_leave();
      *out = c;
      return 0;
    }
    net_leave();
    free(c, M_DEVBUF);
  }
}

void proninx_dgram_free(struct dgram *d) { free(d, M_DEVBUF); }

void proninx_dgram_close(struct dgram_sock *u) {
  struct dgram *d;

  net_enter();
  if (u->raw != 0)
    raw_remove(u->raw);
  else
    udp_remove(u->pcb);
  net_leave();
  while ((d = u->head) != 0) {
    u->head = d->next;
    free(d, M_DEVBUF);
  }
  free(u, M_DEVBUF);
}

int proninx_dgram_readable(struct dgram_sock *u) {
  int ready;

  net_enter();
  if (!(ready = u->head != 0))
    selrecord(&u->rsel);
  net_leave();
  return ready;
}

// the next datagram's length
int64_t proninx_dgram_avail(struct dgram_sock *u) {
  int64_t n;

  net_enter();
  n = u->head != 0 ? u->head->len : 0;
  net_leave();
  return n;
}

int proninx_dgram_local(struct dgram_sock *u, struct fnu_network_endpoint *out) {
  net_enter();
  if (u->raw != 0)
    endpoint_from(out, &u->raw->local_ip, 0);
  else
    endpoint_from(out, &u->pcb->local_ip, u->pcb->local_port);
  net_leave();
  return 0;
}

// TCP


enum tcp_sock_state {
  TCP_NEW,
  TCP_CONNECTING,
  TCP_CONNECTED,
  TCP_PEER_CLOSED,
  TCP_LISTENING,
  TCP_ERROR
};

struct tcp_sock {
  struct tcp_pcb *pcb;
  enum tcp_sock_state state;
  int error; // SO_ERROR value
  int rxshut, txshut;
  uint head, tail, count, rsize; // the ring is the receive buffer
  uchar *ring;
  uint sndwat;
  int64_t rtttime, srtt; // ns, srtt scaled by 8
  u32_t rtseq, snd_max;
  int snd_max_ok;
  int64_t rfbuf_ts;
  uint rfbuf_cnt;
  struct selinfo rsel, wsel;
  struct tcp_sock *acc_head, **acc_tail, *acc_next;
  int acc_count, backlog;
};

static void tcp_detach(struct tcp_sock *t) {
  struct tcp_pcb *pcb = t->pcb;

  tcp_arg(pcb, 0);
  if (t->state == TCP_LISTENING) {
    tcp_accept(pcb, 0);
    return;
  }
  tcp_err(pcb, 0);
  tcp_recv(pcb, 0);
  tcp_sent(pcb, 0);
  tcp_poll(pcb, 0, 0);
}

static void tcp_ring_put(struct tcp_sock *t, struct pbuf *p) {
  uint first = t->rsize - t->tail;

  if (first > p->tot_len)
    first = p->tot_len;
  pbuf_copy_partial(p, t->ring + t->tail, (u16_t)first, 0);
  if (first < p->tot_len)
    pbuf_copy_partial(p, t->ring, (u16_t)(p->tot_len - first), (u16_t)first);
  t->tail = (t->tail + p->tot_len) % t->rsize;
  t->count += p->tot_len;
}

static uint tcp_ring_get(struct tcp_sock *t, void *buffer, uint length, uint off,
                         int peek) {
  uint first, at;

  if (off >= t->count)
    return 0;
  if (length > t->count - off)
    length = t->count - off;
  at = (t->head + off) % t->rsize;
  first = t->rsize - at;
  if (first > length)
    first = length;
  memmove(buffer, t->ring + at, first);
  if (first < length)
    memmove((char *)buffer + first, t->ring, length - first);
  if (!peek) {
    t->head = (t->head + length) % t->rsize;
    t->count -= length;
  }
  return length;
}

static void tcp_open_window(struct tcp_pcb *pcb, uint n) {
  u16_t step;

  for (; n > 0; n -= step) {
    step = n > 0xffff ? 0xffff : (u16_t)n;
    tcp_recved(pcb, step);
  }
}

// a new connection offers its ring, not lwIP's ceiling
static void tcp_window_set(struct tcp_sock *t, struct tcp_pcb *pcb) {
  tcpwnd_size_t w = t->rsize - t->count;

  if (w > TCP_WND_MAX(pcb))
    w = TCP_WND_MAX(pcb);
  pcb->rcv_wnd = pcb->rcv_ann_wnd = w;
  pcb->rcv_ann_right_edge = pcb->rcv_nxt + w;
}

static void tcp_ring_grow(struct tcp_sock *t, struct tcp_pcb *pcb, uint size) {
  uchar *ring = malloc(size, M_DEVBUF, M_NOWAIT);
  uint old = t->rsize;

  if (ring == 0)
    return;
  tcp_ring_get(t, ring, t->count, 0, 1);
  free(t->ring, M_DEVBUF);
  t->ring = ring;
  t->head = 0;
  t->tail = t->count;
  t->rsize = size;
  tcp_open_window(pcb, size - old);
}

// half an RTT that brought 3/8 of the ring grows it by half
static void tcp_autorcvbuf(struct tcp_sock *t, struct tcp_pcb *pcb, uint len) {
  int64_t now = uptime_ns();
  uint size;

  if (!tcp_recvbuf_auto || !(pcb->flags & TF_WND_SCALE))
    return;
  if (t->srtt != 0 && t->rfbuf_ts != 0 && now - t->rfbuf_ts > (t->srtt >> 3) / 2) {
    if (t->rfbuf_cnt > t->rsize / 2 / 4 * 3 && t->rsize < (uint)tcp_recvbuf_max) {
      size = t->rsize + t->rsize / 2;
      tcp_ring_grow(t, pcb, size < (uint)tcp_recvbuf_max ? size : (uint)tcp_recvbuf_max);
    }
    t->rfbuf_ts = 0;
    t->rfbuf_cnt = 0;
  } else
    t->rfbuf_cnt += len;
}

// what is in flight is not held against the space
static uint tcp_hiwat(struct tcp_sock *t) {
  uint hiwat = t->sndwat;

  if (tcp_sendbuf_auto && t->pcb != 0)
    hiwat += t->pcb->snd_nxt - t->pcb->lastack;
  return hiwat < TCP_SND_BUF ? hiwat : TCP_SND_BUF;
}

static uint tcp_space(struct tcp_sock *t) {
  uint left = t->pcb->snd_buf, hiwat = tcp_hiwat(t), queued;

  queued = left < TCP_SND_BUF ? TCP_SND_BUF - left : 0;
  return hiwat > queued ? hiwat - queued : 0;
}

uint proninx_tcp_sndbuf(struct tcp_sock *t) {
  uint hiwat;

  net_enter();
  hiwat = tcp_hiwat(t);
  net_leave();
  return hiwat;
}

// one segment timed at a time, none resent
static void tcp_rtt_sample(struct tcp_sock *t, int64_t rtt) {
  if (rtt <= 0)
    rtt = 1;
  if (t->srtt == 0)
    t->srtt = rtt << 3;
  else if ((t->srtt += rtt - (t->srtt >> 3)) <= 0)
    t->srtt = 1;
}

static void tcp_rtt_acked(struct tcp_sock *t, struct tcp_pcb *pcb) {
  if (t->rtttime != 0 && TCP_SEQ_GT(pcb->lastack, t->rtseq)) {
    tcp_rtt_sample(t, uptime_ns() - t->rtttime);
    t->rtttime = 0;
  }
}

u32_t *proninx_tcp_out(struct pbuf *p, struct tcp_hdr *hdr, const struct tcp_pcb *pcb,
                       u32_t *opts) {
  struct tcp_sock *t = pcb != 0 ? pcb->callback_arg : 0;
  u32_t seq, end;

  if (t == 0 || t->state == TCP_LISTENING)
    return opts;
  if (t->rfbuf_ts == 0)
    t->rfbuf_ts = uptime_ns();
  seq = lwip_ntohl(hdr->seqno);
  end = seq + (u32_t)(p->tot_len - TCPH_HDRLEN_BYTES(hdr)) +
        ((TCPH_FLAGS(hdr) & TCP_SYN) != 0) + ((TCPH_FLAGS(hdr) & TCP_FIN) != 0);
  if (end == seq)
    return opts;
  if (!t->snd_max_ok) {
    t->snd_max = seq;
    t->snd_max_ok = 1;
  }
  if (TCP_SEQ_LT(seq, t->snd_max))
    t->rtttime = 0;
  else if (t->rtttime == 0) {
    t->rtttime = uptime_ns();
    t->rtseq = seq;
  }
  if (TCP_SEQ_GT(end, t->snd_max)) {
    t->snd_max = end;
    if (tcp_sendbuf_auto)
      netwake(t, 0, &t->wsel);
  }
  return opts;
}

// a SYN-ACK goes out right after this
static err_t tcp_synack_sent(u8_t id, struct tcp_pcb_listen *lpcb, struct tcp_pcb *cpcb) {
  (void)lpcb;
  tcp_ext_arg_set(cpcb, id, (void *)(uintptr_t)uptime_ns());
  return ERR_OK;
}

static const struct tcp_ext_arg_callbacks tcp_synack_cb = {0, tcp_synack_sent};

static err_t so_tcp_received(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                          err_t error) {
  struct tcp_sock *t = arg;

  (void)error;
  if (!p) {
    if (t->state == TCP_CONNECTED)
      t->state = TCP_PEER_CLOSED;
    if (t->txshut) {
      tcp_detach(t);
      t->pcb = 0;
    }
    netwake(t, &t->rsel, 0);
    return ERR_OK;
  }
  if (t->rxshut) {
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
  }
  tcp_autorcvbuf(t, pcb, p->tot_len);
  if (p->tot_len > t->rsize - t->count)
    return ERR_MEM;
  tcp_ring_put(t, p);
  pbuf_free(p);
  netwake(t, &t->rsel, 0);
  return ERR_OK;
}

static void so_tcp_error(void *arg, err_t error) {
  struct tcp_sock *t = arg;

  t->pcb = 0;
  if (error == ERR_CLSD)
    t->state = TCP_PEER_CLOSED;
  else {
    if (error == ERR_RST)
      t->error = t->state == TCP_CONNECTING ? FNU_ERR_REFUSED : FNU_ERR_RESET;
    else
      t->error = error == ERR_ABRT ? FNU_ERR_ABORTED : FNU_ERR_TIMEDOUT;
    t->state = TCP_ERROR;
  }
  netwake(t, &t->rsel, &t->wsel);
}

static err_t so_tcp_established(void *arg, struct tcp_pcb *pcb, err_t error) {
  struct tcp_sock *t = arg;

  (void)error;
  tcp_rtt_acked(t, pcb);
  tcp_window_set(t, pcb);
  t->state = TCP_CONNECTED;
  netwake(t, &t->rsel, &t->wsel);
  return ERR_OK;
}

static err_t so_tcp_sent(void *arg, struct tcp_pcb *pcb, u16_t length) {
  struct tcp_sock *t = arg;

  (void)length;
  tcp_rtt_acked(t, pcb);
  netwake(t, 0, &t->wsel);
  return ERR_OK;
}

static void tcp_install_callbacks(struct tcp_sock *t) {
  tcp_arg(t->pcb, t);
  tcp_recv(t->pcb, so_tcp_received);
  tcp_sent(t->pcb, so_tcp_sent);
  tcp_err(t->pcb, so_tcp_error);
}

static struct tcp_sock *tcp_sock_new(int flags) {
  struct tcp_sock *t = malloc(sizeof(*t), M_DEVBUF, flags | M_ZERO);

  if (t == 0)
    return 0;
  t->rsize = (uint)tcp_recvspace;
  t->sndwat = (uint)tcp_sendspace;
  if ((t->ring = malloc(t->rsize, M_DEVBUF, flags)) == 0) {
    free(t, M_DEVBUF);
    return 0;
  }
  t->acc_tail = &t->acc_head;
  return t;
}

static void tcp_sock_free(struct tcp_sock *t) {
  if (t->pcb != 0) {
    tcp_detach(t);
    if (tcp_close(t->pcb) != ERR_OK && t->state != TCP_LISTENING)
      tcp_abort(t->pcb);
  }
  free(t->ring, M_DEVBUF);
  free(t, M_DEVBUF);
  netif_poll_all();
}

static err_t so_tcp_accepted(void *arg, struct tcp_pcb *pcb, err_t error) {
  struct tcp_sock *listener = arg, *child;
  int64_t synack;

  if (error != ERR_OK || pcb == 0)
    return ERR_OK;
  if (listener->state != TCP_LISTENING || listener->acc_count >= listener->backlog ||
      (child = tcp_sock_new(M_NOWAIT)) == 0)
    return ERR_MEM;
  child->pcb = pcb;
  child->state = TCP_CONNECTED;
  child->snd_max = pcb->snd_nxt;
  child->snd_max_ok = 1;
  synack = (int64_t)(uintptr_t)tcp_ext_arg_get(pcb, tcp_synack_id);
  if (synack != 0 && pcb->nrtx == 0)
    tcp_rtt_sample(child, uptime_ns() - synack);
  tcp_window_set(child, pcb);
  tcp_install_callbacks(child);
  *listener->acc_tail = child;
  listener->acc_tail = &child->acc_next;
  listener->acc_count++;
  netwake(listener, &listener->rsel, 0);
  return ERR_OK;
}

int proninx_tcp_open(struct tcp_sock **out) {
  struct tcp_sock *t = tcp_sock_new(M_WAITOK);

  net_enter();
  if ((t->pcb = tcp_new()) == 0) {
    net_leave();
    free(t->ring, M_DEVBUF);
    free(t, M_DEVBUF);
    return -FNU_ERR_NOBUFFERS;
  }
  t->state = TCP_NEW;
  tcp_install_callbacks(t);
  net_leave();
  *out = t;
  return 0;
}

int proninx_tcp_bind(struct tcp_sock *t, const struct fnu_network_endpoint *local) {
  ip_addr_t address;
  int result;

  net_enter();
  if (t->state != TCP_NEW || t->pcb->local_port != 0) {
    net_leave();
    return -FNU_ERR_INVALID;
  }
  endpoint_to(local, &address);
  result = -errno_of(tcp_bind(t->pcb, &address, local->port));
  net_leave();
  return result;
}

int proninx_tcp_reuse(struct tcp_sock *t, int on) {
  net_enter();
  if (t->pcb == 0) {
    net_leave();
    return -FNU_ERR_INVALID;
  }
  if (on)
    ip_set_option(t->pcb, SOF_REUSEADDR);
  else
    ip_reset_option(t->pcb, SOF_REUSEADDR);
  net_leave();
  return 0;
}

int proninx_tcp_listen(struct tcp_sock *t, int backlog) {
  struct tcp_pcb *listener;

  if (backlog < 1 || backlog > somaxconn)
    backlog = somaxconn;
  net_enter();
  if (t->state != TCP_NEW || t->pcb->local_port == 0) {
    net_leave();
    return -FNU_ERR_INVALID;
  }
  tcp_ext_arg_set_callbacks(t->pcb, tcp_synack_id, &tcp_synack_cb);
  listener = tcp_listen_with_backlog(t->pcb, (u8_t)(backlog > 255 ? 255 : backlog));
  if (!listener) {
    net_leave();
    return -FNU_ERR_NOBUFFERS;
  }
  t->pcb = listener;
  t->state = TCP_LISTENING;
  t->backlog = backlog;
  tcp_arg(listener, t);
  tcp_accept(listener, so_tcp_accepted);
  net_leave();
  return 0;
}

int proninx_tcp_connect(struct tcp_sock *t, const struct fnu_network_endpoint *endpoint,
                        uint timeout_ms) {
  ip_addr_t address;
  u32_t deadline;
  err_t err;
  int result;

  if (!endpoint->port)
    return -FNU_ERR_INVALID;
  net_enter();
  if (t->state == TCP_CONNECTING) {
    net_leave();
    return -FNU_ERR_ALREADY;
  }
  if (t->state != TCP_NEW) {
    net_leave();
    return t->state == TCP_CONNECTED ? -FNU_ERR_CONNECTED : -FNU_ERR_INVALID;
  }
  endpoint_to(endpoint, &address);
  t->state = TCP_CONNECTING;
  err = tcp_connect(t->pcb, &address, endpoint->port, so_tcp_established);
  if (err != ERR_OK) {
    t->state = TCP_NEW;
    net_leave();
    return -errno_of(err);
  }
  netif_poll_all();
  if (timeout_ms == 0) {
    net_leave();
    return -FNU_ERR_INPROGRESS;
  }
  deadline = deadline_of(timeout_ms);
  result = 0;
  while (t->state == TCP_CONNECTING)
    if (netsleep(t, deadline) < 0) {
      result = proc_interrupted() ? -FNU_ERR_INTERRUPTED : -FNU_ERR_TIMEDOUT;
      break;
    }
  if (t->state == TCP_CONNECTING) {
    tcp_detach(t);
    tcp_abort(t->pcb);
    t->pcb = 0;
    t->state = TCP_ERROR;
    t->error = FNU_ERR_TIMEDOUT;
  } else if (t->state != TCP_CONNECTED)
    result = -t->error;
  net_leave();
  return result;
}

static struct tcp_sock *tcp_accept_next(struct tcp_sock *t) {
  struct tcp_sock *child = t->acc_head;

  if ((t->acc_head = child->acc_next) == 0)
    t->acc_tail = &t->acc_head;
  child->acc_next = 0;
  t->acc_count--;
  return child;
}

int proninx_tcp_accept(struct tcp_sock *t, uint timeout_ms, struct tcp_sock **out) {
  struct tcp_sock *child;
  u32_t deadline;

  net_enter();
  if (t->state != TCP_LISTENING) {
    net_leave();
    return -FNU_ERR_INVALID;
  }
  deadline = deadline_of(timeout_ms);
  for (;;) {
    while (t->acc_head == 0)
      if (timeout_ms == 0 || netsleep(t, deadline) < 0) {
        net_leave();
        return waiterr();
      }
    child = tcp_accept_next(t);
    if (child->state == TCP_CONNECTED || child->state == TCP_PEER_CLOSED)
      break;
    tcp_sock_free(child);
  }
  net_leave();
  *out = child;
  return 0;
}

int proninx_tcp_send(struct tcp_sock *t, const void *buffer, uint length,
                     uint timeout_ms) {
  u32_t deadline;
  uint space;
  err_t result;

  net_enter();
  deadline = deadline_of(timeout_ms);
  for (;;) {
    if (t->state == TCP_ERROR) {
      net_leave();
      return -t->error;
    }
    if (t->state != TCP_CONNECTED && t->state != TCP_PEER_CLOSED) {
      net_leave();
      return -FNU_ERR_NOTCONNECTED;
    }
    if (t->txshut || t->pcb == 0) {
      net_leave();
      return -FNU_ERR_BROKENPIPE;
    }
    space = tcp_space(t);
    if (space > length)
      space = length;
    if (space > 0xffff)
      space = 0xffff;
    result = space == 0 ? ERR_MEM
                        : tcp_write(t->pcb, buffer, (u16_t)space, TCP_WRITE_FLAG_COPY);
    if (result == ERR_OK)
      break;
    if (result != ERR_MEM) {
      net_leave();
      return -errno_of(result);
    }
    if (timeout_ms == 0 || netsleep(t, deadline) < 0) {
      net_leave();
      return waiterr();
    }
  }
  tcp_output(t->pcb);
  netif_poll_all();
  net_leave();
  return (int)space;
}

int proninx_tcp_recv(struct tcp_sock *t, void *buffer, uint length, uint off,
                     uint timeout_ms, int peek) {
  u32_t deadline;
  uint n;

  net_enter();
  if (t->state == TCP_NEW || t->state == TCP_CONNECTING || t->state == TCP_LISTENING) {
    net_leave();
    return -FNU_ERR_NOTCONNECTED;
  }
  if (peek && off >= t->rsize) {
    net_leave();
    return 0;
  }
  deadline = deadline_of(timeout_ms);
  while (t->count <= off) {
    if (t->rxshut || t->state == TCP_PEER_CLOSED) {
      net_leave();
      return 0;
    }
    if (t->state == TCP_ERROR) {
      net_leave();
      return -t->error;
    }
    if (timeout_ms == 0 || netsleep(t, deadline) < 0) {
      net_leave();
      return waiterr();
    }
  }
  n = tcp_ring_get(t, buffer, length, off, peek);
  if (!peek && t->pcb != 0)
    tcp_open_window(t->pcb, n);
  net_leave();
  return (int)n;
}

int proninx_tcp_readable(struct tcp_sock *t) {
  int ready;

  net_enter();
  ready = t->count != 0 || t->acc_head != 0 || t->rxshut ||
          t->state == TCP_PEER_CLOSED || t->state == TCP_ERROR;
  if (!ready)
    selrecord(&t->rsel);
  net_leave();
  return ready;
}

int proninx_tcp_writable(struct tcp_sock *t) {
  int ready = 0;

  net_enter();
  if (t->state == TCP_ERROR || t->txshut)
    ready = 1;
  else if (t->state == TCP_CONNECTED || t->state == TCP_PEER_CLOSED)
    ready = t->pcb != 0 && tcp_space(t) > 0;
  if (!ready)
    selrecord(&t->wsel);
  net_leave();
  return ready;
}

int64_t proninx_tcp_avail(struct tcp_sock *t, int write, int *eof) {
  int64_t n;

  net_enter();
  if (write) {
    *eof = t->state == TCP_ERROR || t->txshut;
    n = t->pcb != 0 && (t->state == TCP_CONNECTED || t->state == TCP_PEER_CLOSED)
            ? tcp_space(t)
            : 0;
  } else {
    *eof = t->rxshut || t->state == TCP_PEER_CLOSED || t->state == TCP_ERROR;
    n = t->acc_head != 0 ? t->acc_count : t->count;
  }
  net_leave();
  return n;
}

int proninx_tcp_takeerror(struct tcp_sock *t) {
  int error;

  net_enter();
  error = t->error;
  t->error = 0;
  net_leave();
  return error;
}

int proninx_tcp_local(struct tcp_sock *t, struct fnu_network_endpoint *out) {
  net_enter();
  if (t->pcb == 0) {
    net_leave();
    return -FNU_ERR_INVALID;
  }
  endpoint_from(out, &t->pcb->local_ip, t->pcb->local_port);
  net_leave();
  return 0;
}

int proninx_tcp_peer(struct tcp_sock *t, struct fnu_network_endpoint *out) {
  net_enter();
  if (t->pcb == 0 || (t->state != TCP_CONNECTED && t->state != TCP_PEER_CLOSED)) {
    net_leave();
    return -FNU_ERR_NOTCONNECTED;
  }
  endpoint_from(out, &t->pcb->remote_ip, t->pcb->remote_port);
  net_leave();
  return 0;
}

int proninx_tcp_shutdown(struct tcp_sock *t, int rx, int tx) {
  uint dropped;
  int result = 0;

  net_enter();
  if (t->state != TCP_CONNECTED && t->state != TCP_PEER_CLOSED) {
    net_leave();
    return -FNU_ERR_NOTCONNECTED;
  }
  if (rx && !t->rxshut) {
    t->rxshut = 1;
    dropped = t->count;
    t->head = t->tail = t->count = 0;
    if (t->pcb != 0 && dropped != 0)
      tcp_open_window(t->pcb, dropped);
  }
  if (tx && !t->txshut) {
    err_t err = t->pcb != 0 ? tcp_shutdown(t->pcb, 0, 1) : ERR_OK;

    if (err != ERR_OK)
      result = -errno_of(err);
    else
      t->txshut = 1;
  }
  netif_poll_all();
  netwake(t, &t->rsel, &t->wsel);
  net_leave();
  return result;
}

void proninx_tcp_close(struct tcp_sock *t) {
  net_enter();
  while (t->acc_head != 0)
    tcp_sock_free(tcp_accept_next(t));
  tcp_sock_free(t);
  net_leave();
}

uint32_t proninx_lwip_random(void) {
  static uint32_t pool[16];
  static int left;
  uint32_t v;

  if (left == 0) {
    random_bytes(pool, sizeof(pool));
    left = (int)(sizeof(pool) / sizeof(pool[0]));
  }
  v = pool[--left];
  pool[left] = 0;
  return v;
}

// RFC 6528: the 4 us clock plus a keyed PRF of the 4-tuple
u32_t proninx_tcp_isn(const ip_addr_t *local_ip, u16_t local_port,
                      const ip_addr_t *remote_ip, u16_t remote_port) {
  static uchar secret[16];
  static int have_secret;
  struct {
    u16_t fport, lport;
    u32_t faddr, laddr;
  } tuple;
  uint64_t h;

  if (!have_secret) {
    random_bytes(secret, sizeof(secret));
    have_secret = 1;
  }
  memset(&tuple, 0, sizeof(tuple));
  tuple.fport = remote_port;
  tuple.lport = local_port;
  tuple.faddr = ip4_addr_get_u32(ip_2_ip4(remote_ip));
  tuple.laddr = ip4_addr_get_u32(ip_2_ip4(local_ip));
  h = siphash24(secret, &tuple, sizeof(tuple));
  return (u32_t)(h ^ h >> 32) + (u32_t)(uptime_ns() / 4000);
}

