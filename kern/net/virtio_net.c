#include "if.h"
#include "defs.h"
#include "driver.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "mutex.h"
#include "x86.h"

#define VIRTIO_HOST_FEATURES 0
#define VIRTIO_GUEST_FEATURES 4
#define VIRTIO_QUEUE_PFN 8
#define VIRTIO_QUEUE_NUM 12
#define VIRTIO_QUEUE_SEL 14
#define VIRTIO_QUEUE_NOTIFY 16
#define VIRTIO_STATUS 18
#define VIRTIO_ISR 19
#define VIRTIO_CONFIG 20 // without MSI-X
#define VIRTIO_ISR_QUEUE 1
#define VIRTIO_ISR_CONFIG 2
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED 128
#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_STATUS 16
#define VIRTIO_NET_S_LINK_UP 1
#define VIRTIO_NET_HDR_SIZE 10
#define VRING_DESC_F_WRITE 2
#define VRING_ALIGN 4096

#define VTNET_RXQ 0
#define VTNET_TXQ 1
#define VTNET_BUFSIZE 2048

struct vring_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __packed;

struct vring_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[];
} __packed;

struct vring_used_elem {
  uint32_t id;
  uint32_t len;
} __packed;

struct vring_used {
  uint16_t flags;
  uint16_t idx;
  struct vring_used_elem ring[];
} __packed;

struct vtnet_vq {
  struct vring_desc *desc;
  struct vring_avail *avail;
  struct vring_used *used;
  void *mem;
  uint16_t num;
  uint16_t used_idx;
  char **buf;     // each descriptor's own
  uchar *busy;    // the device has it
  uint16_t hint;
};

struct vtnet_softc {
  struct device *dev;
  struct ifnet ifnet;
  struct mutex lock;
  ushort iobase;
  uint32_t features;
  int running;
  struct vtnet_vq rx, tx;
};

static uint64_t vring_size(uint num) {
  uint64_t a = (uint64_t)num * sizeof(struct vring_desc) + sizeof(uint16_t) * (3 + num);

  a = (a + VRING_ALIGN - 1) & ~(uint64_t)(VRING_ALIGN - 1);
  return a + sizeof(uint16_t) * 3 + sizeof(struct vring_used_elem) * num;
}

static int vq_setup(struct vtnet_softc *sc, struct vtnet_vq *vq, int index) {
  uint64_t avail_end;
  uint i;

  outw(sc->iobase + VIRTIO_QUEUE_SEL, (ushort)index);
  if (vq->mem == 0) {
    vq->num = inw(sc->iobase + VIRTIO_QUEUE_NUM);
    if (vq->num == 0 || (vq->mem = dma_alloc(vring_size(vq->num))) == 0)
      return -1;
    vq->buf = malloc(vq->num * sizeof(*vq->buf), M_DEVBUF, M_WAITOK | M_ZERO);
    vq->busy = malloc(vq->num, M_DEVBUF, M_WAITOK);
    for (i = 0; i < vq->num; i += PGSIZE / VTNET_BUFSIZE) {
      char *p = page_alloc();
      uint k;

      if (p == 0)
        return -1;
      for (k = 0; k < PGSIZE / VTNET_BUFSIZE && i + k < vq->num; k++)
        vq->buf[i + k] = p + k * VTNET_BUFSIZE;
    }
  }
  memset(vq->mem, 0, vring_size(vq->num));
  memset(vq->busy, 0, vq->num);
  vq->desc = vq->mem;
  vq->avail = (struct vring_avail *)((char *)vq->mem + vq->num * sizeof(struct vring_desc));
  avail_end = (uint64_t)vq->num * sizeof(struct vring_desc) + sizeof(uint16_t) * (3 + vq->num);
  vq->used = (struct vring_used *)((char *)vq->mem +
                                   ((avail_end + VRING_ALIGN - 1) & ~(uint64_t)(VRING_ALIGN - 1)));
  vq->used_idx = 0;
  vq->hint = 0;
  outl(sc->iobase + VIRTIO_QUEUE_PFN, (uint32_t)(V2P(vq->mem) / VRING_ALIGN));
  return 0;
}

