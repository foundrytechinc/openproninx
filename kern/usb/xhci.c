// XHCI

#include "defs.h"
#include "driver.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "proc.h"
#include "mutex.h"
#include "sleeplock.h"
#include "trap.h"
#include "usbdi.h"
#include "x86.h"
#include "xhcireg.h"

#define CMD_TIMEOUT_MS 500
#define XFER_TIMEOUT_MS 500
#define DCI_MAX 32

struct xhci_ring {
  struct xhci_trb *trb;
  uint64_t phys;
  uint32_t idx;
  uint32_t cycle;
};

struct xhci_slot {
  int id;
  struct usb_device *dev;
  uint8_t *devctx;
  uint8_t *inctx;
  uint64_t inctx_phys;
  uint8_t *ctlbuf;
  uint64_t ctlbuf_phys;
  struct xhci_ring ring[DCI_MAX];
  struct usb_pipe *pipe[DCI_MAX];
  uint64_t trb_at[DCI_MAX];
  uint32_t span_lo[DCI_MAX];
  uint32_t final_lo[DCI_MAX]; // its last descriptor begins
  uint32_t span_hi[DCI_MAX];
  int maxdci;
  volatile int ctl_done;
  volatile uint32_t ctl_status;
  volatile int gone;
};

struct xhci_softc {
  struct device *dev;
  volatile uint8_t *cap;
  volatile uint8_t *opr;
  volatile uint8_t *rts;
  volatile uint8_t *dbl;

  struct mutex lock;

  uint32_t maxslots;
  uint32_t maxports;
  uint32_t ctxsize;
  int wide_dma;

  uint64_t *dcbaa;
  uint64_t dcbaa_phys;
  struct xhci_ring cmd;

  struct xhci_trb *evt;
  uint64_t evt_phys;
  uint32_t evt_idx;
  uint32_t evt_cycle;
  struct xhci_erseg *erst;

  struct sleeplock cmdlock;
  volatile int cmd_done;
  int fresh; // reset at resume: the slots of before are no more
  uint64_t cmd_at;
  struct xhci_trb cmd_evt;

  struct xhci_slot **slot;
  uint64_t *scratch;
  uint32_t nscratch;
  int scratch_dma;

  struct usb_bus bus;
  struct usb_device root;
  volatile int port_changed;
  int dead;
  uint8_t *port_major;
  uint8_t *port_state;
};

// REGISTERS

static inline uint32_t rd4(volatile uint8_t *p, uint32_t off) {
  return *(volatile uint32_t *)(p + off);
}

static inline void wr4(volatile uint8_t *p, uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(p + off) = v;
}

static inline uint8_t rd1(volatile uint8_t *p, uint32_t off) {
  return *(volatile uint8_t *)(p + off);
}

static inline void wr1(volatile uint8_t *p, uint32_t off, uint8_t v) {
  *(volatile uint8_t *)(p + off) = v;
}

static void *dma_page(struct xhci_softc *sc, uint64_t *phys) {
  char *p = page_alloc();
  uint64_t pa;

  if (p == 0)
    return 0;
  pa = (uint64_t)V2P(p);
  if (!sc->wide_dma && (pa >> 32) != 0) {
    page_free(p);
    return 0;
  }
  memset(p, 0, PGSIZE);
  if (phys)
    *phys = pa;
  return p;
}

// RINGS

static int ring_init(struct xhci_softc *sc, struct xhci_ring *r) {
  uint64_t phys;

  if (r->trb != 0)
    return 0;
  r->trb = dma_page(sc, &phys);
  if (r->trb == 0)
    return -1;
  r->phys = phys;
  r->idx = 0;
  r->cycle = 1;
  r->trb[XHCI_RING_TRBS - 1].params = phys;
  r->trb[XHCI_RING_TRBS - 1].control =
      XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC;
  return 0;
}

static void ring_free(struct xhci_ring *r) {
  if (r->trb) {
    page_free((char *)r->trb);
    r->trb = 0;
  }
  r->phys = 0;
  r->idx = 0;
  r->cycle = 0;
}

static uint64_t ring_push(struct xhci_ring *r, uint64_t params, uint32_t status,
                          uint32_t control) {
  struct xhci_trb *t = &r->trb[r->idx];
  uint64_t at = r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE;

  t->params = params;
  t->status = status;
  mb();
  t->control = control | (r->cycle ? XHCI_TRB_CYCLE : 0);
  mb();

  r->idx++;
  if (r->idx == XHCI_RING_TRBS - 1) {
    struct xhci_trb *l = &r->trb[r->idx];
    l->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC |
                 (r->cycle ? XHCI_TRB_CYCLE : 0);
    mb();
    r->idx = 0;
    r->cycle ^= 1;
  }
  return at;
}

static uint64_t push_td(struct xhci_ring *r, uint64_t phys, uint32_t len,
                        uint32_t maxp, int in) {
  uint32_t left = len, first, npkt;
  uint64_t at = 0;

  first = PGSIZE - (uint32_t)(phys & (PGSIZE - 1));
  if (first > left)
    first = left;
  do {
    uint32_t n = left > first ? first : left;
    uint32_t control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL);

    left -= n;
    npkt = maxp != 0 ? (left + maxp - 1) / maxp : 0;
    if (npkt > 31)
      npkt = 31;
    if (in)
      control |= XHCI_TRB_ISP;
    if (left != 0)
      control |= XHCI_TRB_CHAIN;
    else
      control |= XHCI_TRB_IOC;
    at = ring_push(r, phys, XHCI_TRB_LEN(n) | XHCI_TRB_TDREM(npkt) |
                                XHCI_TRB_INTR(0),
                   control);
    phys += n;
    first = PGSIZE;
  } while (left != 0);
  return at;
}

static int ring_covers(const struct xhci_ring *r, uint32_t lo, uint32_t hi,
                       uint64_t at) {
  uint32_t i;

  if (at < r->phys || at >= r->phys + (uint64_t)XHCI_RING_TRBS * XHCI_TRB_SIZE)
    return 0;
  i = (uint32_t)((at - r->phys) / XHCI_TRB_SIZE);
  if (lo <= hi)
    return i >= lo && i <= hi;
  return i >= lo || i <= hi;
}

static void doorbell(struct xhci_softc *sc, uint32_t slot, uint32_t target) {
  mb();
  wr4(sc->dbl, XHCI_DOORBELL(slot), target);
  (void)rd4(sc->dbl, XHCI_DOORBELL(slot));
}

static void erdp_write(struct xhci_softc *sc) {
  uint64_t at = sc->evt_phys + (uint64_t)sc->evt_idx * XHCI_TRB_SIZE;

  wr4(sc->rts, XHCI_ERDP_LO(0), (uint32_t)at | XHCI_ERDP_LO_BUSY);
  wr4(sc->rts, XHCI_ERDP_HI(0), (uint32_t)(at >> 32));
}

// with the lock held
static struct xhci_slot *slot_of(struct xhci_softc *sc, uint32_t id) {
  return id == 0 || id > sc->maxslots ? 0 : sc->slot[id];
}

// 4.11.2.3: bursts per interval
static uint32_t isoc_burst(struct usb_pipe *p, int len, uint32_t *tlbpc) {
  uint32_t packets = ((uint32_t)len + p->maxp - 1) / p->maxp;
  uint32_t perburst = (uint32_t)p->burst + 1;
  uint32_t residue;

  if (packets == 0)
    packets = 1;
  if (p->dev->speed == USB_SPEED_SUPER) {
    residue = packets % perburst;
    *tlbpc = residue == 0 ? perburst - 1 : residue - 1;
  } else {
    *tlbpc = packets - 1;
  }
  return (packets + perburst - 1) / perburst - 1;
}

static void isoc_push(struct xhci_ring *r, struct usb_pipe *p, int seg) {
  uint32_t tlbpc, tbc = isoc_burst(p, p->seglen, &tlbpc);
  int in = UE_GET_DIR(p->edesc.bEndpointAddress) != 0;

  ring_push(r, p->buf_phys + (uint64_t)seg * USB_ISOC_STRIDE,
            XHCI_TRB_LEN(p->seglen) | XHCI_TRB_INTR(0),
            XHCI_TRB_TYPE(XHCI_TRB_ISOCH) | XHCI_TRB_ISOCH_SIA | XHCI_TRB_IOC |
                (in ? XHCI_TRB_ISP : 0) | XHCI_TRB_ISOC_TBC(tbc) |
                XHCI_TRB_ISOC_TLBPC(tlbpc));
}

// THE EVENT RING

