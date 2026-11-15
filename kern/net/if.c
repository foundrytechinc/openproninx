// INTERFACES
#include "if.h"
#include "defs.h"
#include "inc/abi.h"
#include "malloc.h"
#include "mutex.h"

static struct {
  struct mutex lock;
  struct ifnet *head, **tail;
  int n;
} ifs;

void ifinit(void) {
  mtx_init(&ifs.lock, "ifnet");
  ifs.tail = &ifs.head;
}

void if_attach(struct ifnet *ifp) {
  mtx_enter(&ifs.lock);
  ifp->if_next = 0;
  ifp->if_index = ++ifs.n;
  __atomic_store_n(ifs.tail, ifp, __ATOMIC_RELEASE);
  ifs.tail = &ifp->if_next;
  mtx_leave(&ifs.lock);
  printf("%s: address %02x:%02x:%02x:%02x:%02x:%02x\n", ifp->if_xname, ifp->if_lladdr[0],
          ifp->if_lladdr[1], ifp->if_lladdr[2], ifp->if_lladdr[3],
          ifp->if_lladdr[4], ifp->if_lladdr[5]);
  net_stack_attach(ifp);
}

struct ifnet *if_first(void) {
  return __atomic_load_n(&ifs.head, __ATOMIC_ACQUIRE);
}

struct ifnet *ifunit(const char *name) {
  struct ifnet *ifp;

  for (ifp = if_first(); ifp != 0; ifp = __atomic_load_n(&ifp->if_next, __ATOMIC_ACQUIRE))
    if (strncmp(ifp->if_xname, name, IFNAMSIZ) == 0)
      return ifp;
  return 0;
}

// from the driver's interrupt
void if_input(struct ifnet *ifp, const void *frame, uint len) {
  if (len < ETHER_HDR_LEN || len > ETHER_MAX_LEN) {
    __atomic_fetch_add(&ifp->if_iqdrops, 1, __ATOMIC_RELAXED);
    return;
  }
  __atomic_fetch_add(&ifp->if_ipackets, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ifp->if_ibytes, len, __ATOMIC_RELAXED);
  net_stack_input(ifp, frame, len);
}

int if_output(struct ifnet *ifp, const void *frame, uint len) {
  if (len > ETHER_MAX_LEN || ifp->if_transmit(ifp, frame, len) < 0) {
    __atomic_fetch_add(&ifp->if_oerrors, 1, __ATOMIC_RELAXED);
    return -1;
  }
  __atomic_fetch_add(&ifp->if_opackets, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&ifp->if_obytes, len, __ATOMIC_RELAXED);
  return 0;
}

void if_link_state_change(struct ifnet *ifp, int state, uint64_t baudrate) {
  if (ifp->if_link_state == state && ifp->if_baudrate == baudrate)
    return;
  ifp->if_link_state = state;
  ifp->if_baudrate = baudrate;
  if (LINK_STATE_IS_UP(state) && state != LINK_STATE_UNKNOWN && baudrate != 0)
    printf("%s: link up, %lu Mbps%s\n", ifp->if_xname, baudrate / 1000000,
            state == LINK_STATE_FULL_DUPLEX ? " full duplex" : "");
  else if (LINK_STATE_IS_UP(state) && state != LINK_STATE_UNKNOWN)
    printf("%s: link up\n", ifp->if_xname);
  else if (state == LINK_STATE_DOWN)
    printf("%s: link down\n", ifp->if_xname);
  net_stack_link();
}

int if_info_all(struct fnu_ifinfo *buf, int capacity) {
  struct ifnet *ifp;
  struct fnu_ifinfo *e;
  int n = 0;

  for (ifp = if_first(); ifp != 0;
       ifp = __atomic_load_n(&ifp->if_next, __ATOMIC_ACQUIRE), n++) {
    if (n >= capacity)
      continue;
    e = &buf[n];
    memset(e, 0, sizeof(*e));
    strlcpy(e->name, ifp->if_xname, sizeof(e->name));
    e->index = (uint32)ifp->if_index;
    e->mtu = ifp->if_mtu;
    memmove(e->lladdr, ifp->if_lladdr, ETHER_ADDR_LEN);
    e->link = (uint8)ifp->if_link_state;
    e->baudrate = ifp->if_baudrate;
    e->ipackets = __atomic_load_n(&ifp->if_ipackets, __ATOMIC_RELAXED);
    e->ibytes = __atomic_load_n(&ifp->if_ibytes, __ATOMIC_RELAXED);
    e->iqdrops = __atomic_load_n(&ifp->if_iqdrops, __ATOMIC_RELAXED);
    e->opackets = __atomic_load_n(&ifp->if_opackets, __ATOMIC_RELAXED);
    e->obytes = __atomic_load_n(&ifp->if_obytes, __ATOMIC_RELAXED);
    e->oerrors = __atomic_load_n(&ifp->if_oerrors, __ATOMIC_RELAXED);
    net_stack_ifinfo(ifp, e);
  }
  return n;
}
