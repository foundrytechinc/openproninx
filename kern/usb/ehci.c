#include "ehcireg.h"
#include "malloc.h"
#include "defs.h"
#include "driver.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "proc.h"
#include "mutex.h"
#include "usbdi.h"
#include "x86.h"

#define EHCI_FLSIZE 1024
#define EHCI_IPOLLRATES 8
#define EHCI_INTRQHS ((1 << EHCI_IPOLLRATES) - 1)
#define EHCI_MAX_POLLRATE (1 << (EHCI_IPOLLRATES - 1))
#define EHCI_IQHIDX(lev, pos) ((((pos) & ((1 << (lev)) - 1)) | (1 << (lev))) - 1)
#define EHCI_ILEV_IVAL(lev) (1 << (lev))
#define EHCI_QH_STRIDE 128
#define XFER_TIMEOUT_MS 3000
#define PORT_RESET_MS 50
#define PORT_RESET_DONE_MS 2
#define EHCI_WAITS 100 // hz / 128 apart
#define EHCI_WAIT_MS 8

#define EHCI_QTDS (MAXPHYS / (4 * PGSIZE) + 3)
struct ehci_pipe {
  struct ehci_qh qh;
  struct ehci_qtd qtd[EHCI_QTDS] __aligned(32);
  uint8_t setup[8] __aligned(32);
  int qlen[EHCI_QTDS];
  int nqtd;
  uint32_t phys;
  struct usb_pipe *pipe;
  struct usb_device *dev;
  struct ehci_softc *sc;
  struct ehci_pipe *next;
  struct ehci_pipe *active_next, **active_prev;
  int type;
  int islot;
  int toggle;
  int pending;
  int ctl_status;
  int ctl_len;
  volatile int ctl_done;
  void *isoc;
  uint32_t isoc_phys;
  int per_itd;
  int per_frame;
  int ndesc;
  int stride;
  int frame_next;
  int frame[USB_ISOC_SEGS];
  int running;
  int xacterrs;
};

struct ehci_dev {
  struct ehci_pipe *ep0;
  uint8_t *buf;
  uint32_t buf_phys;
  int addr;
  volatile int gone;
};

struct ehci_softc {
  struct mutex lock;
  struct device *dev;
  volatile uint8_t *cap, *opr;
  uint32_t *flist;
  uint32_t flist_phys;
  struct ehci_qh *islot[EHCI_INTRQHS];
  struct ehci_qh *async;
  uint32_t async_phys;
  struct ehci_pipe *async_first;
  struct ehci_pipe *intr_first[EHCI_INTRQHS];
  struct ehci_pipe *active;
  struct ehci_pipe *isoc_first;
  uint rand;
  int nports, ppc, ettf;
  uint8_t addr_used[128];
  uint8_t *port_state;
  int asleep;
  volatile int port_changed;
  volatile int doorbell;
  int wake;
  struct usb_bus bus;
  struct usb_device root;
  const char *name;
};

static const uint8_t revbits[EHCI_MAX_POLLRATE] = {
0x00,0x40,0x20,0x60,0x10,0x50,0x30,0x70,0x08,0x48,0x28,0x68,0x18,0x58,0x38,0x78,
0x04,0x44,0x24,0x64,0x14,0x54,0x34,0x74,0x0c,0x4c,0x2c,0x6c,0x1c,0x5c,0x3c,0x7c,
0x02,0x42,0x22,0x62,0x12,0x52,0x32,0x72,0x0a,0x4a,0x2a,0x6a,0x1a,0x5a,0x3a,0x7a,
0x06,0x46,0x26,0x66,0x16,0x56,0x36,0x76,0x0e,0x4e,0x2e,0x6e,0x1e,0x5e,0x3e,0x7e,
0x01,0x41,0x21,0x61,0x11,0x51,0x31,0x71,0x09,0x49,0x29,0x69,0x19,0x59,0x39,0x79,
0x05,0x45,0x25,0x65,0x15,0x55,0x35,0x75,0x0d,0x4d,0x2d,0x6d,0x1d,0x5d,0x3d,0x7d,
0x03,0x43,0x23,0x63,0x13,0x53,0x33,0x73,0x0b,0x4b,0x2b,0x6b,0x1b,0x5b,0x3b,0x7b,
0x07,0x47,0x27,0x67,0x17,0x57,0x37,0x77,0x0f,0x4f,0x2f,0x6f,0x1f,0x5f,0x3f,0x7f,
};

static inline uint32_t oprd(struct ehci_softc *sc, uint32_t off) {
  return *(volatile uint32_t *)(sc->opr + off);
}

static inline void opwr(struct ehci_softc *sc, uint32_t off, uint32_t v) {
  *(volatile uint32_t *)(sc->opr + off) = v;
}

_Static_assert(sizeof(struct ehci_pipe) <= PGSIZE, "ehci_pipe wants a page");

static void *low_page(uint32_t *phys) {
  char *p = kalloc_low();

  if (p == 0)
    return 0;
  memset(p, 0, PGSIZE);
  if (phys)
    *phys = (uint32_t)V2P(p);
  return p;
}

// THE SCHEDULES

static void async_link(struct ehci_softc *sc, struct ehci_pipe *ep) {
  ep->next = sc->async_first;
  sc->async_first = ep;
  ep->qh.link = sc->async->link;
  mb();
  sc->async->link = ep->phys | EHCI_LINK_QH;
}

static void intr_link(struct ehci_softc *sc, struct ehci_pipe *ep) {
  struct ehci_qh *head = sc->islot[ep->islot];

  ep->next = sc->intr_first[ep->islot];
  sc->intr_first[ep->islot] = ep;
  ep->qh.link = head->link;
  mb();
  head->link = ep->phys | EHCI_LINK_QH;
}

static void list_unlink(struct ehci_pipe **first, struct ehci_qh *head,
                        struct ehci_pipe *ep) {
  struct ehci_pipe **pp;
  struct ehci_qh *prev = head;

  for (pp = first; *pp != 0 && *pp != ep; pp = &(*pp)->next)
    prev = &(*pp)->qh;
  if (*pp != ep)
    return;
  *pp = ep->next;
  prev->link = ep->qh.link;
  mb();
}

// doorbell by interrupt; polled where nobody may sleep
static void async_sync(struct ehci_softc *sc) {
  uint64_t deadline = ticks + HZ / 2 + 1;
  int i;

  // an async schedule the hc does not walk: unlinked at once
  if (!(oprd(sc, EHCI_USBSTS) & EHCI_STS_ASS))
    return;
  sc->doorbell = 0;
  opwr(sc, EHCI_USBCMD, oprd(sc, EHCI_USBCMD) | EHCI_CMD_IAAD);
  if (curproc != 0 && !usbd_polling && curcpu()->spinlock_count == 1 &&
      curcpu()->spinlock_intr) {
    while (!sc->doorbell && ticks < deadline)
      msleep(&sc->wake, &sc->lock, PRIBIO, "ehcidb", (int)(deadline - ticks));
    return;
  }
  for (i = 0; i < 500 && !sc->doorbell; i++) {
    if (oprd(sc, EHCI_USBSTS) & EHCI_STS_IAA) {
      opwr(sc, EHCI_USBSTS, EHCI_STS_IAA);
      sc->doorbell = 1;
    }
    delay(1000);
  }
}

static void active_add(struct ehci_softc *sc, struct ehci_pipe *ep) {
  if (!ep->pending) {
    if ((ep->active_next = sc->active) != 0)
      sc->active->active_prev = &ep->active_next;
    ep->active_prev = &sc->active;
    sc->active = ep;
  }
  ep->pending = 1;
}

static void active_remove(struct ehci_softc *sc, struct ehci_pipe *ep) {
  (void)sc;
  if (ep->pending) {
    if (ep->active_next != 0)
      ep->active_next->active_prev = ep->active_prev;
    *ep->active_prev = ep->active_next;
    ep->active_next = 0;
    ep->active_prev = 0;
  }
  ep->pending = 0;
}

// TRANSFERS ON A QUEUE HEAD

static uint32_t qtd_phys(struct ehci_pipe *ep, int i) {
  return ep->phys + (uint32_t)((uintptr_t)&ep->qtd[i] - (uintptr_t)ep);
}