static void transfer_event(struct xhci_softc *sc, const struct xhci_trb *e) {
  struct xhci_slot *s = slot_of(sc, XHCI_TRB_GET_SLOT(e->control));
  uint32_t dci = XHCI_TRB_GET_EP(e->control);
  uint32_t code = XHCI_TRB_GET_CODE(e->status);
  struct usb_pipe *p;
  int len, status;

  if (s == 0 || dci >= DCI_MAX)
    return;
  // a stopped TD stays on the ring, goes on at the doorbell
  if (code == XHCI_CODE_STOPPED || code == XHCI_CODE_STOPPED_LEN ||
      code == XHCI_CODE_STOPPED_SHORT)
    return;

  if (dci == 1) {
    if (e->params != s->trb_at[1] && code == XHCI_CODE_SHORT_PKT)
      return;
    s->ctl_status = code;
    s->ctl_done = 1;
    return;
  }

  p = s->pipe[dci];
  if (p == 0)
    return;
  if (!p->isoc && !p->repeat &&
      !ring_covers(&s->ring[dci], s->span_lo[dci], s->span_hi[dci], e->params))
    return;
  len = (int)((uint32_t)p->xfer_len - XHCI_TRB_GET_LEN(e->status));
  if (code == XHCI_CODE_SUCCESS)
    status = USBD_NORMAL;
  else if (code == XHCI_CODE_SHORT_PKT)
    status = USBD_SHORT;
  else if (code == XHCI_CODE_STALL)
    status = USBD_STALLED;
  else {
    status = USBD_IOERROR;
    printf("%s: slot %d endpoint %d: completion code %d, port %d status 0x%x\n",
            sc->dev->name, s->id, dci, code, s->dev->rhport,
            rd4(sc->opr, XHCI_PORTSC(s->dev->rhport)));
  }

  if (p->isoc) {
    if (code == XHCI_CODE_RING_OVERRUN || code == XHCI_CODE_RING_UNDERRUN)
      return;
    {
      struct xhci_ring *r = &s->ring[dci];
      uint64_t data;
      if (e->params >= r->phys && e->params < r->phys + (uint64_t)XHCI_RING_TRBS * XHCI_TRB_SIZE) {
        data = r->trb[(e->params - r->phys) / XHCI_TRB_SIZE].params;
        if (data >= p->buf_phys && data < p->buf_phys + (uint64_t)p->nseg * USB_ISOC_STRIDE)
          p->seg_next = (int)((data - p->buf_phys) / USB_ISOC_STRIDE);
      }
    }
    p->seg = p->seg_next;
    p->seg_next = (p->seg_next + 1) % p->nseg;
    if (p->done && !usbd_polling)
      p->done(p, p->arg, status, len);
    if (p->repeat) {
      isoc_push(&s->ring[dci], p, p->seg);
      doorbell(sc, (uint32_t)s->id, dci);
    }
    return;
  }

  if (p->done == 0) {
    if (!ring_covers(&s->ring[dci], s->final_lo[dci], s->span_hi[dci],
                     e->params) &&
        status != USBD_IOERROR && status != USBD_STALLED)
      return;
    p->sync_status = status;
    p->sync_len = len;
    p->sync_done = 1;
    return;
  }

  if (!usbd_polling || p->polled)
    p->done(p, p->arg, status, len);

  if (p->repeat && status != USBD_STALLED) {
    ring_push(&s->ring[dci], p->buf_phys,
              XHCI_TRB_LEN(p->xfer_len) | XHCI_TRB_INTR(0),
              XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP);
    doorbell(sc, (uint32_t)s->id, dci);
  }
}

// call with the lock held
static void events_drain(struct xhci_softc *sc) {
  struct xhci_trb e;

  for (;;) {
    struct xhci_trb *t = &sc->evt[sc->evt_idx];

    if (((t->control & XHCI_TRB_CYCLE) != 0) != (sc->evt_cycle != 0))
      break;
    mb();
    e = *t;
    sc->evt_idx++;
    if (sc->evt_idx == XHCI_RING_TRBS) {
      sc->evt_idx = 0;
      sc->evt_cycle ^= 1;
    }
    erdp_write(sc);

    switch (XHCI_TRB_GET_TYPE(e.control)) {
    case XHCI_EVT_CMD_COMPLETE:
      if (e.params == sc->cmd_at) {
        sc->cmd_evt = e;
        sc->cmd_done = 1;
      }
      break;
    case XHCI_EVT_TRANSFER:
      transfer_event(sc, &e);
      break;
    case XHCI_EVT_PORT_CHANGE:
      sc->port_changed = 1;
      break;
    default:
      break;
    }
  }
}

static int wait_flag(struct xhci_softc *sc, volatile int *flag, int ms) {
  uint64_t deadline = ticks + (uint64_t)ms * HZ / 1000 + 1;
  int i;

  if (curproc == 0 || (readeflags() & FL_IF) == 0) {
    for (i = 0; i < ms * 10; i++) {
      mtx_enter(&sc->lock);
      events_drain(sc);
      if (*flag) {
        mtx_leave(&sc->lock);
        return 0;
      }
      mtx_leave(&sc->lock);
      delay(100);
    }
    return -1;
  }
  mtx_enter(&sc->lock);
  for (;;) {
    events_drain(sc);
    if (*flag) {
      mtx_leave(&sc->lock);
      return 0;
    }
    if (ticks >= deadline) {
      mtx_leave(&sc->lock);
      return -1;
    }
    msleep(&sc->evt_idx, &sc->lock, PRIBIO, "xhci", (int)(deadline - ticks));
  }
}

static int command(struct xhci_softc *sc, uint64_t params, uint32_t status,
                   uint32_t control) {
  uint32_t code;
  int r, locked = curproc != 0 && (readeflags() & FL_IF) != 0;

  if (locked)
    acquiresleep(&sc->cmdlock);
  mtx_enter(&sc->lock);
  sc->cmd_done = 0;
  sc->cmd_at = ring_push(&sc->cmd, params, status, control);
  doorbell(sc, 0, 0);
  mtx_leave(&sc->lock);

  if (wait_flag(sc, &sc->cmd_done, CMD_TIMEOUT_MS) < 0)
    r = -1;
  else {
    code = XHCI_TRB_GET_CODE(sc->cmd_evt.status);
    r = code == XHCI_CODE_SUCCESS ? 0 : (int)code;
  }
  if (locked)
    releasesleep(&sc->cmdlock);
  return r;
}

// CONTEXTS

static uint32_t *slot_ctx(struct xhci_softc *sc, uint8_t *base, int input) {
  return (uint32_t *)(base + (input ? sc->ctxsize : 0));
}

static uint32_t *ep_ctx(struct xhci_softc *sc, uint8_t *base, int dci,
                        int input) {
  return (uint32_t *)(base + (uint32_t)(dci + (input ? 1 : 0)) * sc->ctxsize);
}

static uint32_t psi_of(int speed) {
  switch (speed) {
  case USB_SPEED_FULL:
    return 1;
  case USB_SPEED_LOW:
    return 2;
  case USB_SPEED_HIGH:
    return 3;
  default:
    return 4;
  }
}

static int speed_of_psi(uint32_t psi) {
  switch (psi) {
  case 1:
    return USB_SPEED_FULL;
  case 2:
    return USB_SPEED_LOW;
  case 3:
    return USB_SPEED_HIGH;
  case 4:
  case 5:
    return USB_SPEED_SUPER;
  default:
    return 0;
  }
}

static int dci_of(uint8_t epaddr) {
  int n = UE_GET_ADDR(epaddr);

  if (n == 0)
    return 1;
  return (n * 2) + (UE_GET_DIR(epaddr) ? 1 : 0);
}

static uint32_t eptype_of(uint8_t epaddr, uint8_t attr) {
  int in = UE_GET_DIR(epaddr) != 0;

  switch (UE_GET_XFERTYPE(attr)) {
  case UE_CONTROL:
    return 4;
  case UE_ISOCHRONOUS:
    return in ? 5 : 1;
  case UE_BULK:
    return in ? 6 : 2;
  default:
    return in ? 7 : 3;
  }
}

// bInterval to log2 microframes
static uint32_t interval_of(int speed, uint8_t attr, int binterval) {
  int i;

  if (UE_GET_XFERTYPE(attr) == UE_BULK || UE_GET_XFERTYPE(attr) == UE_CONTROL)
    return 0;
  if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER) {
    if (binterval < 1)
      binterval = 1;
    if (binterval > 16)
      binterval = 16;
    return (uint32_t)(binterval - 1);
  }
  if (binterval < 1)
    binterval = 1;
  for (i = 3; i < 15; i++)
    if ((1 << i) >= binterval * 8)
      return (uint32_t)i;
  return 10;
}

static void fill_slot_ctx(struct xhci_softc *sc __unused, struct xhci_slot *s,
                          uint32_t *slot) {
  struct usb_device *dev = s->dev;

  slot[0] = (dev->route & 0xfffff) | (psi_of(dev->speed) << 20) |
            ((uint32_t)s->maxdci << 27);
  slot[1] = (uint32_t)dev->rhport << 16;
  slot[2] = 0;
  slot[3] = 0;

  if (dev->nports > 0) {
    slot[0] |= 1u << 26;
    slot[1] |= (uint32_t)(dev->nports & 0xff) << 24;
  }
  if (dev->hs_hub != 0 && dev->hs_hub->hcpriv != 0) {
    struct xhci_slot *hs = dev->hs_hub->hcpriv;

    slot[2] = ((uint32_t)hs->id & 0xff) | (((uint32_t)dev->hs_port & 0xff) << 8);
  }
}

// SLOTS