static void vq_offer(struct vtnet_vq *vq, uint16_t id, uint32_t len, uint16_t flags) {
  vq->desc[id].addr = V2P(vq->buf[id]);
  vq->desc[id].len = len;
  vq->desc[id].flags = flags;
  vq->desc[id].next = 0;
  vq->busy[id] = 1;
  mb();
  vq->avail->ring[vq->avail->idx % vq->num] = id;
  mb();
  vq->avail->idx++;
}

static void vtnet_update_link(struct vtnet_softc *sc) {
  uint16_t status;

  if (!(sc->features & (1u << VIRTIO_NET_F_STATUS))) {
    if_link_state_change(&sc->ifnet, LINK_STATE_UP, 0);
    return;
  }
  status = inw(sc->iobase + VIRTIO_CONFIG + ETHER_ADDR_LEN);
  if_link_state_change(&sc->ifnet,
                       (status & VIRTIO_NET_S_LINK_UP) ? LINK_STATE_UP : LINK_STATE_DOWN, 0);
}

static int vtnet_transmit(struct ifnet *ifp, const void *frame, uint len) {
  struct vtnet_softc *sc = ifp->if_softc;
  struct vtnet_vq *vq = &sc->tx;
  uint16_t i, n;

  if (len + VIRTIO_NET_HDR_SIZE > VTNET_BUFSIZE)
    return -1;
  mtx_enter(&sc->lock);
  if (!sc->running) {
    mtx_leave(&sc->lock);
    return -1;
  }
  for (n = 0, i = vq->hint; n < vq->num && vq->busy[i]; n++)
    i = (uint16_t)((i + 1) % vq->num);
  if (n == vq->num) {
    mtx_leave(&sc->lock);
    return -1;
  }
  vq->hint = (uint16_t)((i + 1) % vq->num);
  memset(vq->buf[i], 0, VIRTIO_NET_HDR_SIZE);
  memmove(vq->buf[i] + VIRTIO_NET_HDR_SIZE, frame, len);
  vq_offer(vq, i, len + VIRTIO_NET_HDR_SIZE, 0);
  outw(sc->iobase + VIRTIO_QUEUE_NOTIFY, VTNET_TXQ);
  mtx_leave(&sc->lock);
  return 0;
}

static int vtnet_intr(struct device *dev) {
  struct vtnet_softc *sc = dev->driver_data;
  struct vring_used_elem *e;
  struct vtnet_vq *vq;
  int refilled = 0;
  uint8_t isr;

  if (sc == 0)
    return 0;
  mtx_enter(&sc->lock);
  if (!sc->running || (isr = inb(sc->iobase + VIRTIO_ISR)) == 0) {
    mtx_leave(&sc->lock);
    return 0;
  }
  vq = &sc->tx;
  while (vq->used_idx != vq->used->idx) {
    e = &vq->used->ring[vq->used_idx % vq->num];
    if (e->id < vq->num)
      vq->busy[e->id] = 0;
    vq->used_idx++;
  }
  vq = &sc->rx;
  while (vq->used_idx != vq->used->idx) {
    e = &vq->used->ring[vq->used_idx % vq->num];
    if (e->id < vq->num) {
      if (e->len > VIRTIO_NET_HDR_SIZE && e->len <= VTNET_BUFSIZE)
        if_input(&sc->ifnet, vq->buf[e->id] + VIRTIO_NET_HDR_SIZE,
                 e->len - VIRTIO_NET_HDR_SIZE);
      vq_offer(vq, (uint16_t)e->id, VTNET_BUFSIZE, VRING_DESC_F_WRITE);
      refilled = 1;
    }
    vq->used_idx++;
  }
  if (refilled)
    outw(sc->iobase + VIRTIO_QUEUE_NOTIFY, VTNET_RXQ);
  if (isr & VIRTIO_ISR_CONFIG)
    vtnet_update_link(sc);
  mtx_leave(&sc->lock);
  return 1;
}

static void vq_free(struct vtnet_vq *vq) {
  uint i;

  if (vq->buf != 0) {
    for (i = 0; i < vq->num; i += PGSIZE / VTNET_BUFSIZE)
      if (vq->buf[i] != 0)
        page_free(vq->buf[i]);
    free(vq->buf, M_DEVBUF);
    free(vq->busy, M_DEVBUF);
  }
  if (vq->mem != 0)
    dma_free(vq->mem);
}