static void qtd_fill(struct ehci_pipe *ep, int i, uint32_t next, uint32_t alt,
                     int pid, uint32_t buf, int len, int toggle) {
  struct ehci_qtd *q = &ep->qtd[i];
  int k;

  q->next = next;
  q->altnext = alt;
  for (k = 0; k < EHCI_QTD_NBUFFERS; k++) {
    q->buffer[k] = (buf & ~0xfffu) + (uint32_t)k * PGSIZE;
    q->buffer_hi[k] = 0;
  }
  q->buffer[0] = buf;
  ep->qlen[i] = len;
  mb();
  q->status = EHCI_QTD_ACTIVE | EHCI_QTD_SET_CERR(3) | EHCI_QTD_SET_PID(pid) |
              EHCI_QTD_SET_BYTES(len) | (toggle ? EHCI_QTD_TOGGLE : 0) |
              EHCI_QTD_IOC;
}

static void qh_start(struct ehci_pipe *ep) {
  struct ehci_qh *qh = &ep->qh;

  qh->curqtd = 0;
  qh->ovl.altnext = EHCI_LINK_TERMINATE;
  qh->ovl.status = 0;
  mb();
  qh->ovl.next = qtd_phys(ep, 0);
  mb();
  ep->xacterrs = 0;
  active_add(ep->sc, ep);
}

#define QTD_BYTES (4 * PGSIZE)
static void submit(struct ehci_pipe *ep, int len) {
  struct usb_pipe *p = ep->pipe;
  int in = UE_GET_DIR(p->edesc.bEndpointAddress) != 0;
  int i, n = len <= 0 ? 1 : (len + QTD_BYTES - 1) / QTD_BYTES, off = 0;

  if (n > EHCI_QTDS)
    n = EHCI_QTDS;

  for (i = 0; i < n; i++) {
    int chunk = len - off > QTD_BYTES ? QTD_BYTES : len - off;

    qtd_fill(ep, i, i + 1 < n ? qtd_phys(ep, i + 1) : EHCI_LINK_TERMINATE,
             EHCI_LINK_TERMINATE, in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
             (uint32_t)p->buf_phys + (uint32_t)off, chunk, ep->toggle);
    off += chunk;
  }
  ep->nqtd = n;
  qh_start(ep);
}

static void ctl_submit(struct ehci_pipe *ep, const struct usb_device_request *req,
                       uint32_t data, int n, int in) {
  int i = 1;

  memmove(ep->setup, req, 8);
  qtd_fill(ep, 0, qtd_phys(ep, 1), EHCI_LINK_TERMINATE, EHCI_QTD_PID_SETUP,
           ep->phys + (uint32_t)((uintptr_t)ep->setup - (uintptr_t)ep), 8, 0);
  if (n) {
    qtd_fill(ep, 1, qtd_phys(ep, 2), qtd_phys(ep, 2),
             in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT, data, n, 1);
    i = 2;
  }
  qtd_fill(ep, i, EHCI_LINK_TERMINATE, EHCI_LINK_TERMINATE,
           n && in ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN, 0, 0, 1);
  ep->nqtd = i + 1;
  qh_start(ep);
}

static int chain_status(struct ehci_pipe *ep, int *len, int *toggle) {
  uint32_t st = 0, s;
  int i, got = 0, ctl = ep->type == UE_CONTROL;

  s = ep->qtd[ep->nqtd - 1].status;
  if (s & EHCI_QTD_ACTIVE) {
    for (i = 0; i < ep->nqtd - 1; i++) {
      s = ep->qtd[i].status;
      if (s & EHCI_QTD_ACTIVE)
        return -1;
      if (s & EHCI_QTD_HALTED)
        break;
      if (EHCI_QTD_GET_BYTES(s) != 0 && !ctl)
        break;
    }
    if (i == ep->nqtd - 1)
      return -1;
  }
  for (i = 0; i < ep->nqtd; i++) {
    s = ep->qtd[i].status;
    if (s & EHCI_QTD_ACTIVE)
      break;
    st = s;
    if (((s >> 8) & 3) != EHCI_QTD_PID_SETUP)
      got += ep->qlen[i] - (int)EHCI_QTD_GET_BYTES(s);
  }
  *len = got;
  *toggle = (st & EHCI_QTD_TOGGLE) != 0;
  if (st & EHCI_QTD_HALTED)
    return (st & EHCI_QTD_STATERRS) ? USBD_IOERROR : USBD_STALLED;
  return USBD_NORMAL;
}

// xact errors behind a TT: retry the qTD before telling anyone
#define XACTERR_MAX 32

static int xact_retry(struct ehci_pipe *ep) {
  uint32_t s = 0;
  int i;

  for (i = 0; i < ep->nqtd; i++) {
    s = ep->qtd[i].status;
    if (s & EHCI_QTD_ACTIVE)
      return 0;
    if (s & EHCI_QTD_HALTED)
      break;
  }
  if (i == ep->nqtd || (s & EHCI_QTD_STATERRS) != EHCI_QTD_XACTERR ||
      (s & EHCI_QTD_SET_CERR(3)) != 0 || ++ep->xacterrs >= XACTERR_MAX)
    return 0;
  s = (s & ~(uint32_t)EHCI_QTD_HALTED) | EHCI_QTD_ACTIVE | EHCI_QTD_SET_CERR(3);
  ep->qtd[i].status = s;
  mb();
  ep->qh.ovl.status = s;
  return 1;
}

// lock held
static void complete(struct ehci_softc *sc, struct ehci_pipe *ep, int status,
                     int len, int toggle) {
  struct usb_pipe *p = ep->pipe;

  active_remove(sc, ep);
  if (ep->type == UE_CONTROL) {
    ep->ctl_status = status;
    ep->ctl_len = len;
    ep->ctl_done = 1;
    return;
  }
  ep->toggle = status == USBD_STALLED ? 0 : toggle;
  if (status == USBD_NORMAL && len < p->xfer_len)
    status = USBD_SHORT;
  if (p->done == 0) {
    p->sync_status = status;
    p->sync_len = len;
    p->sync_done = 1;
    return;
  }
  if (!usbd_polling || p->polled)
    p->done(p, p->arg, status, len);
  if (p->repeat && status != USBD_STALLED && status != USBD_IOERROR)
    submit(ep, p->xfer_len);
}

static void isoc_service(struct ehci_softc *sc, struct ehci_pipe *ep);

// what a failed transfer's qTD said, for the first few
static void report_error(struct ehci_softc *sc, struct ehci_pipe *ep, int status) {
  static int reported;
  uint32_t st = 0;
  int i;

  if (reported >= 16)
    return;
  reported++;
  for (i = 0; i < ep->nqtd; i++)
    if (ep->qtd[i].status & EHCI_QTD_HALTED) {
      st = ep->qtd[i].status;
      break;
    }
  printf("%s: addr %d ep 0x%x %s, qtd %d token 0x%x%s%s%s%s, usbsts 0x%x\n",
         sc->name, ep->dev != 0 ? ep->dev->address : -1,
         ep->pipe != 0 ? ep->pipe->edesc.bEndpointAddress : 0,
         status == USBD_STALLED ? "stall" : "error", i, st,
         (st & EHCI_QTD_BUFERR) ? " buffer" : "",
         (st & EHCI_QTD_BABBLE) ? " babble" : "",
         (st & EHCI_QTD_XACTERR) ? " xact" : "",
         (st & EHCI_QTD_MISSEDMICRO) ? " missed" : "", oprd(sc, EHCI_USBSTS));
}

static void service_locked(struct ehci_softc *sc) {
  struct ehci_pipe *ep, *next;
  int len, toggle, status;

  for (ep = sc->active; ep != 0; ep = next) {
    next = ep->active_next;
    if ((status = chain_status(ep, &len, &toggle)) >= 0) {
      if (status == USBD_IOERROR && xact_retry(ep))
        continue;
      if (status == USBD_IOERROR || (status == USBD_STALLED && ep->type != UE_CONTROL))
        report_error(sc, ep, status);
      complete(sc, ep, status, len, toggle);
    }
  }
  for (ep = sc->isoc_first; ep != 0; ep = ep->next)
    if (ep->running)
      isoc_service(sc, ep);
}