static void slot_release(struct xhci_softc *sc, struct xhci_slot *s) {
  int i;

  mtx_enter(&sc->lock);
  if (s->id > 0 && (uint32_t)s->id <= sc->maxslots && sc->slot[s->id] == s)
    sc->slot[s->id] = 0;
  mtx_leave(&sc->lock);

  for (i = 0; i < DCI_MAX; i++) {
    ring_free(&s->ring[i]);
    if (s->pipe[i] && s->pipe[i]->buf) {
      if (s->pipe[i]->buflen > PGSIZE)
        dma_free(s->pipe[i]->buf);
      else
        page_free((char *)s->pipe[i]->buf);
      s->pipe[i]->buf = 0;
      s->pipe[i]->open = 0;
    }
    s->pipe[i] = 0;
    s->trb_at[i] = 0;
  }
  if (s->ctlbuf) {
    page_free((char *)s->ctlbuf);
    s->ctlbuf = 0;
  }
  if (s->devctx) {
    page_free((char *)s->devctx);
    s->devctx = 0;
  }
  if (s->inctx) {
    page_free((char *)s->inctx);
    s->inctx = 0;
  }
  if (s->id > 0)
    sc->dcbaa[s->id] = 0;
  free(s, M_DEVBUF);
}

// BUS METHODS

static int xhci_new_device(struct usb_device *dev) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s;
  uint32_t *in, *slot, *ep, id;

  if (command(sc, 0, 0, XHCI_TRB_TYPE(XHCI_CMD_ENABLE_SLOT)) != 0)
    return -1;
  id = XHCI_TRB_GET_SLOT(sc->cmd_evt.control);
  if (id == 0 || id > sc->maxslots)
    return -1;
  s = malloc(sizeof(*s), M_DEVBUF, M_WAITOK | M_ZERO);
  s->id = (int)id;
  s->dev = dev;
  mtx_enter(&sc->lock);
  sc->slot[id] = s;
  mtx_leave(&sc->lock);
  s->maxdci = 1;
  dev->hcpriv = s;

  s->devctx = dma_page(sc, 0);
  s->inctx = dma_page(sc, &s->inctx_phys);
  s->ctlbuf = dma_page(sc, &s->ctlbuf_phys);
  if (s->devctx == 0 || s->inctx == 0 || s->ctlbuf == 0 ||
      ring_init(sc, &s->ring[1]) < 0) {
    slot_release(sc, s);
    dev->hcpriv = 0;
    return -1;
  }
  sc->dcbaa[s->id] = (uint64_t)V2P(s->devctx);

  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[1] = 0x3;
  slot = slot_ctx(sc, s->inctx, 1);
  fill_slot_ctx(sc, s, slot);
  ep = ep_ctx(sc, s->inctx, 1, 1);
  ep[1] = (3u << 1) | (4u << 3) | ((uint32_t)dev->maxp0 << 16);
  ep[2] = (uint32_t)(s->ring[1].phys | 1);
  ep[3] = (uint32_t)(s->ring[1].phys >> 32);
  ep[4] = 8;

  if (command(sc, s->inctx_phys, 0,
              XHCI_TRB_TYPE(XHCI_CMD_ADDRESS_DEVICE) | XHCI_TRB_SLOT(s->id)) !=
      0) {
    command(sc, 0, 0,
            XHCI_TRB_TYPE(XHCI_CMD_DISABLE_SLOT) | XHCI_TRB_SLOT(s->id));
    slot_release(sc, s);
    dev->hcpriv = 0;
    return -1;
  }

  dev->address = (uint8_t)(slot_ctx(sc, s->devctx, 0)[3] & 0xff);
  return 0;
}

static int root_port_reset(struct xhci_softc *sc, int port);
static int root_port_rereset(struct xhci_softc *sc, int port);
static void ep_fill(struct xhci_softc *sc, struct xhci_slot *s,
                    struct usb_pipe *pipe, int dci, uint64_t deq, int dcs);

static int xhci_root_reset(struct usb_bus *bus, int port) {
  return root_port_rereset(bus->hcpriv, port);
}

// slot lost: another one, contexts and rings kept
static int slot_reenable(struct xhci_softc *sc, struct xhci_slot *s) {
  uint32_t id;
  int old = s->id;

  if (command(sc, 0, 0, XHCI_TRB_TYPE(XHCI_CMD_ENABLE_SLOT)) != 0)
    return -1;
  id = XHCI_TRB_GET_SLOT(sc->cmd_evt.control);
  if (id == 0 || id > sc->maxslots)
    return -1;
  mtx_enter(&sc->lock);
  if (sc->slot[old] == s)
    sc->slot[old] = 0;
  sc->dcbaa[old] = 0;
  s->id = (int)id;
  sc->slot[id] = s;
  mtx_leave(&sc->lock);
  memset(s->devctx, 0, PGSIZE);
  sc->dcbaa[id] = (uint64_t)V2P(s->devctx);
  printf("%s: slot %d was lost, the device is slot %u now\n", sc->dev->name, old, id);
  return 0;
}

// Reset Device, Address Device, pipes added back at their rings (4.6.11)
// 4.6.11 это ебаный лабиринт
static int xhci_readdress(struct usb_device *dev) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  struct xhci_ring *r;
  uint32_t *in, *ep;
  int dci, any = 0, rc, tries;

  if (s == 0 || s->gone)
    return -1;
  rc = command(sc, 0, 0, XHCI_TRB_TYPE(XHCI_CMD_RESET_DEVICE) | XHCI_TRB_SLOT(s->id));
  if (rc == XHCI_CODE_SLOT_NOT_ENABLED) {
    if (slot_reenable(sc, s) < 0)
      return -1;
  } else if (rc != 0 && rc != XHCI_CODE_CONTEXT_STATE) {
    printf("%s: slot %d: Reset Device answered %d\n", sc->dev->name, s->id, rc);
    return -1;
  }

  r = &s->ring[1];
  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[1] = 0x3;
  fill_slot_ctx(sc, s, slot_ctx(sc, s->inctx, 1));
  ep = ep_ctx(sc, s->inctx, 1, 1);
  ep[1] = (3u << 1) | (4u << 3) | ((uint32_t)dev->maxp0 << 16);
  ep[2] = (uint32_t)((r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE) |
                     (r->cycle ? 1 : 0));
  ep[3] = (uint32_t)((r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE) >> 32);
  ep[4] = 8;
  for (tries = 0;; tries++) {
    rc = command(sc, s->inctx_phys, 0,
                 XHCI_TRB_TYPE(XHCI_CMD_ADDRESS_DEVICE) | XHCI_TRB_SLOT(s->id));
    if (rc == 0)
      break;
    if (rc != XHCI_CODE_SLOT_NOT_ENABLED || tries > 0 || slot_reenable(sc, s) < 0) {
      printf("%s: slot %d: Address Device answered %d\n", sc->dev->name, s->id, rc);
      return -1;
    }
  }
  dev->address = (uint8_t)(slot_ctx(sc, s->devctx, 0)[3] & 0xff);

  memset(s->inctx, 0, PGSIZE);
  in[1] = 0x1;
  fill_slot_ctx(sc, s, slot_ctx(sc, s->inctx, 1));
  for (dci = 2; dci < DCI_MAX; dci++) {
    if (s->pipe[dci] == 0 || s->ring[dci].trb == 0)
      continue;
    r = &s->ring[dci];
    in[1] |= 1u << dci;
    ep_fill(sc, s, s->pipe[dci], dci,
            r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE, r->cycle);
    any = 1;
  }
  if (any && (rc = command(sc, s->inctx_phys, 0,
                           XHCI_TRB_TYPE(XHCI_CMD_CONFIGURE_EP) | XHCI_TRB_SLOT(s->id))) != 0) {
    printf("%s: slot %d: Configure Endpoint answered %d\n", sc->dev->name, s->id, rc);
    return -1;
  }
  return 0;
}

static void xhci_abort_device(struct usb_device *dev) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  struct usb_pipe *p;
  int dci;

  mtx_enter(&sc->lock);
  s->gone = 1;
  s->ctl_done = 1;
  for (dci = 2; dci < DCI_MAX; dci++)
    if ((p = s->pipe[dci]) != 0 && p->done == 0 && !p->sync_done) {
      p->sync_status = USBD_CANCELLED;
      p->sync_len = 0;
      p->sync_done = 1;
    }
  wakeup(&sc->evt_idx);
  mtx_leave(&sc->lock);
}

static void xhci_free_device(struct usb_device *dev) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;

  if (s == 0)
    return;
  if (!sc->fresh)
    command(sc, 0, 0,
            XHCI_TRB_TYPE(XHCI_CMD_DISABLE_SLOT) | XHCI_TRB_SLOT(s->id));
  slot_release(sc, s);
  dev->hcpriv = 0;
}

static int xhci_set_maxp0(struct usb_device *dev) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  uint32_t *in, *ep;

  if (s == 0)
    return -1;
  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[1] = 0x2;
  ep = ep_ctx(sc, s->inctx, 1, 1);
  memmove(ep, ep_ctx(sc, s->devctx, 1, 0), sc->ctxsize);
  ep[0] &= ~7u;
  ep[1] = (ep[1] & 0xffffu) | ((uint32_t)dev->maxp0 << 16);
  return command(sc, s->inctx_phys, 0,
                 XHCI_TRB_TYPE(XHCI_CMD_EVALUATE_CTX) | XHCI_TRB_SLOT(s->id)) ==
                 0
             ? 0
             : -1;
}