static void vtnet_fail(struct vtnet_softc *sc) {
  outb(sc->iobase + VIRTIO_STATUS, 0);
  vq_free(&sc->rx);
  vq_free(&sc->tx);
  outb(sc->iobase + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FAILED);
}

static int vtnet_probe(struct device *dev) {
  if (dev->bus != BUS_TYPE_PCI)
    return 0;
  if (dev->pci.vendor_id == PCI_VENDOR_VIRTIO &&
      dev->pci.device_id == PCI_DEVICE_VIRTIO_NET)
    return 50;
  return 0;
}

static int vtnet_init(struct vtnet_softc *sc) {
  uint32_t host;
  uint16_t i;

  outb(sc->iobase + VIRTIO_STATUS, 0);
  outb(sc->iobase + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
  outb(sc->iobase + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
  host = inl(sc->iobase + VIRTIO_HOST_FEATURES);
  if ((host & (1u << VIRTIO_NET_F_MAC)) == 0) {
    printf("%s: the device has no address of its own\n", sc->dev->name);
    return -1;
  }
  sc->features = host & ((1u << VIRTIO_NET_F_MAC) | (1u << VIRTIO_NET_F_STATUS));
  outl(sc->iobase + VIRTIO_GUEST_FEATURES, sc->features);
  if (vq_setup(sc, &sc->rx, VTNET_RXQ) < 0 || vq_setup(sc, &sc->tx, VTNET_TXQ) < 0) {
    printf("%s: no memory for the queues\n", sc->dev->name);
    return -1;
  }
  for (i = 0; i < sc->rx.num; i++)
    vq_offer(&sc->rx, i, VTNET_BUFSIZE, VRING_DESC_F_WRITE);
  outb(sc->iobase + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                       VIRTIO_STATUS_DRIVER_OK);
  outw(sc->iobase + VIRTIO_QUEUE_NOTIFY, VTNET_RXQ);
  mtx_enter(&sc->lock);
  sc->running = 1;
  vtnet_update_link(sc);
  mtx_leave(&sc->lock);
  return 0;
}

static int vtnet_attach(struct device *dev) {
  struct vtnet_softc *sc;
  uint i;

  if (dev->io_base == 0)
    return -1;
  // MSI-X
  if (driver_setup_intr_best(dev, PCI_INTR_MSI) < 0) {
    printf("%s: no interrupt\n", dev->name);
    return -1;
  }
  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->dev = dev;
  sc->iobase = dev->io_base;
  mtx_init(&sc->lock, "vtnet");
  dev->driver_data = sc;
  strlcpy(sc->ifnet.if_xname, dev->name, sizeof(sc->ifnet.if_xname));
  sc->ifnet.if_mtu = ETHERMTU;
  sc->ifnet.if_transmit = vtnet_transmit;
  sc->ifnet.if_softc = sc;
  if (vtnet_init(sc) < 0) {
    vtnet_fail(sc);
    dev->driver_data = 0;
    free(sc, M_DEVBUF);
    return -1;
  }
  for (i = 0; i < ETHER_ADDR_LEN; i++)
    sc->ifnet.if_lladdr[i] = inb(sc->iobase + VIRTIO_CONFIG + i);
  printf("%s: virtio network device, queues of %d and %d\n", dev->name,
          sc->rx.num, sc->tx.num);
  if_attach(&sc->ifnet);
  return 0;
}

// SLEEP
static int vtnet_suspend(struct device *dev) {
  struct vtnet_softc *sc = dev->driver_data;

  mtx_enter(&sc->lock);
  sc->running = 0;
  mtx_leave(&sc->lock);
  outb(sc->iobase + VIRTIO_STATUS, 0);
  return 0;
}

static int vtnet_resume(struct device *dev) {
  struct vtnet_softc *sc = dev->driver_data;

  if (vtnet_init(sc) < 0) {
    outb(sc->iobase + VIRTIO_STATUS,
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FAILED);
    return -1;
  }
  return 0;
}

static struct driver vtnet_driver = {
    .name = "vtnet",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_NET,
    .probe = vtnet_probe,
    .attach = vtnet_attach,
    .intr = vtnet_intr,
    .suspend = vtnet_suspend,
    .resume = vtnet_resume,
};

void virtio_net_driver_init(void) { driver_register(&vtnet_driver); }