static void service(struct ehci_softc *sc) {
  mtx_enter(&sc->lock);
  service_locked(sc);
  wakeup(&sc->wake);
  mtx_leave(&sc->lock);
  if (sc->port_changed) {
    sc->port_changed = 0;
    usbd_needs_explore(&sc->bus);
  }
}

static int wait_flag(struct ehci_softc *sc, volatile int *flag, int ms) {
  uint64_t deadline = ticks + (uint64_t)ms * HZ / 1000 + 1;
  int i;

  if (curproc == 0 || (readeflags() & FL_IF) == 0) {
    for (i = 0; i < ms * 10; i++) {
      mtx_enter(&sc->lock);
      service_locked(sc);
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
    service_locked(sc);
    if (*flag) {
      mtx_leave(&sc->lock);
      return 0;
    }
    if (ticks >= deadline) {
      mtx_leave(&sc->lock);
      return -1;
    }
    msleep(&sc->wake, &sc->lock, PRIBIO, "ehci", (int)(deadline - ticks));
  }
}

// no qTD goes active again; let go of the QH first
static void abort_pipe(struct ehci_softc *sc, struct ehci_pipe *ep) {
  int i;

  mtx_enter(&sc->lock);
  if (!ep->pending) {
    mtx_leave(&sc->lock);
    return;
  }
  for (i = 0; i < ep->nqtd; i++)
    ep->qtd[i].status = (ep->qtd[i].status & ~EHCI_QTD_ACTIVE) | EHCI_QTD_HALTED;
  ep->qh.ovl.status = (ep->qh.ovl.status & ~EHCI_QTD_ACTIVE) | EHCI_QTD_HALTED;
  mb();
  if (ep->type != UE_INTERRUPT) {
    async_sync(sc);
  } else {
    mtx_leave(&sc->lock);
    usb_pause_ms(2);
    mtx_enter(&sc->lock);
  }
  ep->qh.ovl.next = EHCI_LINK_TERMINATE;
  ep->qh.curqtd = 0;
  active_remove(sc, ep);
  mtx_leave(&sc->lock);
}

static void ehci_clear_toggle(struct usb_pipe *pipe) {
  struct ehci_softc *sc = pipe->dev->bus->hcpriv;
  struct ehci_pipe *ep = pipe->hcpriv;

  if (ep == 0 || ep->type == UE_CONTROL || ep->type == UE_ISOCHRONOUS)
    return;
  mtx_enter(&sc->lock);
  ep->toggle = 0;
  ep->qh.ovl.status &= ~EHCI_QTD_TOGGLE;
  mtx_leave(&sc->lock);
}

// PIPES AND DEVICES

static int qh_speed(int speed) {
  return speed == USB_SPEED_LOW    ? EHCI_QH_SPEED_LOW
         : speed == USB_SPEED_FULL ? EHCI_QH_SPEED_FULL
                                   : EHCI_QH_SPEED_HIGH;
}

static void qh_setup(struct ehci_softc *sc, struct ehci_pipe *ep,
                     struct usb_device *dev, int epaddr, int type, int maxp) {
  int speed = qh_speed(dev->speed);
  int naks = type == UE_INTERRUPT ? 0 : speed == EHCI_QH_SPEED_HIGH ? 4 : 0;
  struct ehci_qh *qh = &ep->qh;

  ep->dev = dev;
  ep->type = type;
  qh->endp = EHCI_QH_SET_ADDR(dev->address) | EHCI_QH_SET_ENDPT(UE_GET_ADDR(epaddr)) |
             EHCI_QH_SET_EPS(speed) | EHCI_QH_DTC | EHCI_QH_SET_MPL(maxp) |
             (speed != EHCI_QH_SPEED_HIGH && type == UE_CONTROL ? EHCI_QH_CTL : 0) |
             EHCI_QH_SET_NRL(naks);
  qh->endphub = EHCI_QH_SET_MULT(1) | (type == UE_INTERRUPT ? EHCI_QH_SET_SMASK(0x02) : 0);
  if (speed != EHCI_QH_SPEED_HIGH) {
    int huba = dev->hs_hub != 0 ? dev->hs_hub->address : 0;
    int port = dev->hs_hub != 0 ? dev->hs_port : dev->port;

    if (dev->hs_hub != 0 && sc->ettf && dev->hs_hub->depth == 0)
      huba = 0;
    qh->endphub |= EHCI_QH_SET_HUBA(huba) | EHCI_QH_SET_PORT(port) |
                   (type == UE_INTERRUPT ? EHCI_QH_SET_CMASK(0x38) : 0);
  }
  qh->curqtd = 0;
  qh->ovl.next = EHCI_LINK_TERMINATE;
  qh->ovl.altnext = EHCI_LINK_TERMINATE;
  qh->ovl.status = 0;
}

static struct ehci_pipe *pipe_alloc(struct ehci_softc *sc) {
  uint32_t phys;
  struct ehci_pipe *ep = low_page(&phys);

  if (ep == 0)
    return 0;
  ep->phys = phys;
  ep->sc = sc;
  return ep;
}

static int ehci_new_device(struct usb_device *dev) {
  struct ehci_softc *sc = dev->bus->hcpriv;
  struct ehci_dev *ed;
  struct usb_device_request req;
  int a;

  if ((ed = (struct ehci_dev *)page_alloc()) == 0)
    return -1;
  memset(ed, 0, sizeof(*ed));
  for (a = 1; a < 128 && sc->addr_used[a]; a++)
    ;
  if (a == 128 || (ed->ep0 = pipe_alloc(sc)) == 0 ||
      (ed->buf = low_page(&ed->buf_phys)) == 0) {
    if (ed->ep0)
      page_free((char *)ed->ep0);
    page_free((char *)ed);
    return -1;
  }
  dev->hcpriv = ed;
  dev->address = 0;
  qh_setup(sc, ed->ep0, dev, 0, UE_CONTROL, dev->maxp0);
  mtx_enter(&sc->lock);
  async_link(sc, ed->ep0);
  mtx_leave(&sc->lock);

  req.bmRequestType = UT_WRITE_DEVICE;
  req.bRequest = UR_SET_ADDRESS;
  req.wValue = (uint16_t)a;
  req.wIndex = 0;
  req.wLength = 0;
  if (dev->bus->methods->control(dev, &req, 0, 0) < 0)
    return -1;
  usb_pause_ms(2);
  dev->address = (uint8_t)a;
  ed->addr = a;
  sc->addr_used[a] = 1;
  ed->ep0->qh.endp = (ed->ep0->qh.endp & ~0x7fu) | EHCI_QH_SET_ADDR(a);
  return 0;
}

static int root_port_reset(struct ehci_softc *sc, int port);
static int ehci_control(struct usb_device *dev,
                        const struct usb_device_request *req, void *data,
                        int *len);

static int ehci_root_reset(struct usb_bus *bus, int port) {
  return root_port_reset(bus->hcpriv, port - 1);
}

// ???
static int ehci_readdress(struct usb_device *dev) {
  struct ehci_softc *sc = dev->bus->hcpriv;
  struct ehci_dev *ed = dev->hcpriv;
  struct usb_device_request req;
  int rc;

  if (ed == 0 || ed->gone)
    return -1;
  abort_pipe(sc, ed->ep0);
  mtx_enter(&sc->lock);
  ed->ep0->qh.endp &= ~0x7fu;
  dev->address = 0;
  async_sync(sc);
  mtx_leave(&sc->lock);
  req.bmRequestType = UT_WRITE_DEVICE;
  req.bRequest = UR_SET_ADDRESS;
  req.wValue = (uint16_t)ed->addr;
  req.wIndex = 0;
  req.wLength = 0;
  rc = ehci_control(dev, &req, 0, 0);
  usb_pause_ms(2);
  mtx_enter(&sc->lock);
  ed->ep0->qh.endp |= EHCI_QH_SET_ADDR(ed->addr);
  dev->address = (uint8_t)ed->addr;
  async_sync(sc);
  mtx_leave(&sc->lock);
  return rc;
}

static void ehci_abort_device(struct usb_device *dev) {
  struct ehci_softc *sc = dev->bus->hcpriv;
  struct ehci_dev *ed = dev->hcpriv;
  struct ehci_pipe *ep, *next;
  struct usb_pipe *p;
  int i;

  mtx_enter(&sc->lock);
  ed->gone = 1;
  for (ep = sc->active; ep != 0; ep = next) {
    next = ep->active_next;
    if (ep->dev != dev)
      continue;
    for (i = 0; i < ep->nqtd; i++)
      ep->qtd[i].status = (ep->qtd[i].status & ~EHCI_QTD_ACTIVE) | EHCI_QTD_HALTED;
    ep->qh.ovl.status = EHCI_QTD_HALTED;
    ep->qh.ovl.next = EHCI_LINK_TERMINATE;
    active_remove(sc, ep);
    if (ep->type == UE_CONTROL) {
      ep->ctl_status = USBD_CANCELLED;
      ep->ctl_len = 0;
      ep->ctl_done = 1;
    } else if ((p = ep->pipe) != 0 && p->done == 0) {
      p->sync_status = USBD_CANCELLED;
      p->sync_len = 0;
      p->sync_done = 1;
    }
  }
  wakeup(&sc->wake);
  mtx_leave(&sc->lock);
}

static void ehci_free_device(struct usb_device *dev) {
  struct ehci_softc *sc = dev->bus->hcpriv;
  struct ehci_dev *ed = dev->hcpriv;

  if (ed == 0)
    return;
  abort_pipe(sc, ed->ep0);
  mtx_enter(&sc->lock);
  list_unlink(&sc->async_first, sc->async, ed->ep0);
  async_sync(sc);
  mtx_leave(&sc->lock);
  if (ed->addr)
    sc->addr_used[ed->addr] = 0;
  page_free((char *)ed->ep0);
  page_free((char *)ed->buf);
  page_free((char *)ed);
  dev->hcpriv = 0;
}

static int ehci_set_maxp0(struct usb_device *dev) {
  struct ehci_dev *ed = dev->hcpriv;

  if (ed == 0)
    return -1;
  ed->ep0->qh.endp = (ed->ep0->qh.endp & ~EHCI_QH_MPL_MASK) | EHCI_QH_SET_MPL(dev->maxp0);
  return 0;
}

static int ehci_set_hub(struct usb_device *dev, int nports, int tt_think) {
  (void)dev;
  (void)nports;
  (void)tt_think;
  return 0;
}

static int isoc_open(struct ehci_softc *sc, struct ehci_pipe *ep, struct usb_pipe *pipe);

static int ehci_open_pipe(struct usb_pipe *pipe) {
  struct usb_device *dev = pipe->dev;
  struct ehci_softc *sc = dev->bus->hcpriv;
  struct ehci_pipe *ep;
  int type = UE_GET_XFERTYPE(pipe->edesc.bmAttributes);
  uint32_t phys;

  if (dev->hcpriv == 0 || (ep = pipe_alloc(sc)) == 0)
    return -1;
  ep->pipe = pipe;
  if (type == UE_ISOCHRONOUS) {
    if (isoc_open(sc, ep, pipe) < 0) {
      page_free((char *)ep);
      return -1;
    }
    pipe->hcpriv = ep;
    return 0;
  }
  if (type == UE_BULK) {
    if ((pipe->buf = dma_alloc(USB_XFER_MAX)) == 0) {
      page_free((char *)ep);
      return -1;
    }
    phys = (uint32_t)V2P(pipe->buf);
    pipe->buflen = USB_XFER_MAX;
  } else {
    if ((pipe->buf = low_page(&phys)) == 0) {
      page_free((char *)ep);
      return -1;
    }
    pipe->buflen = PGSIZE;
  }
  pipe->buf_phys = phys;
  qh_setup(sc, ep, dev, pipe->edesc.bEndpointAddress, type, pipe->maxp);
  mtx_enter(&sc->lock);
  if (type == UE_INTERRUPT) {
    int ival, lev;

    if (dev->speed == USB_SPEED_HIGH)
      ival = pipe->edesc.bInterval > 16 ? 256 : (1 << (pipe->edesc.bInterval - 1)) / 8;
    else
      ival = pipe->edesc.bInterval;
    for (lev = EHCI_IPOLLRATES - 1; lev > 0; lev--)
      if (EHCI_ILEV_IVAL(lev) <= ival)
        break;
    sc->rand = (sc->rand + 191) % EHCI_FLSIZE;
    ep->islot = EHCI_IQHIDX(lev, sc->rand);
    intr_link(sc, ep);
  } else
    async_link(sc, ep);
  mtx_leave(&sc->lock);
  pipe->hcpriv = ep;
  return 0;
}

static void isoc_close(struct ehci_softc *sc, struct ehci_pipe *ep);

static void ehci_close_pipe(struct usb_pipe *pipe) {
  struct ehci_softc *sc = pipe->dev->bus->hcpriv;
  struct ehci_pipe *ep = pipe->hcpriv;

  if (ep == 0)
    return;
  if (ep->type == UE_ISOCHRONOUS) {
    isoc_close(sc, ep);
  } else {
    abort_pipe(sc, ep);
    mtx_enter(&sc->lock);
    if (ep->type == UE_INTERRUPT) {
      list_unlink(&sc->intr_first[ep->islot], sc->islot[ep->islot], ep);
      mtx_leave(&sc->lock);
      usb_pause_ms(2);
    } else {
      list_unlink(&sc->async_first, sc->async, ep);
      async_sync(sc);
      mtx_leave(&sc->lock);
    }
  }
  if (pipe->buf) {
    if (pipe->buflen > PGSIZE)
      dma_free(pipe->buf);
    else
      page_free((char *)pipe->buf);
    pipe->buf = 0;
  }
  page_free((char *)ep);
  pipe->hcpriv = 0;
}

static int ehci_control(struct usb_device *dev,
                        const struct usb_device_request *req, void *data,
                        int *len) {
  struct ehci_softc *sc = dev->bus->hcpriv;
  struct ehci_dev *ed = dev->hcpriv;
  uint16_t n = req->wLength;
  int in = (req->bmRequestType & UT_READ) != 0;

  if (ed == 0 || ed->gone || n > PGSIZE)
    return -1;
  if (n != 0 && !in && data != 0)
    memmove(ed->buf, data, n);
  if (n != 0 && in)
    memset(ed->buf, 0, n);
  mtx_enter(&sc->lock);
  if (ed->gone) {
    mtx_leave(&sc->lock);
    return -1;
  }
  ed->ep0->ctl_done = 0;
  ctl_submit(ed->ep0, req, ed->buf_phys, n, in);
  mtx_leave(&sc->lock);
  if (wait_flag(sc, &ed->ep0->ctl_done, XFER_TIMEOUT_MS) < 0) {
    abort_pipe(sc, ed->ep0);
    return -1;
  }
  if (ed->ep0->ctl_status != USBD_NORMAL)
    return -1;
  if (n != 0 && in && data != 0)
    memmove(data, ed->buf, n);
  if (len)
    *len = ed->ep0->ctl_len;
  return 0;
}

static int ehci_start(struct usb_pipe *pipe) {
  struct ehci_softc *sc = pipe->dev->bus->hcpriv;
  struct ehci_pipe *ep = pipe->hcpriv;

  if (ep == 0 || ep->type == UE_ISOCHRONOUS)
    return -1;
  mtx_enter(&sc->lock);
  submit(ep, pipe->xfer_len);
  mtx_leave(&sc->lock);
  return 0;
}

static int ehci_transfer(struct usb_pipe *pipe, int len, int ms) {
  struct ehci_softc *sc = pipe->dev->bus->hcpriv;
  struct ehci_pipe *ep = pipe->hcpriv;

  if (ep == 0 || ep->type == UE_ISOCHRONOUS)
    return -1;
  mtx_enter(&sc->lock);
  if (((struct ehci_dev *)pipe->dev->hcpriv)->gone) {
    mtx_leave(&sc->lock);
    return -1;
  }
  pipe->sync_done = 0;
  pipe->xfer_len = len;
  submit(ep, len);
  mtx_leave(&sc->lock);
  if (wait_flag(sc, &pipe->sync_done, ms > 0 ? ms : XFER_TIMEOUT_MS) < 0) {
    abort_pipe(sc, ep);
    pipe->sync_status = USBD_TIMEOUT;
    pipe->sync_len = 0;
    return -1;
  }
  return pipe->sync_status == USBD_NORMAL || pipe->sync_status == USBD_SHORT ? 0 : -1;
}

// ISOCHRONOUS

static void frame_insert(struct ehci_softc *sc, int frame, uint32_t *link,
                         uint32_t self) {
  *link = sc->flist[frame];
  mb();
  sc->flist[frame] = self;
}

static void frame_remove(struct ehci_softc *sc, int frame, uint32_t self) {
  volatile uint32_t *pp = &sc->flist[frame];
  int guard;

  for (guard = 0; guard < 256 && !(*pp & EHCI_LINK_TERMINATE); guard++) {
    if (EHCI_LINK_ADDR(*pp) == EHCI_LINK_ADDR(self)) {
      volatile uint32_t *elem = (volatile uint32_t *)P2V((uintptr_t)EHCI_LINK_ADDR(*pp));
      *pp = *elem;
      mb();
      return;
    }
    pp = (volatile uint32_t *)P2V((uintptr_t)EHCI_LINK_ADDR(*pp));
  }
}

static int isoc_open(struct ehci_softc *sc, struct ehci_pipe *ep, struct usb_pipe *pipe) {
  struct usb_device *dev = pipe->dev;
  uint64_t phys;
  int b = pipe->edesc.bInterval;

  ep->type = UE_ISOCHRONOUS;
  if (dev->speed == USB_SPEED_LOW)
    return -1;
  if (dev->speed == USB_SPEED_HIGH) {
    int per_frame = b >= 4 ? 1 : 8 >> (b - 1);

    ep->per_itd = per_frame > 4 ? 4 : per_frame;
    ep->per_frame = per_frame / ep->per_itd;
    ep->stride = b > 4 ? 1 << (b - 4) : 1;
  } else {
    ep->per_itd = 1;
    ep->per_frame = 1;
    ep->stride = b < 1 ? 1 : b;
  }
  if ((ep->isoc = low_page(&ep->isoc_phys)) == 0)
    return -1;
  pipe->buf = dma_alloc((uint64_t)USB_ISOC_SEGS * USB_ISOC_STRIDE);
  if (pipe->buf == 0) {
    page_free((char *)ep->isoc);
    return -1;
  }
  phys = (uint64_t)V2P(pipe->buf);
  memset(pipe->buf, 0, (uint64_t)USB_ISOC_SEGS * USB_ISOC_STRIDE);
  pipe->buflen = USB_ISOC_SEGS * USB_ISOC_STRIDE;
  pipe->buf_phys = phys;
  mtx_enter(&sc->lock);
  ep->next = sc->isoc_first;
  sc->isoc_first = ep;
  mtx_leave(&sc->lock);
  return 0;
}

static uint32_t desc_self(struct ehci_pipe *ep, int i) {
  if (ep->pipe->dev->speed == USB_SPEED_HIGH)
    return (ep->isoc_phys + (uint32_t)(i * sizeof(struct ehci_itd))) | EHCI_LINK_ITD;
  return (ep->isoc_phys + (uint32_t)(i * sizeof(struct ehci_sitd))) | EHCI_LINK_SITD;
}

static void isoc_arm(struct ehci_softc *sc, struct ehci_pipe *ep, int i) {
  struct usb_pipe *p = ep->pipe;
  struct usb_device *dev = p->dev;
  int in = UE_GET_DIR(p->edesc.bEndpointAddress) != 0;
  int frame;

  if (ep->per_frame == 2 && (i & 1)) {
    frame = ep->frame[i - 1];
  } else {
    frame = ep->frame_next;
    ep->frame_next = (frame + ep->stride) % EHCI_FLSIZE;
  }
  ep->frame[i] = frame;
  if (dev->speed == USB_SPEED_HIGH) {
    struct ehci_itd *itd = (struct ehci_itd *)ep->isoc + i;
    int t, step = 8 / (ep->per_itd * ep->per_frame);
    int ubase = ep->per_frame == 2 && (i & 1) ? 4 : 0;

    memset(itd, 0, sizeof(*itd));
    itd->next = EHCI_LINK_TERMINATE;
    for (t = 0; t < ep->per_itd; t++) {
      int seg = i * ep->per_itd + t;

      if (seg >= p->nseg)
        break;
      itd->bufr[t] = (uint32_t)(p->buf_phys + (uint64_t)seg * USB_ISOC_STRIDE);
      itd->ctl[ubase + t * step] = EHCI_ITD_ACTIVE | EHCI_ITD_SET_LEN(p->seglen) |
                                   EHCI_ITD_SET_PG(t) | EHCI_ITD_IOC;
    }
    itd->bufr[0] |= EHCI_ITD_SET_EP(UE_GET_ADDR(p->edesc.bEndpointAddress)) |
                    EHCI_ITD_SET_DADDR(dev->address);
    itd->bufr[1] |= EHCI_ITD_SET_MAXPKT(p->maxp) | (in ? EHCI_ITD_DIR_IN : 0);
    itd->bufr[2] |= EHCI_ITD_SET_MULTI(p->burst + 1);
    mb();
    frame_insert(sc, frame, (uint32_t *)&itd->next, desc_self(ep, i));
  } else {
    struct ehci_sitd *sitd = (struct ehci_sitd *)ep->isoc + i;
    uint32_t buf = (uint32_t)(p->buf_phys + (uint64_t)i * USB_ISOC_STRIDE);
    int huba = dev->hs_hub != 0 ? dev->hs_hub->address : 0;
    int port = dev->hs_hub != 0 ? dev->hs_port : dev->port;
    uint32_t smask, cmask, tp = 0;

    if (dev->hs_hub != 0 && sc->ettf && dev->hs_hub->depth == 0)
      huba = 0;
    memset(sitd, 0, sizeof(*sitd));
    sitd->next = EHCI_LINK_TERMINATE;
    sitd->endp = (in ? EHCI_SITD_DIR_IN : 0) | EHCI_SITD_SET_PORT(port) |
                 EHCI_SITD_SET_HUBA(huba) |
                 EHCI_SITD_SET_ENDPT(UE_GET_ADDR(p->edesc.bEndpointAddress)) |
                 EHCI_SITD_SET_DADDR(dev->address);
    if (in) {
      smask = 0x01;
      cmask = 0xfc;
    } else {
      int tcount = p->seglen <= 188 ? 1 : (p->seglen + 187) / 188;

      tp = p->seglen <= 188 ? 1 : (uint32_t)tcount | 8;
      smask = (1u << tcount) - 1;
      cmask = 0;
    }
    sitd->sched = EHCI_SITD_SET_SMASK(smask) | EHCI_SITD_SET_CMASK(cmask);
    sitd->buffer[0] = buf;
    sitd->buffer[1] = ((buf & ~0xfffu) + PGSIZE) | tp;
    sitd->back = EHCI_LINK_TERMINATE;
    mb();
    sitd->trans = EHCI_SITD_ACTIVE | EHCI_SITD_SET_LEN(p->seglen) | EHCI_SITD_IOC;
    mb();
    frame_insert(sc, frame, (uint32_t *)&sitd->next, desc_self(ep, i));
  }
}

static int ehci_isoc_start(struct usb_pipe *pipe) {
  struct ehci_softc *sc = pipe->dev->bus->hcpriv;
  struct ehci_pipe *ep = pipe->hcpriv;
  int i;

  if (ep == 0 || ep->type != UE_ISOCHRONOUS)
    return -1;
  mtx_enter(&sc->lock);
  ep->ndesc = (pipe->nseg + ep->per_itd - 1) / ep->per_itd;
  ep->frame_next = (int)(((oprd(sc, EHCI_FRINDEX) >> 3) + 2) % EHCI_FLSIZE);
  for (i = 0; i < ep->ndesc; i++)
    isoc_arm(sc, ep, i);
  ep->running = 1;
  mtx_leave(&sc->lock);
  return 0;
}

static void isoc_service(struct ehci_softc *sc, struct ehci_pipe *ep) {
  struct usb_pipe *p = ep->pipe;
  int i, t;

  for (i = 0; i < ep->ndesc; i++) {
    if (p->dev->speed == USB_SPEED_HIGH) {
      struct ehci_itd *itd = (struct ehci_itd *)ep->isoc + i;
      int step = 8 / (ep->per_itd * ep->per_frame), busy = 0;
      int ubase = ep->per_frame == 2 && (i & 1) ? 4 : 0;

      for (t = 0; t < ep->per_itd; t++)
        if (itd->ctl[ubase + t * step] & EHCI_ITD_ACTIVE)
          busy = 1;
      if (busy)
        continue;
      frame_remove(sc, ep->frame[i], desc_self(ep, i));
      for (t = 0; t < ep->per_itd && i * ep->per_itd + t < p->nseg; t++) {
        uint32_t c = itd->ctl[ubase + t * step];
        int bad = EHCI_ITD_STATUS(c) != 0, in = UE_GET_DIR(p->edesc.bEndpointAddress) != 0;
        int len = bad ? 0 : in ? (int)EHCI_ITD_GET_LEN(c) : p->seglen;

        p->seg = i * ep->per_itd + t;
        if (!usbd_polling)
          p->done(p, p->arg, bad ? USBD_IOERROR : USBD_NORMAL, len);
      }
    } else {
      struct ehci_sitd *sitd = (struct ehci_sitd *)ep->isoc + i;
      uint32_t tr = sitd->trans;
      int len;

      if (tr & EHCI_SITD_ACTIVE)
        continue;
      frame_remove(sc, ep->frame[i], desc_self(ep, i));
      len = (tr & EHCI_SITD_ERRS) ? 0 : p->seglen - (int)EHCI_SITD_GET_LEN(tr);
      p->seg = i;
      if (!usbd_polling)
        p->done(p, p->arg, (tr & EHCI_SITD_ERRS) ? USBD_IOERROR : USBD_NORMAL, len);
    }
    if (p->repeat && ep->running)
      isoc_arm(sc, ep, i);
  }
}

static void isoc_close(struct ehci_softc *sc, struct ehci_pipe *ep) {
  struct ehci_pipe **pp;
  int i;

  mtx_enter(&sc->lock);
  ep->running = 0;
  for (i = 0; i < ep->ndesc; i++)
    frame_remove(sc, ep->frame[i], desc_self(ep, i));
  for (pp = &sc->isoc_first; *pp != 0; pp = &(*pp)->next)
    if (*pp == ep) {
      *pp = ep->next;
      break;
    }
  mtx_leave(&sc->lock);
  usb_pause_ms(2);
  page_free((char *)ep->isoc);
}

static void ehci_poll(struct usb_bus *bus) {
  struct ehci_softc *sc = bus->hcpriv;

  if (sc == 0 || !mtx_enter_try(&sc->lock))
    return;
  service_locked(sc);
  mtx_leave(&sc->lock);
}

static void ehci_bus_suspend(struct usb_bus *bus);
static int ehci_bus_resume(struct usb_bus *bus, int lost);
static void ehci_bus_restart(struct usb_bus *bus);

static const struct usb_bus_methods ehci_bus_methods = {
    .new_device = ehci_new_device,
    .free_device = ehci_free_device,
    .open_pipe = ehci_open_pipe,
    .close_pipe = ehci_close_pipe,
    .control = ehci_control,
    .start = ehci_start,
    .transfer = ehci_transfer,
    .clear_toggle = ehci_clear_toggle,
    .root_reset = ehci_root_reset,
    .readdress = ehci_readdress,
    .isoc_start = ehci_isoc_start,
    .set_maxp0 = ehci_set_maxp0,
    .set_hub = ehci_set_hub,
    .abort_device = ehci_abort_device,
    .poll = ehci_poll,
    .suspend = ehci_bus_suspend,
    .resume = ehci_bus_resume,
    .restart = ehci_bus_restart,
};

// THE ROOT HUB

static int root_port_reset(struct ehci_softc *sc, int port) {
  uint32_t v = oprd(sc, EHCI_PORTSC(port)) & ~EHCI_PS_CLEAR;
  int i;

  if (EHCI_PS_IS_LOWSPEED(v) && !sc->ettf) {
    opwr(sc, EHCI_PORTSC(port), v | EHCI_PS_PO);
    return -1;
  }
  v &= ~(EHCI_PS_PE | EHCI_PS_PR);
  opwr(sc, EHCI_PORTSC(port), v | EHCI_PS_PR);
  usb_pause_ms(PORT_RESET_MS);
  opwr(sc, EHCI_PORTSC(port), v);
  for (i = 0; i < 100; i++) {
    usb_pause_ms(PORT_RESET_DONE_MS);
    v = oprd(sc, EHCI_PORTSC(port));
    if (!(v & EHCI_PS_PR))
      break;
  }
  if (v & EHCI_PS_PR) {
    printf("%s: port %d reset timeout\n", sc->name, port);
    return -1;
  }
  if (!(v & EHCI_PS_PE)) {
    if (!sc->ettf)
      opwr(sc, EHCI_PORTSC(port), (v & ~EHCI_PS_CLEAR) | EHCI_PS_PO);
    return -1;
  }
  usb_pause_ms(10);
  return 0;
}

static int root_speed(struct ehci_softc *sc, uint32_t v) {
  if (!sc->ettf)
    return USB_SPEED_HIGH;
  switch (v & EHCI_PS_PSPD) {
  case EHCI_PS_PSPD_LS:
    return USB_SPEED_LOW;
  case EHCI_PS_PSPD_FS:
    return USB_SPEED_FULL;
  default:
    return USB_SPEED_HIGH;
  }
}

static int ehci_root_explore(struct usb_device *root) {
  struct ehci_softc *sc = root->bus->hcpriv;
  int port;

  for (port = 1; port <= sc->nports; port++) {
    uint32_t v = oprd(sc, EHCI_PORTSC(port - 1));
    int connected;

    if (v == 0xffffffff)
      break;
    connected = (v & EHCI_PS_CS) != 0 && !(v & EHCI_PS_PO);
    if (v & EHCI_PS_CLEAR)
      opwr(sc, EHCI_PORTSC(port - 1), v);
    if (connected == sc->port_state[port - 1])
      continue;
    sc->port_state[port - 1] = (uint8_t)connected;
    if (!connected) {
      if (root->child[port - 1]) {
        printf("usb: device on %s port %d unplugged\n", sc->name, port);
        usbd_free_device(root->child[port - 1]);
      }
      continue;
    }
    if (root_port_reset(sc, port - 1) < 0) {
      if (oprd(sc, EHCI_PORTSC(port - 1)) & EHCI_PS_PO) {
        printf("usb: %s port %d holds a full or low speed device, which "
                "is the companion controller's\n", sc->name, port);
        sc->port_state[port - 1] = 0;
      }
      continue;
    }
    v = oprd(sc, EHCI_PORTSC(port - 1));
    usbd_new_device(&sc->bus, root, 1, root_speed(sc, v), port, 0);
  }
  return 0;
}

// BRINGING IT UP

static void take_controller(struct ehci_softc *sc) {
  const struct pci_device *p = &sc->dev->pci;
  uint32_t cparams = *(volatile uint32_t *)(sc->cap + EHCI_HCCPARAMS);
  uint32_t eecp = EHCI_HCC_EECP(cparams), cap;
  int i;

  for (; eecp >= 0x40; eecp = (cap >> 8) & 0xff) {
    cap = pci_read32(p->bus, p->device, p->function, eecp);
    if ((cap & 0xff) != EHCI_CAP_ID_LEGACY)
      continue;
    if (cap & EHCI_LEG_HC_BIOS_OWNED) {
      pci_write32(p->bus, p->device, p->function, eecp, cap | EHCI_LEG_HC_OS_OWNED);
      for (i = 0; i < 500; i++) {
        cap = pci_read32(p->bus, p->device, p->function, eecp);
        if (!(cap & EHCI_LEG_HC_BIOS_OWNED))
          break;
        delay(1000);
      }
      if (cap & EHCI_LEG_HC_BIOS_OWNED)
        printf("%s: the bios will not give up the controller\n", sc->name);
    }
    pci_write32(p->bus, p->device, p->function, eecp + PCI_EHCI_USBLEGCTLSTS, 0);
  }
}

// halt first. bios, мать твою, оставил его запущенным
static int controller_reset(struct ehci_softc *sc) {
  int i;

  opwr(sc, EHCI_USBCMD, 0);
  for (i = 0; i < EHCI_WAITS && !(oprd(sc, EHCI_USBSTS) & EHCI_STS_HCH); i++)
    usb_pause_ms(EHCI_WAIT_MS);
  if (!(oprd(sc, EHCI_USBSTS) & EHCI_STS_HCH))
    printf("%s: the controller will not stop (status 0x%x)\n", sc->name,
           oprd(sc, EHCI_USBSTS));
  opwr(sc, EHCI_USBCMD, EHCI_CMD_HCRESET);
  for (i = 0; i < EHCI_WAITS; i++) {
    usb_pause_ms(EHCI_WAIT_MS);
    if (!(oprd(sc, EHCI_USBCMD) & EHCI_CMD_HCRESET))
      return 0;
  }
  return -1;
}

static int schedules_init(struct ehci_softc *sc) {
  int i, page = -1;
  uint32_t phys = 0;
  struct ehci_qh *base = 0;

  for (i = 0; i < EHCI_INTRQHS; i++) {
    struct ehci_qh *qh;

    if (i / (PGSIZE / EHCI_QH_STRIDE) != page) {
      page = i / (PGSIZE / EHCI_QH_STRIDE);
      if ((base = low_page(&phys)) == 0)
        return -1;
    }
    qh = (struct ehci_qh *)((uint8_t *)base + (i % (PGSIZE / EHCI_QH_STRIDE)) * EHCI_QH_STRIDE);
    sc->islot[i] = qh;
    qh->endp = EHCI_QH_SET_EPS(EHCI_QH_SPEED_HIGH);
    qh->endphub = EHCI_QH_SET_MULT(1);
    qh->curqtd = 0;
    qh->ovl.next = EHCI_LINK_TERMINATE;
    qh->ovl.altnext = EHCI_LINK_TERMINATE;
    qh->ovl.status = EHCI_QTD_HALTED;
  }
  for (i = 0; i < EHCI_INTRQHS; i++)
    sc->islot[i]->link = i == 0 ? EHCI_LINK_TERMINATE
                                : (uint32_t)V2P(sc->islot[(i + 1) / 2 - 1]) | EHCI_LINK_QH;
  if ((sc->flist = low_page(&sc->flist_phys)) == 0)
    return -1;
  for (i = 0; i < EHCI_FLSIZE; i++) {
    int j = (i & ~(EHCI_MAX_POLLRATE - 1)) | revbits[i & (EHCI_MAX_POLLRATE - 1)];
    sc->flist[j] = (uint32_t)V2P(sc->islot[EHCI_IQHIDX(EHCI_IPOLLRATES - 1, i)]) | EHCI_LINK_QH;
  }
  if ((sc->async = low_page(&sc->async_phys)) == 0)
    return -1;
  sc->async->endp = EHCI_QH_SET_EPS(EHCI_QH_SPEED_HIGH) | EHCI_QH_HRECL;
  sc->async->endphub = EHCI_QH_SET_MULT(1);
  sc->async->link = sc->async_phys | EHCI_LINK_QH;
  sc->async->curqtd = 0;
  sc->async->ovl.next = EHCI_LINK_TERMINATE;
  sc->async->ovl.altnext = EHCI_LINK_TERMINATE;
  sc->async->ovl.status = EHCI_QTD_HALTED;
  return 0;
}

static void schedules_free(struct ehci_softc *sc) {
  int i;

  for (i = 0; i < EHCI_INTRQHS; i += PGSIZE / EHCI_QH_STRIDE)
    if (sc->islot[i] != 0)
      page_free((char *)sc->islot[i]);
  memset(sc->islot, 0, sizeof(sc->islot));
  if (sc->flist != 0)
    page_free((char *)sc->flist);
  if (sc->async != 0)
    page_free((char *)sc->async);
  sc->flist = 0;
  sc->async = 0;
}

// host system error, a bus abort. cleared, mine now
static uint16_t bus_errors(struct ehci_softc *sc) {
  const struct pci_device *p = &sc->dev->pci;
  uint16_t st = pci_read16(p->bus, p->device, p->function, 0x06) & 0xf900;

  if (st != 0)
    pci_write16(p->bus, p->device, p->function, 0x06, st);
  return st;
}

static int ehci_run(struct ehci_softc *sc, int schedules) {
  uint32_t hcc = *(volatile uint32_t *)(sc->cap + EHCI_HCCPARAMS), cmd;
  int i;

  if (EHCI_HCC_64BIT(hcc))
    opwr(sc, EHCI_CTRLDSSEGMENT, 0);
  opwr(sc, EHCI_PERIODICLISTBASE, sc->flist_phys);
  opwr(sc, EHCI_ASYNCLISTADDR, sc->async_phys | EHCI_LINK_QH);
  cmd = oprd(sc, EHCI_USBCMD) & ~(EHCI_CMD_ITC_M | EHCI_CMD_FLS_M);
  opwr(sc, EHCI_USBCMD, cmd | EHCI_CMD_ITC_2 | EHCI_CMD_RS |
                            (schedules ? EHCI_CMD_ASE | EHCI_CMD_PSE : 0));
  opwr(sc, EHCI_CONFIGFLAG, EHCI_CONF_CF);
  for (i = 0; i < EHCI_WAITS; i++) {
    usb_pause_ms(EHCI_WAIT_MS);
    if (!(oprd(sc, EHCI_USBSTS) & EHCI_STS_HCH))
      break;
  }
  if (oprd(sc, EHCI_USBSTS) & EHCI_STS_HCH)
    return -1;
  if (sc->ppc)
    for (i = 0; i < sc->nports; i++)
      opwr(sc, EHCI_PORTSC(i), (oprd(sc, EHCI_PORTSC(i)) & ~EHCI_PS_CLEAR) | EHCI_PS_PP);
  opwr(sc, EHCI_USBINTR, EHCI_NORMAL_INTRS);
  return 0;
}

static int ehci_probe(struct device *dev) {
  if (dev == 0 || dev->bus != BUS_TYPE_PCI)
    return 0;
  return dev->pci.class_code == 0x0c && dev->pci.subclass == 0x03 &&
                 dev->pci.prog_if == 0x20
             ? 100
             : 0;
}

static void ehci_free(struct ehci_softc *sc) {
  int i;

  for (i = 0; i < EHCI_INTRQHS; i += PGSIZE / EHCI_QH_STRIDE)
    if (sc->islot[i] != 0)
      page_free((char *)sc->islot[i]);
  if (sc->flist != 0)
    page_free((char *)sc->flist);
  if (sc->async != 0)
    page_free((char *)sc->async);
  if (sc->port_state != 0)
    free(sc->port_state, M_DEVBUF);
  free(sc, M_DEVBUF);
}

static int ehci_attach(struct device *dev) {
  struct ehci_softc *sc;

  _Static_assert(sizeof(struct ehci_qtd) == 64, "qtd stride");
  _Static_assert(__offsetof(struct ehci_qh, ovl) == 16, "overlay");
  _Static_assert(sizeof(struct ehci_qh) <= EHCI_QH_STRIDE, "qh stride");
  uint32_t hcs, hcc;
  uint16_t st;
  int running = 0, tries;

  if (dev->mmio_vaddr == 0) {
    printf("%s: no register window\n", dev->name);
    return -1;
  }
  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  mtx_init(&sc->lock, "ehci");
  sc->dev = dev;
  sc->name = dev->name;
  sc->cap = (volatile uint8_t *)dev->mmio_vaddr;
  sc->opr = sc->cap + *(volatile uint8_t *)(sc->cap + EHCI_CAPLENGTH);
  hcs = *(volatile uint32_t *)(sc->cap + EHCI_HCSPARAMS);
  hcc = *(volatile uint32_t *)(sc->cap + EHCI_HCCPARAMS);
  sc->nports = (int)EHCI_HCS_N_PORTS(hcs);
  sc->ppc = EHCI_HCS_PPC(hcs) != 0;
  sc->ettf = 0;
  if (sc->nports == 0) {
    printf("%s: no ports\n", sc->name);
    goto fail;
  }
  sc->port_state = malloc((size_t)sc->nports, M_DEVBUF, M_WAITOK | M_ZERO);
  pci_enable_bus_master(&dev->pci);
  if ((st = bus_errors(sc)) != 0 || (oprd(sc, EHCI_USBSTS) & EHCI_STS_HSE))
    printf("%s: as the firmware left it: status 0x%x, bus errors 0x%x\n",
           sc->name, oprd(sc, EHCI_USBSTS), st);
  take_controller(sc);
  if (controller_reset(sc) < 0) {
    printf("%s: the controller will not reset\n", sc->name);
    goto fail;
  }
  if (EHCI_CMD_FLS(oprd(sc, EHCI_USBCMD)) != 0 && !EHCI_HCC_PFLF(hcc)) {
    printf("%s: a frame list that is not 1024 long\n", sc->name);
    goto fail;
  }
  if (schedules_init(sc) < 0) {
    printf("%s: out of memory\n", sc->name);
    goto fail;
  }
  if (driver_setup_intr(dev) < 0) {
    printf("%s: no interrupt\n", sc->name);
    goto fail;
  }
  for (tries = 0; ehci_run(sc, 1) < 0; tries++) {
    printf("%s: the controller will not run (status 0x%x, bus errors 0x%x, "
           "lists 0x%x%s 0x%x%s)\n",
           sc->name, oprd(sc, EHCI_USBSTS), bus_errors(sc),
           sc->flist_phys,
           bootinfo_is_ram(sc->flist_phys, sc->flist_phys + PGSIZE) ? "" : " not ram",
           sc->async_phys,
           bootinfo_is_ram(sc->async_phys, sc->async_phys + PGSIZE) ? "" : " not ram");
    // host system error halts it: reset, lists on other pages
    if (!(oprd(sc, EHCI_USBSTS) & EHCI_STS_HSE) || tries == 2)
      goto fail;
    schedules_free(sc);
    if (controller_reset(sc) < 0 || schedules_init(sc) < 0)
      goto fail;
  }
  running = 1;
  printf("%s: %d ports%s%s\n", sc->name, sc->nports, sc->ppc ? ", port power" : "",
          sc->ettf ? ", embedded tt" : "");

  sc->bus.methods = &ehci_bus_methods;
  sc->bus.hcpriv = sc;
  sc->bus.name = sc->name;
  sc->bus.root = &sc->root;
  sc->root.bus = &sc->bus;
  sc->root.speed = USB_SPEED_HIGH;
  sc->root.depth = 0;
  if (usbd_hub_ports(&sc->root, sc->nports) < 0)
    goto fail;
  sc->root.explore = ehci_root_explore;
  sc->root.in_use = 1;
  strlcpy(sc->root.name, sc->name, sizeof(sc->root.name));
  dev->driver_data = sc;
  usb_pause_ms(100);
  usbd_attach_bus(&sc->bus);
  return 0;

fail:
  if (running) {
    opwr(sc, EHCI_USBINTR, 0);
    opwr(sc, EHCI_USBCMD, oprd(sc, EHCI_USBCMD) & ~EHCI_CMD_RS);
    usb_pause_ms(2);
  }
  ehci_free(sc);
  return -1;
}

static int ehci_intr(struct device *dev) {
  struct ehci_softc *sc = dev->driver_data;
  uint32_t sts;

  if (sc == 0)
    return 0;
  sts = oprd(sc, EHCI_USBSTS);
  if (sts == 0xffffffff || EHCI_STS_INTRS(sts) == 0)
    return 0;
  opwr(sc, EHCI_USBSTS, EHCI_STS_INTRS(sts));
  if (sts & EHCI_STS_PCD)
    sc->port_changed = 1;
  if (sts & EHCI_STS_IAA) {
    mtx_enter(&sc->lock);
    sc->doorbell = 1;
    wakeup(&sc->wake);
    mtx_leave(&sc->lock);
  }
  if (sts & EHCI_STS_HSE)
    printf("%s: host system error\n", sc->name);
  service(sc);
  return 1;
}

static void ehci_shutdown(struct device *dev) {
  struct ehci_softc *sc = dev->driver_data;
  int i;

  opwr(sc, EHCI_USBINTR, 0);
  opwr(sc, EHCI_USBCMD, oprd(sc, EHCI_USBCMD) & ~EHCI_CMD_RS);
  for (i = 0; i < 100 && !(oprd(sc, EHCI_USBSTS) & EHCI_STS_HCH); i++)
    usb_pause_ms(1);
  // what latched while stopped holds the line, nobody acks
  opwr(sc, EHCI_USBSTS, EHCI_STS_INTRS(oprd(sc, EHCI_USBSTS)));
  opwr(sc, EHCI_CONFIGFLAG, 0);
}

// SLEEP

static void schedules_off(struct ehci_softc *sc) {
  int i;

  opwr(sc, EHCI_USBCMD, oprd(sc, EHCI_USBCMD) & ~(EHCI_CMD_ASE | EHCI_CMD_PSE));
  for (i = 0; i < EHCI_WAITS &&
              (oprd(sc, EHCI_USBSTS) & (EHCI_STS_ASS | EHCI_STS_PSS));
       i++)
    usb_pause_ms(1);
}

// schedules off, ports suspended, controller halted
static void ehci_bus_suspend(struct usb_bus *bus) {
  struct ehci_softc *sc = bus->hcpriv;
  uint32_t v;
  int i;

  schedules_off(sc);
  for (i = 0; i < sc->nports; i++) {
    v = oprd(sc, EHCI_PORTSC(i));
    if ((v & EHCI_PS_PE) && !(v & (EHCI_PS_SUSP | EHCI_PS_PO)))
      opwr(sc, EHCI_PORTSC(i), (v & ~EHCI_PS_CLEAR) | EHCI_PS_SUSP);
  }
  delay(150);
  opwr(sc, EHCI_USBINTR, 0);
  opwr(sc, EHCI_USBCMD, oprd(sc, EHCI_USBCMD) & ~EHCI_CMD_RS);
  for (i = 0; i < EHCI_WAITS && !(oprd(sc, EHCI_USBSTS) & EHCI_STS_HCH); i++)
    usb_pause_ms(1);
  opwr(sc, EHCI_USBSTS, EHCI_STS_INTRS(oprd(sc, EHCI_USBSTS)));
  sc->asleep = 1;
}

// reset every time. a hub behind it may have lost its own
static int ehci_bus_resume(struct usb_bus *bus, int lost __unused) {
  struct ehci_softc *sc = bus->hcpriv;

  take_controller(sc);
  sc->asleep = 0;
  if (controller_reset(sc) < 0 || ehci_run(sc, 0) < 0)
    printf("%s: the controller would not come back (status 0x%x)\n", sc->name,
           oprd(sc, EHCI_USBSTS));
  return 1;
}

static void ehci_bus_restart(struct usb_bus *bus) {
  struct ehci_softc *sc = bus->hcpriv;

  opwr(sc, EHCI_USBCMD, oprd(sc, EHCI_USBCMD) | EHCI_CMD_ASE | EHCI_CMD_PSE);
  memset(sc->port_state, 0, (size_t)sc->nports);
  usb_pause_ms(100);
}

static int ehci_suspend(struct device *dev) {
  struct ehci_softc *sc = dev->driver_data;

  if (!sc->asleep)
    ehci_shutdown(dev);
  return 0;
}

static int ehci_resume(struct device *dev) {
  (void)dev;
  return 0;
}

static struct driver ehci_driver = {
    .name = "ehci",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_BUS,
    .probe = ehci_probe,
    .attach = ehci_attach,
    .intr = ehci_intr,
    .shutdown = ehci_shutdown,
    .suspend = ehci_suspend,
    .resume = ehci_resume,
};

void ehci_driver_init(void) { driver_register(&ehci_driver); }