static int xhci_set_hub(struct usb_device *dev, int nports, int tt_think) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  uint32_t *in, *slot;

  if (s == 0)
    return -1;
  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[1] = 0x1;
  slot = slot_ctx(sc, s->inctx, 1);
  fill_slot_ctx(sc, s, slot);
  slot[0] |= 1u << 26;
  slot[1] |= (uint32_t)(nports & 0xff) << 24;
  if (dev->speed == USB_SPEED_HIGH)
    slot[2] |= (uint32_t)(tt_think & 3) << 16;
  return command(sc, s->inctx_phys, 0,
                 XHCI_TRB_TYPE(XHCI_CMD_EVALUATE_CTX) | XHCI_TRB_SLOT(s->id)) ==
                 0
             ? 0
             : -1;
}

// endpoint context: ring dequeued at deq, cycle bit dcs
static void ep_fill(struct xhci_softc *sc, struct xhci_slot *s,
                    struct usb_pipe *pipe, int dci, uint64_t deq, int dcs) {
  struct usb_device *dev = pipe->dev;
  uint32_t *ep = ep_ctx(sc, s->inctx, dci, 1);
  uint32_t esit;

  ep[0] = interval_of(dev->speed, pipe->edesc.bmAttributes,
                      pipe->edesc.bInterval)
          << 16;
  // 4.10.2.4: no retries on isochronous
  ep[1] = (pipe->isoc ? 0u : (3u << 1)) |
          (eptype_of(pipe->edesc.bEndpointAddress, pipe->edesc.bmAttributes)
           << 3) |
          ((uint32_t)pipe->maxp << 16) | ((uint32_t)pipe->burst << 8);
  ep[2] = (uint32_t)(deq | (dcs ? 1 : 0));
  ep[3] = (uint32_t)(deq >> 32);
  if (UE_GET_XFERTYPE(pipe->edesc.bmAttributes) == UE_BULK)
    ep[4] = PGSIZE;
  else {
    esit = pipe->bpi ? pipe->bpi
                     : (uint32_t)pipe->maxp * ((uint32_t)pipe->burst + 1);
    ep[4] = (esit < PGSIZE ? esit : PGSIZE) | (esit << 16);
  }
}

static int xhci_open_pipe(struct usb_pipe *pipe) {
  struct usb_device *dev = pipe->dev;
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  uint32_t *in, *slot;
  int dci = dci_of(pipe->edesc.bEndpointAddress);
  uint64_t phys;

  if (s == 0 || dci <= 1 || dci >= DCI_MAX)
    return -1;
  if (ring_init(sc, &s->ring[dci]) < 0)
    return -1;
  if (pipe->isoc || UE_GET_XFERTYPE(pipe->edesc.bmAttributes) == UE_BULK) {
    int bytes = pipe->isoc ? USB_ISOC_SEGS * USB_ISOC_STRIDE : USB_XFER_MAX;

    pipe->buf = dma_alloc((uint64_t)bytes);
    if (pipe->buf == 0)
      return -1;
    phys = (uint64_t)V2P(pipe->buf);
    memset(pipe->buf, 0, (uint64_t)bytes);
    pipe->buflen = bytes;
  } else {
    pipe->buf = dma_page(sc, &phys);
    if (pipe->buf == 0)
      return -1;
    pipe->buflen = PGSIZE;
  }
  pipe->buf_phys = phys;
  pipe->hcpriv = &s->ring[dci];

  if (dci > s->maxdci)
    s->maxdci = dci;

  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[1] = 0x1 | (1u << dci);
  slot = slot_ctx(sc, s->inctx, 1);
  fill_slot_ctx(sc, s, slot);

  ep_fill(sc, s, pipe, dci, s->ring[dci].phys, 1);

  if (command(sc, s->inctx_phys, 0,
              XHCI_TRB_TYPE(XHCI_CMD_CONFIGURE_EP) | XHCI_TRB_SLOT(s->id)) !=
      0) {
    if (pipe->buflen > PGSIZE)
      dma_free(pipe->buf);
    else
      page_free((char *)pipe->buf);
    pipe->buf = 0;
    return -1;
  }
  s->pipe[dci] = pipe;
  return 0;
}

static void xhci_close_pipe(struct usb_pipe *pipe) {
  struct usb_device *dev = pipe->dev;
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  uint32_t *in, *slot;
  int dci = dci_of(pipe->edesc.bEndpointAddress);

  if (s == 0 || dci <= 1 || dci >= DCI_MAX)
    return;
  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[0] = 1u << dci;
  in[1] = 0x1;
  slot = slot_ctx(sc, s->inctx, 1);
  fill_slot_ctx(sc, s, slot);
  if (!sc->fresh)
    command(sc, s->inctx_phys, 0,
            XHCI_TRB_TYPE(XHCI_CMD_CONFIGURE_EP) | XHCI_TRB_SLOT(s->id));

  s->pipe[dci] = 0;
  ring_free(&s->ring[dci]);
  if (pipe->buf) {
    if (pipe->buflen > PGSIZE)
      dma_free(pipe->buf);
    else
      page_free((char *)pipe->buf);
    pipe->buf = 0;
  }
}

static void ep_recover(struct xhci_softc *sc, struct xhci_slot *s, int dci) {
  struct xhci_ring *r = &s->ring[dci];
  uint64_t at;

  command(sc, 0, 0,
          XHCI_TRB_TYPE(XHCI_CMD_RESET_EP) | XHCI_TRB_EP((uint32_t)dci) |
              XHCI_TRB_SLOT(s->id));
  at = r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE;
  command(sc, at | (r->cycle ? 1 : 0), 0,
          XHCI_TRB_TYPE(XHCI_CMD_SET_TR_DEQUEUE) | XHCI_TRB_EP((uint32_t)dci) |
              XHCI_TRB_SLOT(s->id));
}

// drop and add the endpoint: the only way to zero the toggle (4.6.8)
static void xhci_clear_toggle(struct usb_pipe *pipe) {
  struct usb_device *dev = pipe->dev;
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  int dci = dci_of(pipe->edesc.bEndpointAddress);
  struct xhci_ring *r;
  uint32_t *in, state;

  if (s == 0 || s->gone || dci <= 1 || dci >= DCI_MAX ||
      s->ring[dci].trb == 0 || pipe->isoc ||
      UE_GET_XFERTYPE(pipe->edesc.bmAttributes) == UE_CONTROL)
    return;
  r = &s->ring[dci];
  state = ep_ctx(sc, s->devctx, dci, 0)[0] & 7;
  if (state == XHCI_EPSTATE_HALTED)
    command(sc, 0, 0,
            XHCI_TRB_TYPE(XHCI_CMD_RESET_EP) | XHCI_TRB_EP((uint32_t)dci) |
                XHCI_TRB_SLOT(s->id));
  else if (state == XHCI_EPSTATE_RUNNING)
    command(sc, 0, 0,
            XHCI_TRB_TYPE(XHCI_CMD_STOP_EP) | XHCI_TRB_EP((uint32_t)dci) |
                XHCI_TRB_SLOT(s->id));

  memset(s->inctx, 0, PGSIZE);
  in = (uint32_t *)s->inctx;
  in[0] = 1u << dci;
  in[1] = 0x1;
  fill_slot_ctx(sc, s, slot_ctx(sc, s->inctx, 1));
  command(sc, s->inctx_phys, 0,
          XHCI_TRB_TYPE(XHCI_CMD_CONFIGURE_EP) | XHCI_TRB_SLOT(s->id));

  memset(s->inctx, 0, PGSIZE);
  in[1] = 0x1 | (1u << dci);
  fill_slot_ctx(sc, s, slot_ctx(sc, s->inctx, 1));
  ep_fill(sc, s, pipe, dci, r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE,
          r->cycle);
  if (command(sc, s->inctx_phys, 0,
              XHCI_TRB_TYPE(XHCI_CMD_CONFIGURE_EP) | XHCI_TRB_SLOT(s->id)) != 0)
    printf("%s: endpoint %d would not come back\n", sc->dev->name, dci);
}

static void ep_stop(struct xhci_softc *sc, struct xhci_slot *s, int dci) {
  struct xhci_ring *r = &s->ring[dci];
  uint64_t at;

  command(sc, 0, 0,
          XHCI_TRB_TYPE(XHCI_CMD_STOP_EP) | XHCI_TRB_EP((uint32_t)dci) |
              XHCI_TRB_SLOT(s->id));
  at = r->phys + (uint64_t)r->idx * XHCI_TRB_SIZE;
  command(sc, at | (r->cycle ? 1 : 0), 0,
          XHCI_TRB_TYPE(XHCI_CMD_SET_TR_DEQUEUE) | XHCI_TRB_EP((uint32_t)dci) |
              XHCI_TRB_SLOT(s->id));
}

static int xhci_control(struct usb_device *dev,
                        const struct usb_device_request *req, void *data,
                        int *len) {
  struct xhci_softc *sc = dev->bus->hcpriv;
  struct xhci_slot *s = dev->hcpriv;
  uint16_t n = req->wLength;
  int in = (req->bmRequestType & UT_READ) != 0;
  uint64_t setup;

  if (s == 0 || s->gone || n > PGSIZE)
    return -1;
  if (n != 0 && !in && data != 0)
    memmove(s->ctlbuf, data, n);
  if (n != 0 && in)
    memset(s->ctlbuf, 0, n);

  setup = (uint64_t)req->bmRequestType | ((uint64_t)req->bRequest << 8) |
          ((uint64_t)req->wValue << 16) | ((uint64_t)req->wIndex << 32) |
          ((uint64_t)req->wLength << 48);

  mtx_enter(&sc->lock);
  if (s->gone) {
    mtx_leave(&sc->lock);
    return -1;
  }
  s->ctl_done = 0;
  ring_push(&s->ring[1], setup, XHCI_TRB_LEN(8) | XHCI_TRB_INTR(0),
            XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT |
                (n ? (in ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT) : 0));
  if (n)
    ring_push(&s->ring[1], s->ctlbuf_phys, XHCI_TRB_LEN(n) | XHCI_TRB_INTR(0),
              XHCI_TRB_TYPE(XHCI_TRB_DATA) | XHCI_TRB_ISP |
                  (in ? XHCI_TRB_DIR_IN : 0));
  s->trb_at[1] = ring_push(&s->ring[1], 0, XHCI_TRB_INTR(0),
                           XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC |
                               (in ? 0 : XHCI_TRB_DIR_IN));
  doorbell(sc, (uint32_t)s->id, 1);
  mtx_leave(&sc->lock);

  if (wait_flag(sc, &s->ctl_done, XFER_TIMEOUT_MS) < 0) {
    if (!s->gone)
      ep_stop(sc, s, 1);
    return -1;
  }
  if (s->gone)
    return -1;
  if (s->ctl_status != XHCI_CODE_SUCCESS &&
      s->ctl_status != XHCI_CODE_SHORT_PKT) {
    ep_recover(sc, s, 1);
    return -1;
  }
  if (n != 0 && in && data != 0)
    memmove(data, s->ctlbuf, n);
  if (len)
    *len = n;
  return 0;
}

static int xhci_start(struct usb_pipe *pipe) {
  struct xhci_softc *sc = pipe->dev->bus->hcpriv;
  struct xhci_slot *s = pipe->dev->hcpriv;
  int dci = dci_of(pipe->edesc.bEndpointAddress);

  if (s == 0 || dci <= 1 || dci >= DCI_MAX || s->ring[dci].trb == 0)
    return -1;
  mtx_enter(&sc->lock);
  ring_push(&s->ring[dci], pipe->buf_phys,
            XHCI_TRB_LEN(pipe->xfer_len) | XHCI_TRB_INTR(0),
            XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP);
  doorbell(sc, (uint32_t)s->id, (uint32_t)dci);
  mtx_leave(&sc->lock);
  return 0;
}

static int xhci_isoc_start(struct usb_pipe *pipe) {
  struct xhci_softc *sc = pipe->dev->bus->hcpriv;
  struct xhci_slot *s = pipe->dev->hcpriv;
  int dci = dci_of(pipe->edesc.bEndpointAddress);
  int i;

  if (s == 0 || dci <= 1 || dci >= DCI_MAX || s->ring[dci].trb == 0)
    return -1;
  mtx_enter(&sc->lock);
  for (i = 0; i < pipe->nseg; i++)
    isoc_push(&s->ring[dci], pipe, i);
  doorbell(sc, (uint32_t)s->id, (uint32_t)dci);
  mtx_leave(&sc->lock);
  return 0;
}

static void controller_check(struct xhci_softc *sc) {
  uint32_t sts = rd4(sc->opr, XHCI_USBSTS);

  if ((sts & (XHCI_STS_HSE | XHCI_STS_HCE | XHCI_STS_HCH)) == 0 || sc->dead)
    return;
  sc->dead = 1;
  printf("%s: controller %s, status 0x%x\n", sc->dev->name, 
          (sts & XHCI_STS_HCE) ? "error" : (sts & XHCI_STS_HSE) ? "host system error" : "halted",
          sts);
  if (sts & XHCI_STS_HSE)
    wr4(sc->opr, XHCI_USBSTS, XHCI_STS_HSE);
}

static void events_service(struct xhci_softc *sc) {
  controller_check(sc);
  mtx_enter(&sc->lock);
  events_drain(sc);
  wakeup(&sc->evt_idx);
  mtx_leave(&sc->lock);
  if (sc->port_changed) {
    sc->port_changed = 0;
    usbd_needs_explore(&sc->bus);
  }
}

static int xhci_transfer_n(struct usb_pipe *pipe, const int *lens, int nseg,
                           int ms) {
  struct xhci_softc *sc = pipe->dev->bus->hcpriv;
  struct xhci_slot *s = pipe->dev->hcpriv;
  int dci = dci_of(pipe->edesc.bEndpointAddress);
  struct xhci_ring *r;
  uint64_t phys;
  int in, i, total = 0;

  if (s == 0 || s->gone || dci <= 1 || dci >= DCI_MAX || s->ring[dci].trb == 0)
    return -1;
  for (i = 0; i < nseg; i++)
    total += lens[i];
  if (nseg < 1 || total > pipe->buflen)
    return -1;

  r = &s->ring[dci];
  in = UE_GET_DIR(pipe->edesc.bEndpointAddress) != 0;
  mtx_enter(&sc->lock);
  if (s->gone) {
    mtx_leave(&sc->lock);
    return -1;
  }
  pipe->sync_done = 0;
  pipe->xfer_len = total;
  s->span_lo[dci] = r->idx;
  phys = pipe->buf_phys;
  for (i = 0; i < nseg; i++) {
    if (i == nseg - 1)
      s->final_lo[dci] = r->idx;
    s->trb_at[dci] = push_td(r, phys, (uint32_t)lens[i], pipe->maxp, in);
    phys += (uint64_t)lens[i];
  }
  s->span_hi[dci] = r->idx == 0 ? XHCI_RING_TRBS - 1 : r->idx - 1;
  doorbell(sc, (uint32_t)s->id, (uint32_t)dci);
  mtx_leave(&sc->lock);

  if (wait_flag(sc, &pipe->sync_done, ms > 0 ? ms : XFER_TIMEOUT_MS) < 0) {
    if (s->gone)
      return -1;
    printf("%s: endpoint %d timed out after %d bytes, state %d\n", sc->dev->name,
            dci, total, ep_ctx(sc, s->devctx, dci, 0)[0] & 7);
    ep_stop(sc, s, dci);
    pipe->sync_status = USBD_TIMEOUT;
    pipe->sync_len = 0;
    return -1;
  }
  if (s->gone)
    return -1;
  if (pipe->sync_status != USBD_NORMAL && pipe->sync_status != USBD_SHORT) {
    ep_recover(sc, s, dci);
    return -1;
  }
  return 0;
}

static int xhci_transfer(struct usb_pipe *pipe, int len, int ms) {
  return xhci_transfer_n(pipe, &len, 1, ms);
}


// at panic: the event ring, if nobody holds the hc
static void xhci_poll(struct usb_bus *bus) {
  struct xhci_softc *sc = bus->hcpriv;

  if (sc == 0 || sc->dead || !mtx_enter_try(&sc->lock))
    return;
  events_drain(sc);
  mtx_leave(&sc->lock);
}

static void xhci_bus_suspend(struct usb_bus *bus);
static int xhci_bus_resume(struct usb_bus *bus, int lost);
static void xhci_bus_restart(struct usb_bus *bus);

static const struct usb_bus_methods xhci_bus_methods = {
    .new_device = xhci_new_device,
    .free_device = xhci_free_device,
    .open_pipe = xhci_open_pipe,
    .isoc_start = xhci_isoc_start,
    .close_pipe = xhci_close_pipe,
    .control = xhci_control,
    .start = xhci_start,
    .transfer = xhci_transfer,
    .clear_toggle = xhci_clear_toggle,
    .root_reset = xhci_root_reset,
    .readdress = xhci_readdress,
    .set_maxp0 = xhci_set_maxp0,
    .set_hub = xhci_set_hub,
    .abort_device = xhci_abort_device,
    .poll = xhci_poll,
    .suspend = xhci_bus_suspend,
    .resume = xhci_bus_resume,
    .restart = xhci_bus_restart,
};

// THE ROOT HUB

static int root_port_reset(struct xhci_softc *sc, int port) {
  uint32_t v = rd4(sc->opr, XHCI_PORTSC(port));
  int i;

  if ((v & XHCI_PS_PP) == 0) {
    wr4(sc->opr, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PP);
    usb_pause_ms(20);
    v = rd4(sc->opr, XHCI_PORTSC(port));
  }

  if ((v & XHCI_PS_PED) == 0) {
    wr4(sc->opr, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PR);
    for (i = 0; i < 500; i++) {
      v = rd4(sc->opr, XHCI_PORTSC(port));
      if (v & XHCI_PS_PRC)
        break;
      delay(1000);
    }
    wr4(sc->opr, XHCI_PORTSC(port),
        (v & ~XHCI_PS_CLEAR) | XHCI_PS_PRC | XHCI_PS_CSC);
    usb_pause_ms(20);
    v = rd4(sc->opr, XHCI_PORTSC(port));
  }

  return (v & (XHCI_PS_CCS | XHCI_PS_PED)) == (XHCI_PS_CCS | XHCI_PS_PED) ? 0
                                                                          : -1;
}

#define PS_CHANGES                                                             \
  (XHCI_PS_CSC | XHCI_PS_PEC | XHCI_PS_WRC | XHCI_PS_OCC | XHCI_PS_PRC |       \
   XHCI_PS_PLC | XHCI_PS_CEC)

static uint32_t root_port_warm_reset(struct xhci_softc *sc, int port, uint32_t v) {
  int i;

  printf("usb: root port %d link in state %d%s, warm reset\n", port,
          (int)XHCI_PS_PLS_GET(v), (v & XHCI_PS_CAS) ? " cold-attached" : "");
  wr4(sc->opr, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_WPR);
  for (i = 0; i < 500; i++) {
    v = rd4(sc->opr, XHCI_PORTSC(port));
    if (v & XHCI_PS_WRC)
      break;
    delay(1000);
  }
  wr4(sc->opr, XHCI_PORTSC(port),
      (v & ~XHCI_PS_CLEAR) | XHCI_PS_WRC | XHCI_PS_PRC | XHCI_PS_CSC);
  usb_pause_ms(20);
  return rd4(sc->opr, XHCI_PORTSC(port));
}

#define PORT_RESET_TRIES 5

// reset an enabled port too; warm if the link wants
static int root_port_rereset(struct xhci_softc *sc, int port) {
  uint32_t v = rd4(sc->opr, XHCI_PORTSC(port)), pls;
  int usb3 = sc->port_major[port - 1] >= 3, warm, i, tries;

  pls = XHCI_PS_PLS_GET(v);
  warm = usb3 && ((v & XHCI_PS_CAS) || pls == XHCI_PS_PLS_INACTIVE ||
                  pls == XHCI_PS_PLS_COMPLIANCE);
  for (tries = 0; tries < PORT_RESET_TRIES; tries++) {
    if (warm)
      v = root_port_warm_reset(sc, port, v);
    else {
      wr4(sc->opr, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PR);
      for (i = 0; i < 500; i++) {
        v = rd4(sc->opr, XHCI_PORTSC(port));
        if (v & XHCI_PS_PRC)
          break;
        delay(1000);
      }
      wr4(sc->opr, XHCI_PORTSC(port),
          (v & ~XHCI_PS_CLEAR) | XHCI_PS_PRC | XHCI_PS_CSC | XHCI_PS_PLC);
      usb_pause_ms(20);
      v = rd4(sc->opr, XHCI_PORTSC(port));
    }
    if ((v & (XHCI_PS_CCS | XHCI_PS_PED)) == (XHCI_PS_CCS | XHCI_PS_PED))
      return 0;
    if (usb3)
      warm = 1;
  }
  printf("%s: root port %d would not reset (0x%x)\n", sc->dev->name, port, v);
  return -1;
}

static int link_stuck(struct xhci_softc *sc, int port, uint32_t *vp) {
  uint32_t v = *vp, pls = XHCI_PS_PLS_GET(v);
  int i;

  if (sc->port_major[port - 1] < 3)
    return 0;
  if ((v & XHCI_PS_CCS) != 0 || (v & XHCI_PS_PP) == 0)
    return 0;
  if (pls == XHCI_PS_PLS_DISABLED) {
    wr4(sc->opr, XHCI_PORTSC(port),
        (v & ~XHCI_PS_CLEAR) | XHCI_PS_LWS |
            XHCI_PS_PLS_SET(XHCI_PS_PLS_RXDETECT));
    usb_pause_ms(10);
    v = rd4(sc->opr, XHCI_PORTSC(port));
    pls = XHCI_PS_PLS_GET(v);
  }
  if ((v & XHCI_PS_CAS) || pls == XHCI_PS_PLS_INACTIVE ||
      pls == XHCI_PS_PLS_COMPLIANCE || pls == XHCI_PS_PLS_LOOPBACK)
    return 1;
  if (pls != XHCI_PS_PLS_POLLING)
    return 0;
  for (i = 0; i < 20; i++) {
    usb_pause_ms(10);
    v = rd4(sc->opr, XHCI_PORTSC(port));
    if ((v & XHCI_PS_CCS) || XHCI_PS_PLS_GET(v) != XHCI_PS_PLS_POLLING)
      break;
  }
  *vp = v;
  return (v & XHCI_PS_CCS) == 0 && XHCI_PS_PLS_GET(v) == XHCI_PS_PLS_POLLING;
}

static int xhci_root_explore(struct usb_device *root) {
  struct xhci_softc *sc = root->bus->hcpriv;
  uint32_t port;

  for (port = 1; port <= sc->maxports; port++) {
    uint32_t v = rd4(sc->opr, XHCI_PORTSC(port));
    int connected;

    if (v == 0xffffffff)
      break;
    if (v & PS_CHANGES) {
      wr4(sc->opr, XHCI_PORTSC(port),
          (v & ~XHCI_PS_CLEAR) | (v & PS_CHANGES));
      v = rd4(sc->opr, XHCI_PORTSC(port));
    }
    if (link_stuck(sc, (int)port, &v))
      v = root_port_warm_reset(sc, (int)port, v);
    connected = (v & XHCI_PS_CCS) != 0;

    if (connected == sc->port_state[port - 1])
      continue;
    sc->port_state[port - 1] = (uint8_t)connected;

    if (!connected) {
      if (root->child[port - 1]) {
        printf("%s: device on root port %d unplugged, status 0x%x\n",
                sc->dev->name, port, v);
        usbd_free_device(root->child[port - 1]);
      }
      continue;
    }

    usb_pause_ms(USB_PORT_POWERUP_DELAY);
    v = rd4(sc->opr, XHCI_PORTSC(port));
    if ((v & XHCI_PS_CCS) == 0) {
      sc->port_state[port - 1] = 0;
      continue;
    }
    if (root_port_reset(sc, (int)port) < 0) {
      printf("usb: root port %d will not enable\n", port);
      sc->port_state[port - 1] = 0;
      continue;
    }
    v = rd4(sc->opr, XHCI_PORTSC(port));
    if (sc->port_major[port - 1] >= 3 &&
        speed_of_psi(XHCI_PS_SPEED(v)) == USB_SPEED_SUPER) {
      uint32_t pm = rd4(sc->opr, XHCI_PORTPMSC(port));

      wr4(sc->opr, XHCI_PORTPMSC(port), 0);
      if (pm != 0)
        printf("%s: root port %d slept on its own (0x%x), stopped\n",
                sc->dev->name, port, pm);
    }
    usbd_new_device(&sc->bus, root, 1, speed_of_psi(XHCI_PS_SPEED(v)),
                    (int)port, 0);
  }
  return 0;
}

// BRINGING IT UP

static void take_controller(struct xhci_softc *sc) {
  uint32_t cparams = rd4(sc->cap, XHCI_HCCPARAMS);
  uint32_t eec = 0xffffffff;
  uint32_t xecp;
  int i;

  if (cparams == 0xffffffff)
    return;

  for (xecp = XHCI_HCC_XECP(cparams) << 2; xecp != 0 && XHCI_XECP_NEXT(eec);
       xecp += XHCI_XECP_NEXT(eec) << 2) {
    eec = rd4(sc->cap, xecp);
    if (eec == 0xffffffff)
      return;
    if (XHCI_XECP_ID(eec) != XHCI_ID_USB_LEGACY)
      continue;
    if (rd1(sc->cap, xecp + XHCI_XECP_BIOS_SEM) == 0)
      continue;

    printf("%s: waiting for the firmware to give up the controller\n", sc->dev->name);
    wr1(sc->cap, xecp + XHCI_XECP_OS_SEM, 1);
    for (i = 0; i < 5000; i++) {
      if (rd1(sc->cap, xecp + XHCI_XECP_BIOS_SEM) == 0)
        break;
      delay(1000);
    }
    if (rd1(sc->cap, xecp + XHCI_XECP_BIOS_SEM) != 0)
      printf("%s: timed out waiting for the firmware\n", sc->dev->name);
  }
}

static int intel_shares_ports(const struct pci_device *pd) {
  static const uint16_t shared[] = {0x0f35, 0x9c31, 0x1e31,
                                    0x8c31, 0x8cb1, 0x9cb1};
  uint i;

  if (pd->vendor_id != PCI_VENDOR_INTEL)
    return 0;
  for (i = 0; i < sizeof(shared) / sizeof(shared[0]); i++)
    if (pd->device_id == shared[i])
      return 1;
  return 0;
}

static void intel_route_ports(struct xhci_softc *sc) {
  const struct pci_device *pd = &sc->dev->pci;
  uint32_t want, usb2_mask, usb3_mask;

  if (!intel_shares_ports(pd))
    return;

  want = pci_read32(pd->bus, pd->device, pd->function,
                    PCI_XHCI_INTEL_USB3_PSSEN) |
         pci_read32(pd->bus, pd->device, pd->function, PCI_XHCI_INTEL_XUSB2PR);
  usb3_mask =
      pci_read32(pd->bus, pd->device, pd->function, PCI_XHCI_INTEL_USB3PRM);
  usb2_mask =
      pci_read32(pd->bus, pd->device, pd->function, PCI_XHCI_INTEL_XUSB2PRM);
  if (usb2_mask == 0xffffffff || usb3_mask == 0xffffffff)
    return;

  pci_write32(pd->bus, pd->device, pd->function, PCI_XHCI_INTEL_USB3_PSSEN,
              want & usb3_mask);
  pci_write32(pd->bus, pd->device, pd->function, PCI_XHCI_INTEL_XUSB2PR,
              want & usb2_mask);
}

static void protocol_map(struct xhci_softc *sc) {
  uint32_t cparams = rd4(sc->cap, XHCI_HCCPARAMS);
  uint32_t eec = 0xffffffff;
  uint32_t xecp, w0, w4, w8;
  uint32_t first, count, i;

  if (cparams == 0xffffffff)
    return;
  for (xecp = XHCI_HCC_XECP(cparams) << 2; xecp != 0 && XHCI_XECP_NEXT(eec);
       xecp += XHCI_XECP_NEXT(eec) << 2) {
    eec = rd4(sc->cap, xecp);
    if (eec == 0xffffffff)
      return;
    if (XHCI_XECP_ID(eec) != XHCI_ID_PROTOCOLS)
      continue;
    w0 = eec;
    w4 = rd4(sc->cap, xecp + 4);
    w8 = rd4(sc->cap, xecp + 8);
    if (w4 != XHCI_XECP_USBID)
      continue;
    first = XHCI_XECP_SP_CPO(w8);
    count = XHCI_XECP_SP_CPC(w8);
    if (first == 0 || count == 0 || first + count - 1 > sc->maxports)
      continue;
    printf("%s: usb %d.0 on ports %d..%d\n", sc->dev->name, XHCI_XECP_SP_MAJOR(w0), first,
            first + count - 1);
    for (i = first; i < first + count; i++)
      sc->port_major[i - 1] = (uint8_t)XHCI_XECP_SP_MAJOR(w0);
  }
}

static int wait_bits(volatile uint8_t *p, uint32_t off, uint32_t mask,
                     uint32_t want, int ms) {
  int i;

  for (i = 0; i < ms * 10; i++) {
    if ((rd4(p, off) & mask) == want)
      return 0;
    delay(100);
  }
  return -1;
}

static int controller_reset(struct xhci_softc *sc) {
  if (wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_CNR, 0, 1000) < 0)
    return -1;

  wr4(sc->opr, XHCI_USBCMD, rd4(sc->opr, XHCI_USBCMD) & ~XHCI_CMD_RS);
  if (wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 1000) < 0)
    return -1;

  wr4(sc->opr, XHCI_USBCMD, XHCI_CMD_HCRST);
  delay(1000);
  if (wait_bits(sc->opr, XHCI_USBCMD, XHCI_CMD_HCRST, 0, 1000) < 0)
    return -1;
  if (wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_CNR, 0, 1000) < 0)
    return -1;
  return 0;
}

static int scratchpad_setup(struct xhci_softc *sc, uint32_t count) {
  uint64_t page;
  uint32_t i;

  if (count == 0)
    return 0;
  if (count * sizeof(uint64_t) <= PGSIZE)
    sc->scratch = dma_page(sc, 0);
  else if ((sc->scratch = dma_alloc(count * sizeof(uint64_t))) != 0) {
    sc->scratch_dma = 1;
    memset(sc->scratch, 0, count * sizeof(uint64_t));
  }
  if (sc->scratch == 0)
    return -1;
  for (i = 0; i < count; i++, sc->nscratch++) {
    if (dma_page(sc, &page) == 0)
      return -1;
    sc->scratch[i] = page;
  }
  sc->dcbaa[0] = (uint64_t)V2P(sc->scratch);
  return 0;
}

static void xhci_free(struct xhci_softc *sc) {
  uint32_t i;

  for (i = 0; i < sc->nscratch; i++)
    page_free((char *)P2V(sc->scratch[i]));
  if (sc->scratch != 0 && sc->scratch_dma)
    dma_free(sc->scratch);
  else if (sc->scratch != 0)
    page_free((char *)sc->scratch);
  ring_free(&sc->cmd);
  if (sc->dcbaa != 0)
    page_free((char *)sc->dcbaa);
  if (sc->evt != 0)
    page_free((char *)sc->evt);
  if (sc->erst != 0)
    page_free((char *)sc->erst);
  if (sc->slot != 0)
    free(sc->slot, M_DEVBUF);
  if (sc->port_major != 0)
    free(sc->port_major, M_DEVBUF);
  if (sc->port_state != 0)
    free(sc->port_state, M_DEVBUF);
  free(sc, M_DEVBUF);
}

static int xhci_run(struct xhci_softc *sc) {
  wr4(sc->opr, XHCI_USBSTS, rd4(sc->opr, XHCI_USBSTS));
  wr4(sc->opr, XHCI_DNCTRL, 0);
  wr4(sc->opr, XHCI_CONFIG, sc->maxslots);
  wr4(sc->opr, XHCI_DCBAAP_LO, (uint32_t)sc->dcbaa_phys);
  wr4(sc->opr, XHCI_DCBAAP_HI, (uint32_t)(sc->dcbaa_phys >> 32));
  wr4(sc->opr, XHCI_CRCR_LO, (uint32_t)sc->cmd.phys | XHCI_CRCR_LO_RCS);
  wr4(sc->opr, XHCI_CRCR_HI, (uint32_t)(sc->cmd.phys >> 32));

  sc->evt_idx = 0;
  sc->evt_cycle = 1;
  sc->erst[0].addr = sc->evt_phys;
  sc->erst[0].size = XHCI_RING_TRBS;
  wr4(sc->rts, XHCI_ERSTSZ(0), 1);
  erdp_write(sc);
  wr4(sc->rts, XHCI_ERSTBA_LO(0), (uint32_t)V2P(sc->erst));
  wr4(sc->rts, XHCI_ERSTBA_HI(0), (uint32_t)(V2P(sc->erst) >> 32));
  wr4(sc->rts, XHCI_IMOD(0), 0);
  wr4(sc->rts, XHCI_IMAN(0), XHCI_IMAN_INTR_ENA | XHCI_IMAN_INTR_PEND);

  wr4(sc->opr, XHCI_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE);
  return wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_HCH, 0, 1000);
}

static void ports_power(struct xhci_softc *sc) {
  uint32_t port, v;

  for (port = 1; port <= sc->maxports; port++) {
    v = rd4(sc->opr, XHCI_PORTSC(port));
    if (v != 0xffffffff && (v & XHCI_PS_PP) == 0)
      wr4(sc->opr, XHCI_PORTSC(port), (v & ~XHCI_PS_CLEAR) | XHCI_PS_PP);
  }
  usb_pause_ms(100);
}

// SLEEP
static void cmd_ring_clear(struct xhci_softc *sc) {
  memset(sc->cmd.trb, 0, PGSIZE);
  sc->cmd.idx = 0;
  sc->cmd.cycle = 1;
  sc->cmd.trb[XHCI_RING_TRBS - 1].params = sc->cmd.phys;
  sc->cmd.trb[XHCI_RING_TRBS - 1].control =
      XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC;
}

// stop each running endpoint, then the default one
static void slot_quiesce(struct xhci_softc *sc, struct xhci_slot *s) {
  int dci;

  for (dci = DCI_MAX - 1; dci > 1; dci--)
    if (s->ring[dci].trb != 0 &&
        (ep_ctx(sc, s->devctx, dci, 0)[0] & 7) == XHCI_EPSTATE_RUNNING)
      command(sc, 0, 0,
              XHCI_TRB_TYPE(XHCI_CMD_STOP_EP) | XHCI_TRB_SP |
                  XHCI_TRB_EP((uint32_t)dci) | XHCI_TRB_SLOT(s->id));
  command(sc, 0, 0,
          XHCI_TRB_TYPE(XHCI_CMD_STOP_EP) | XHCI_TRB_SP | XHCI_TRB_EP(1) |
              XHCI_TRB_SLOT(s->id));
}

static int live_slots(struct xhci_softc *sc, struct xhci_slot **out) {
  uint32_t id;
  int n = 0;

  mtx_enter(&sc->lock);
  for (id = 1; id <= sc->maxslots; id++)
    if (sc->slot[id] != 0 && !sc->slot[id]->gone && sc->slot[id]->devctx != 0)
      out[n++] = sc->slot[id];
  mtx_leave(&sc->lock);
  return n;
}

// U3, no wake, halt, CSS. rebuilt on the way back
static void xhci_bus_suspend(struct usb_bus *bus) {
  struct xhci_softc *sc = bus->hcpriv;
  struct xhci_slot **live;
  uint32_t port, v, mine[8] = {0};
  int i, n;

  live = malloc((sc->maxslots + 1) * sizeof(*live), M_DEVBUF, M_WAITOK | M_ZERO);
  n = live_slots(sc, live);
  for (i = 0; i < n; i++)
    slot_quiesce(sc, live[i]);
  free(live, M_DEVBUF);

  for (port = 1; port <= sc->maxports && port <= 8 * 32; port++) {
    v = rd4(sc->opr, XHCI_PORTSC(port));
    if (v == 0xffffffff)
      continue;
    if ((v & XHCI_PS_PED) && XHCI_PS_PLS_GET(v) == XHCI_PS_PLS_U0) {
      wr4(sc->opr, XHCI_PORTSC(port),
          (v & ~(XHCI_PS_CLEAR | XHCI_PS_WAKE)) | XHCI_PS_LWS |
              XHCI_PS_PLS_SET(XHCI_PS_PLS_U3));
      mine[(port - 1) / 32] |= 1u << ((port - 1) % 32);
    } else if (v & XHCI_PS_WAKE)
      wr4(sc->opr, XHCI_PORTSC(port), v & ~(XHCI_PS_CLEAR | XHCI_PS_WAKE));
  }
  for (port = 1; port <= sc->maxports && port <= 8 * 32; port++)
    if (((mine[(port - 1) / 32] >> ((port - 1) % 32)) & 1) &&
        wait_bits(sc->opr, XHCI_PORTSC(port), XHCI_PS_PLS_SET(0xf),
                  XHCI_PS_PLS_SET(XHCI_PS_PLS_U3), 10) < 0)
      printf("%s: port %u would not suspend (0x%x)\n", sc->dev->name, port,
             rd4(sc->opr, XHCI_PORTSC(port)));
  wr4(sc->opr, XHCI_USBCMD, rd4(sc->opr, XHCI_USBCMD) & ~XHCI_CMD_RS);
  if (wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100) < 0)
    return;
  cmd_ring_clear(sc);
  // intel won't power down without a save. why? ???
  wr4(sc->opr, XHCI_USBCMD, rd4(sc->opr, XHCI_USBCMD) | XHCI_CMD_CSS);
  wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_SSS, 0, 20);
}

static int xhci_suspend(struct device *dev) {
  struct xhci_softc *sc = dev->driver_data;

  wr4(sc->opr, XHCI_USBCMD, rd4(sc->opr, XHCI_USBCMD) & ~XHCI_CMD_RS);
  wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100);
  return 0;
}

static int xhci_resume(struct device *dev) {
  (void)dev;
  return 0;
}

// reset and initialised again
static int xhci_reinit(struct xhci_softc *sc) {
  uint64_t scratch;

  if (controller_reset(sc) < 0)
    return -1;
  scratch = sc->dcbaa[0];
  memset(sc->dcbaa, 0, PGSIZE);
  sc->dcbaa[0] = scratch;
  cmd_ring_clear(sc);
  memset(sc->evt, 0, PGSIZE);
  sc->dead = 0;
  sc->fresh = 1;
  if (xhci_run(sc) < 0)
    return -1;
  intel_route_ports(sc);
  ports_power(sc);
  memset(sc->port_state, 0, sc->maxports + 1);
  return 0;
}

// the EC may cut port power in S3: the device restarts, the xHC remembers a ghost
// so: reset it all, find the devices as new
static int xhci_bus_resume(struct usb_bus *bus, int lost) {
  struct xhci_softc *sc = bus->hcpriv;

  (void)lost;
  take_controller(sc);
  if (xhci_reinit(sc) < 0)
    printf("%s: the controller would not come back\n", sc->dev->name);
  return 1;
}

static void xhci_bus_restart(struct usb_bus *bus) {
  struct xhci_softc *sc = bus->hcpriv;

  sc->fresh = 0;
}

// DRIVER FRAMEWORK

static int xhci_probe(struct device *dev) {
  if (dev->pci.class_code != PCI_CLASS_SERIAL_BUS ||
      dev->pci.subclass != PCI_SUBCLASS_USB ||
      dev->pci.prog_if != PCI_PROGIF_XHCI)
    return 0;
  return 100;
}

static int xhci_attach(struct device *dev) {
  struct xhci_softc *sc;
  uint32_t hcs1, hcs2, hcc, page;
  uint64_t phys, erst;
  int running = 0;

  if (dev->mmio_vaddr == 0 || dev->mmio_size < 0x1000) {
    printf("%s: no register window\n", dev->name);
    return -1;
  }
  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->dev = dev;
  mtx_init(&sc->lock, "xhci");
  initsleeplock(&sc->cmdlock, "xhcicmd");
  sc->cap = (volatile uint8_t *)dev->mmio_vaddr;
  sc->opr = sc->cap + rd1(sc->cap, XHCI_CAPLENGTH);
  sc->rts = sc->cap + (rd4(sc->cap, XHCI_RTSOFF) & ~0x1fu);
  sc->dbl = sc->cap + (rd4(sc->cap, XHCI_DBOFF) & ~0x3u);

  pci_enable_bus_master(&dev->pci);
  take_controller(sc);

  if (controller_reset(sc) < 0) {
    printf("%s: the controller will not reset\n", dev->name);
    goto fail;
  }

  hcs1 = rd4(sc->cap, XHCI_HCSPARAMS1);
  hcs2 = rd4(sc->cap, XHCI_HCSPARAMS2);
  hcc = rd4(sc->cap, XHCI_HCCPARAMS);
  page = rd4(sc->opr, XHCI_PAGESIZE);

  sc->maxslots = XHCI_HCS1_DEVSLOT_MAX(hcs1);
  sc->maxports = XHCI_HCS1_N_PORTS(hcs1);
  sc->ctxsize = XHCI_HCC_CSZ(hcc) ? 64 : 32;
  sc->wide_dma = XHCI_HCC_AC64(hcc) != 0;

  if ((page & 1) == 0) {
    printf("%s: the controller will not work in 4K pages\n", dev->name);
    goto fail;
  }
  if (sc->maxslots == 0 || sc->maxports == 0 || sc->maxports > USB_MAX_PORTS) {
    printf("%s: %d slots and %d ports is not a controller we understand\n",
            dev->name, sc->maxslots, sc->maxports);
    goto fail;
  }
  sc->slot = malloc((sc->maxslots + 1) * sizeof(*sc->slot), M_DEVBUF,
                    M_WAITOK | M_ZERO);
  sc->port_major = malloc(sc->maxports + 1, M_DEVBUF, M_WAITOK | M_ZERO);
  sc->port_state = malloc(sc->maxports + 1, M_DEVBUF, M_WAITOK | M_ZERO);

  sc->dcbaa = dma_page(sc, &phys);
  sc->evt = dma_page(sc, &sc->evt_phys);
  sc->erst = dma_page(sc, &erst);
  if (sc->dcbaa == 0 || sc->evt == 0 || sc->erst == 0 ||
      scratchpad_setup(sc, XHCI_HCS2_SPB_MAX(hcs2)) < 0 ||
      ring_init(sc, &sc->cmd) < 0) {
    printf("%s: out of memory\n", dev->name);
    goto fail;
  }
  sc->dcbaa_phys = phys;
  if (driver_setup_intr(dev) < 0) {
    printf("%s: no interrupt\n", dev->name);
    goto fail;
  }
  if (xhci_run(sc) < 0) {
    printf("%s: the controller will not run\n", dev->name);
    goto fail;
  }
  running = 1;

  protocol_map(sc);
  intel_route_ports(sc);
  printf("%s: %d slots, %d ports, %d byte contexts\n", dev->name, sc->maxslots,
          sc->maxports, sc->ctxsize);

  sc->bus.methods = &xhci_bus_methods;
  sc->bus.hcpriv = sc;
  sc->bus.name = dev->name;
  sc->bus.root = &sc->root;
  sc->root.bus = &sc->bus;
  sc->root.speed = USB_SPEED_SUPER;
  sc->root.depth = 0;
  if (usbd_hub_ports(&sc->root, (int)sc->maxports) < 0)
    goto fail;
  sc->root.explore = xhci_root_explore;
  sc->root.in_use = 1;
  strlcpy(sc->root.name, dev->name, sizeof(sc->root.name));
  dev->driver_data = sc;

  ports_power(sc);
  usbd_attach_bus(&sc->bus);
  return 0;

fail:
  if (running) {
    wr4(sc->opr, XHCI_USBCMD, rd4(sc->opr, XHCI_USBCMD) & ~XHCI_CMD_RS);
    wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100);
  }
  xhci_free(sc);
  return -1;
}

static int xhci_intr(struct device *dev) {
  struct xhci_softc *sc = dev->driver_data;
  uint32_t sts;

  if (sc == 0)
    return 0;
  sts = rd4(sc->opr, XHCI_USBSTS);
  if (sts == 0xffffffff || (sts & (XHCI_STS_EINT | XHCI_STS_HSE | XHCI_STS_HCE)) == 0)
    return 0;
  wr4(sc->opr, XHCI_USBSTS, sts & (XHCI_STS_EINT | XHCI_STS_HSE));
  wr4(sc->rts, XHCI_IMAN(0), rd4(sc->rts, XHCI_IMAN(0)) | XHCI_IMAN_INTR_PEND);
  events_service(sc);
  return 1;
}

static void xhci_shutdown(struct device *dev) {
  struct xhci_softc *sc = dev->driver_data;

  sc->dead = 1;
  wr4(sc->opr, XHCI_USBCMD, rd4(sc->opr, XHCI_USBCMD) & ~XHCI_CMD_RS);
  wait_bits(sc->opr, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 100);
}

static struct driver xhci_driver = {
    .name = "xhci",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_BUS,
    .probe = xhci_probe,
    .attach = xhci_attach,
    .intr = xhci_intr,
    .shutdown = xhci_shutdown,
    .suspend = xhci_suspend,
    .resume = xhci_resume,
};

void xhci_driver_init(void) { driver_register(&xhci_driver); }
