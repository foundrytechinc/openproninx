#include "nvme.h"
#include "blockdev.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "malloc.h"
#include "mutex.h"
#include "x86.h"

struct nvme_tracker {
  volatile int busy;
  volatile int done;
  struct nvme_cqe cqe;
  uint64_t *prpl;
};

_Static_assert(NVME_ADMIN_Q_SIZE * sizeof(struct nvme_tracker) <= PGSIZE &&
                   NVME_IO_Q_SIZE * sizeof(struct nvme_tracker) <= PGSIZE,
               "nvme trackers must fit a page");

struct nvme_softc;

struct nvme_queue {
  struct nvme_softc *sc;
  struct nvme_sqe *sq;
  struct nvme_cqe *cq;
  void *sq_virt;
  void *cq_virt;
  uint16_t sq_tail;
  uint16_t cq_head;
  uint16_t next_cid;
  uint8_t  cq_phase;
  uint16_t qid;
  uint16_t size;
  volatile uint32_t *sq_db;
  volatile uint32_t *cq_db;
  int inflight;
  struct nvme_tracker *trackers;
  struct mutex lock;
};

struct nvme_ns {
  struct nvme_softc *sc;
  uint64_t sector_count;
  uint32_t sector_size;
  int nsid;
  volatile int dirty;
};

struct nvme_softc {
  volatile uint32_t *mmio;
  struct device *dev;
  uint32_t db_stride;
  struct nvme_queue admin_q;
  struct nvme_queue io_q;
  uint maxphys;
  uint rdy_to;
  int attached;
  struct mutex lock;
  volatile int resetting; // no command is begun
  struct proc *resetter;
  volatile int dead;
  volatile int suspended; // no command is begun
  char serial[21];        // the controller's SN
  char model[41];         // and MN
};

static struct nvme_ns **nvme_units;
static uint nvme_nunits;
static struct blockdrv nvme_blockdrv;

static struct nvme_ns *unit_ns(uint unit) {
  return unit < nvme_nunits ? nvme_units[unit] : 0;
}

static inline uint32_t nvme_read32(struct nvme_softc *sc, uint32_t reg) {
  return *(volatile uint32_t *)((uintptr_t)sc->mmio + reg);
}

static inline void nvme_write32(struct nvme_softc *sc, uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)((uintptr_t)sc->mmio + reg) = val;
}

static inline uint64_t nvme_read64(struct nvme_softc *sc, uint32_t reg) {
  uint32_t low = nvme_read32(sc, reg);
  uint32_t high = nvme_read32(sc, reg + 4);
  return (uint64_t)low | ((uint64_t)high << 32);
}

static inline void nvme_write64(struct nvme_softc *sc, uint32_t reg, uint64_t val) {
  nvme_write32(sc, reg, (uint32_t)val);
  nvme_write32(sc, reg + 4, (uint32_t)(val >> 32));
}

static int nvme_reap(struct nvme_queue *q) {
  volatile struct nvme_cqe *cqe;
  int found = 0;

  for (;;) {
    struct nvme_cqe c;
    cqe = &q->cq[q->cq_head];
    if ((cqe->status & 1) != q->cq_phase)
      break;
    c = *(struct nvme_cqe *)cqe;
    if (c.cid < q->size && q->trackers[c.cid].busy) {
      q->trackers[c.cid].cqe = c;
      q->trackers[c.cid].done = 1;
    }
    found = 1;
    q->cq_head = (q->cq_head + 1) % q->size;
    if (q->cq_head == 0)
      q->cq_phase ^= 1;
  }
  if (found) {
    *q->cq_db = q->cq_head;
    wakeup(q);
  }
  return found;
}

// lock held
static struct nvme_tracker *nvme_slot(struct nvme_queue *q, uint16_t *cid) {
  uint16_t i;

  mtx_enter(&q->lock);
  while (q->inflight >= q->size - 1 ||
         ((q->sc->suspended || q->sc->resetting) && q->qid != 0)) {
    if (q->sc->dead) {
      mtx_leave(&q->lock);
      return 0;
    }
    if (q->sc->attached && curproc != 0)
      msleep(q, &q->lock, PRIBIO, "nvme", 0);
    else {
      mtx_leave(&q->lock);
      delay(10);
      mtx_enter(&q->lock);
      nvme_reap(q);
    }
  }
  if (q->sc->dead) {
    mtx_leave(&q->lock);
    return 0;
  }
  for (i = q->next_cid; q->trackers[i].busy; i = (uint16_t)((i + 1) % q->size))
    ;
  q->next_cid = (uint16_t)((i + 1) % q->size);
  *cid = i;
  q->inflight++;
  q->trackers[i].busy = 1;
  q->trackers[i].done = 0;
  return &q->trackers[i];
}

static void nvme_reset(struct nvme_softc *sc);

#define NVME_TIMO_QOP 5000
#define NVME_TIMO_IDENT 10000
#define NVME_TIMO_IO 60000
#define NVME_TIMO_FLUSH 100000

static int nvme_submit(struct nvme_queue *q, struct nvme_tracker *t,
                       struct nvme_sqe *cmd, struct nvme_cqe *resp, uint ms) {
  uint64_t until = ticks + (uint64_t)ms * HZ / 1000 + 1;
  uint64_t us = (uint64_t)ms * 1000;
  uint16_t status;

  q->sq[q->sq_tail] = *cmd;
  q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->size);
  *q->sq_db = q->sq_tail;

  while (!t->done) {
    nvme_reap(q);
    if (t->done)
      break;
    if (q->sc->attached && curproc != 0 && (int64_t)(until - ticks) > 0)
      msleep(q, &q->lock, PRIBIO, "nvme", (int)(until - ticks));
    else if ((!q->sc->attached || curproc == 0) && us > 0) {
      us -= us < 10 ? us : 10;
      mtx_leave(&q->lock);
      delay(10);
      mtx_enter(&q->lock);
    } else {
      printf("%s: command 0x%x on queue %d timed out\n", q->sc->dev->name,
              cmd->opcode, q->qid);
      mtx_leave(&q->lock);
      nvme_reset(q->sc);
      mtx_enter(&q->lock);
      while (!t->done)
        msleep(q, &q->lock, PRIBIO, "nvreset", 0);
    }
  }

  if (resp)
    *resp = t->cqe;
  status = t->cqe.status >> 1;
  t->busy = 0;
  q->inflight--;
  wakeup(q);
  mtx_leave(&q->lock);

  return status == 0 ? 0 : -1;
}

static int nvme_submit_admin_cmd(struct nvme_softc *sc, struct nvme_sqe *cmd,
                                 struct nvme_cqe *resp, uint ms) {
  struct nvme_tracker *t = nvme_slot(&sc->admin_q, &cmd->cid);

  if (t == 0)
    return -1;
  return nvme_submit(&sc->admin_q, t, cmd, resp, ms);
}

static int nvme_init_queue(struct nvme_softc *sc, struct nvme_queue *q,
                           uint16_t qid, uint16_t size, uint32_t db_stride) {
  q->sc = sc;
  q->qid = qid;
  q->size = size;
  q->sq_tail = 0;
  q->cq_head = 0;
  q->cq_phase = 1;
  q->inflight = 0;
  mtx_init(&q->lock, "nvme_q");

  void *sq = page_alloc();
  void *cq = page_alloc();
  void *tr = page_alloc();
  int i;
  if (!sq || !cq || !tr) {
    if (sq)
      page_free(sq);
    if (cq)
      page_free(cq);
    if (tr)
      page_free(tr);
    return -1;
  }
  memset(sq, 0, PGSIZE);
  memset(cq, 0, PGSIZE);
  memset(tr, 0, PGSIZE);
  q->trackers = tr;
  for (i = 0; i < size; i++)
    if ((q->trackers[i].prpl = (uint64_t *)page_alloc()) == 0) {
      while (i-- > 0)
        page_free((char *)q->trackers[i].prpl);
      page_free(sq);
      page_free(cq);
      page_free(tr);
      return -1;
    }

  q->sq_virt = sq;
  q->cq_virt = cq;
  q->sq = (struct nvme_sqe *)sq;
  q->cq = (struct nvme_cqe *)cq;

  uintptr_t db_base = (uintptr_t)sc->mmio + NVME_REG_DBS;
  q->sq_db = (volatile uint32_t *)(db_base + (2 * qid) * db_stride);
  q->cq_db = (volatile uint32_t *)(db_base + (2 * qid + 1) * db_stride);

  return 0;
}

static int nvme_ready(struct nvme_softc *sc, uint32_t rdy) {
  uint i;

  for (i = 0; (nvme_read32(sc, NVME_REG_CSTS) & NVME_CSTS_RDY) != rdy; i++) {
    if (i > sc->rdy_to)
      return -1;
    delay(1000);
  }
  return 0;
}

static int nvme_disable(struct nvme_softc *sc) {
  uint32_t cc = nvme_read32(sc, NVME_REG_CC);

  if ((cc & NVME_CC_EN) && !(nvme_read32(sc, NVME_REG_CSTS) & NVME_CSTS_CFS) &&
      nvme_ready(sc, NVME_CSTS_RDY) != 0)
    return -1;
  nvme_write32(sc, NVME_REG_CC, cc & ~NVME_CC_EN);
  return nvme_ready(sc, 0);
}

static int nvme_enable(struct nvme_softc *sc) {
  nvme_write32(sc, NVME_REG_AQA,
               ((NVME_ADMIN_Q_SIZE - 1) << 16) | (NVME_ADMIN_Q_SIZE - 1));
  nvme_write64(sc, NVME_REG_ASQ, V2P(sc->admin_q.sq_virt));
  nvme_write64(sc, NVME_REG_ACQ, V2P(sc->admin_q.cq_virt));
  nvme_write32(sc, NVME_REG_CC, NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                                    NVME_CC_IOCQES | NVME_CC_IOSQES);
  return nvme_ready(sc, NVME_CSTS_RDY);
}

static int nvme_create_io(struct nvme_softc *sc) {
  struct nvme_sqe cmd;

  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_CREATE_IO_CQ;
  cmd.prp1 = (uint64_t)V2P(sc->io_q.cq_virt);
  cmd.cdw10 = ((NVME_IO_Q_SIZE - 1) << 16) | 1;
  cmd.cdw11 = (0 << 16) | 2 | 1;
  if (nvme_submit_admin_cmd(sc, &cmd, 0, NVME_TIMO_QOP) < 0) {
    printf("%s: no I/O completion queue\n", sc->dev->name);
    return -1;
  }
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_CREATE_IO_SQ;
  cmd.prp1 = (uint64_t)V2P(sc->io_q.sq_virt);
  cmd.cdw10 = ((NVME_IO_Q_SIZE - 1) << 16) | 1;
  cmd.cdw11 = (1 << 16) | 1;
  if (nvme_submit_admin_cmd(sc, &cmd, 0, NVME_TIMO_QOP) < 0) {
    printf("%s: no I/O submission queue\n", sc->dev->name);
    return -1;
  }
  return 0;
}

static void nvme_queue_reset(struct nvme_queue *q) {
  mtx_enter(&q->lock);
  q->sq_tail = 0;
  q->cq_head = 0;
  q->cq_phase = 1;
  memset(q->cq_virt, 0, PGSIZE);
  mtx_leave(&q->lock);
}

static void nvme_queue_free(struct nvme_queue *q) {
  int i;

  if (q->trackers == 0)
    return;
  for (i = 0; i < q->size; i++)
    page_free((char *)q->trackers[i].prpl);
  page_free((char *)q->trackers);
  page_free(q->sq_virt);
  page_free(q->cq_virt);
  q->trackers = 0;
}

#define NVME_SC_ABORTED_SQ_DELETION 0x08
static void nvme_fail_all(struct nvme_queue *q) {
  int i;

  if (q->trackers == 0)
    return;
  mtx_enter(&q->lock);
  for (i = 0; i < q->size; i++)
    if (q->trackers[i].busy && !q->trackers[i].done) {
      q->trackers[i].cqe.status = NVME_SC_ABORTED_SQ_DELETION << 1;
      q->trackers[i].done = 1;
    }
  wakeup(q);
  mtx_leave(&q->lock);
}

static void nvme_reset(struct nvme_softc *sc) {
  struct proc *me = curproc;
  int mine;

  mtx_enter(&sc->lock);
  if (sc->resetting) {
    mine = sc->resetter == me;
    mtx_leave(&sc->lock);
    if (mine) {
      nvme_disable(sc);
      nvme_fail_all(&sc->admin_q);
      nvme_fail_all(&sc->io_q);
    }
    return;
  }
  sc->resetting = 1;
  sc->resetter = me;
  mtx_leave(&sc->lock);
  printf("%s: resetting the controller\n", sc->dev->name);
  nvme_disable(sc);
  nvme_fail_all(&sc->admin_q);
  nvme_fail_all(&sc->io_q);
  nvme_queue_reset(&sc->admin_q);
  nvme_queue_reset(&sc->io_q);
  if (!sc->attached || nvme_enable(sc) < 0 || nvme_create_io(sc) < 0) {
    printf("%s: the controller is gone\n", sc->dev->name);
    nvme_disable(sc);
    nvme_fail_all(&sc->admin_q);
    nvme_fail_all(&sc->io_q);
    sc->dead = 1;
  }
  mtx_enter(&sc->lock);
  sc->resetting = 0;
  sc->resetter = 0;
  mtx_leave(&sc->lock);
  wakeup(&sc->admin_q);
  wakeup(&sc->io_q);
}

static int nvme_probe_device(struct device *dev) {
  if (dev->pci.class_code == PCI_CLASS_STORAGE &&
      dev->pci.subclass == PCI_SUBCLASS_NVME &&
      dev->pci.prog_if == PCI_PROGIF_NVME) {
    return 100;
  }
  return 0;
}

static void ident_string(char *out, const char *in, int len) {
  int i;

  memmove(out, in, (size_t)len);
  for (i = len; i > 0 && out[i - 1] == ' '; i--)
    ;
  out[i] = 0;
}

static void nvme_add_ns(struct nvme_softc *sc, uint32_t nsid, char *ident_buf) {
  struct nvme_sqe cmd;
  struct nvme_ns *ns, **grown;
  uint64_t nsze;
  uint8_t flbas, lbads;
  uint32_t sector_size;

  memset(ident_buf, 0, PGSIZE);
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_IDENTIFY;
  cmd.nsid = nsid;
  cmd.prp1 = (uint64_t)V2P(ident_buf);
  cmd.cdw10 = 0;
  if (nvme_submit_admin_cmd(sc, &cmd, 0, NVME_TIMO_IDENT) != 0)
    return;
  nsze = *(uint64_t *)(ident_buf + 0);
  flbas = *(uint8_t *)(ident_buf + 26);
  lbads = *(uint8_t *)(ident_buf + 128 + (flbas & 0x0F) * 4 + 2);
  sector_size = 1U << (lbads ? lbads : 9);
  if (nsze == 0)
    return;
  if (sector_size != NVME_SECTOR_SIZE) {
    printf("%s: namespace %u has %u-byte sectors; only %u-byte ones "
            "are supported, not attached\n",
            sc->dev->name, nsid, (uint)sector_size, (uint)NVME_SECTOR_SIZE);
    return;
  }
  ns = malloc(sizeof(*ns), M_DEVBUF, M_WAITOK | M_ZERO);
  ns->sc = sc;
  ns->nsid = (int)nsid;
  ns->sector_count = nsze;
  ns->sector_size = sector_size;
  grown = malloc((nvme_nunits + 1) * sizeof(*grown), M_DEVBUF, M_WAITOK);
  if (nvme_nunits > 0)
    memmove(grown, nvme_units, nvme_nunits * sizeof(*grown));
  if (nvme_units != 0)
    free(nvme_units, M_DEVBUF);
  nvme_units = grown;
  nvme_units[nvme_nunits++] = ns;
  nvme_blockdrv.units = nvme_nunits;
  printf("%s: namespace %u, %u MB (%lu sectors of %u bytes)\n",
          sc->dev->name, nsid, (uint)(nsze * sector_size / (1024 * 1024)),
          (uint64_t)nsze, (uint)sector_size);
}

static int nvme_attach_device(struct device *dev) {
  uint64_t bar0_phys = dev->pci.bar[0], cap;
  struct nvme_softc *sc;
  struct nvme_sqe cmd;
  char *ident_buf;
  uint32_t nn = 0, nsid;
  uint first = nvme_nunits;

  if (bar0_phys == 0 || dev->pci.bar_is_io[0]) {
    printf("%s: no memory BAR0\n", dev->name);
    return -1;
  }
  if (driver_setup_intr(dev) < 0) {
    printf("%s: no interrupt\n", dev->name);
    return -1;
  }
  sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  sc->mmio = (volatile uint32_t *)ioremap(bar0_phys, 0x2000);
  sc->dev = dev;
  mtx_init(&sc->lock, "nvme");

  cap = nvme_read64(sc, NVME_REG_CAP);
  sc->db_stride = 4 << (uint32_t)((cap >> 32) & 0x0F);
  sc->rdy_to = 500 * (uint)((cap >> 24) & 0xff);

  if (nvme_disable(sc) < 0) {
    printf("%s: the controller will not stop\n", dev->name);
    goto fail;
  }
  if (nvme_init_queue(sc, &sc->admin_q, 0, NVME_ADMIN_Q_SIZE, sc->db_stride) < 0) {
    printf("%s: no memory for the admin queues\n", dev->name);
    goto fail;
  }
  if (nvme_enable(sc) < 0) {
    printf("%s: the controller did not become ready\n", dev->name);
    goto fail;
  }
  if (nvme_init_queue(sc, &sc->io_q, 1, NVME_IO_Q_SIZE, sc->db_stride) < 0) {
    printf("%s: no memory for the I/O queues\n", dev->name);
    goto fail;
  }
  if (nvme_create_io(sc) < 0)
    goto fail;

  if ((ident_buf = page_alloc()) != 0) {
    memset(ident_buf, 0, PGSIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.prp1 = (uint64_t)V2P(ident_buf);
    cmd.cdw10 = 1;
    if (nvme_submit_admin_cmd(sc, &cmd, 0, NVME_TIMO_IDENT) == 0) {
      ident_string(sc->serial, ident_buf + 4, 20);
      ident_string(sc->model, ident_buf + 24, 40);
      if ((uint8_t)ident_buf[77] != 0) {
        uint mpsmin = (uint)((cap >> 48) & 0x0f);

        sc->maxphys = (1u << (12 + mpsmin)) << (uint8_t)ident_buf[77];
      }
      nn = *(uint32_t *)(ident_buf + 516);
    }
    if (!sc->dead)
      for (nsid = 1; nsid <= nn; nsid++)
        nvme_add_ns(sc, nsid, ident_buf);
    page_free(ident_buf);
  }
  if (sc->dead)
    goto fail;

  sc->attached = 1;
  dev->driver_data = sc;
  printf("%s: %s, %u of its %u namespaces in use\n", dev->name,
          sc->model[0] ? sc->model : "NVMe", nvme_nunits - first, nn);
  return 0;

fail:
  while (nvme_nunits > first)
    free(nvme_units[--nvme_nunits], M_DEVBUF);
  nvme_blockdrv.units = nvme_nunits > 0 ? nvme_nunits : 1;
  nvme_disable(sc);
  nvme_queue_free(&sc->io_q);
  nvme_queue_free(&sc->admin_q);
  free(sc, M_DEVBUF);
  return -1;
}

static int nvme_intr(struct device *dev) {
  struct nvme_softc *sc = dev->driver_data;
  int found;

  if (sc == 0)
    return 0;
  mtx_enter(&sc->admin_q.lock);
  found = nvme_reap(&sc->admin_q);
  mtx_leave(&sc->admin_q.lock);
  mtx_enter(&sc->io_q.lock);
  found |= nvme_reap(&sc->io_q);
  mtx_leave(&sc->io_q.lock);
  return found;
}

static void nvme_shutdown(struct device *dev) {
  struct nvme_softc *sc = dev->driver_data;
  uint32_t cc, csts;
  int i;

  if (sc == 0)
    return;
  cc = nvme_read32(sc, NVME_REG_CC);
  cc = (cc & ~NVME_CC_SHN_MASK) | NVME_CC_SHN_NORMAL;
  nvme_write32(sc, NVME_REG_CC, cc);

  for (i = 0; i < 4000; i++) {
    csts = nvme_read32(sc, NVME_REG_CSTS);
    if (csts == 0xffffffffu)
      return;
    if ((csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_DONE)
      return;
    delay(1000);
  }
  printf("%s: did not finish its shutdown, disabling\n", dev->name);
  nvme_disable(sc);
}

static void nvme_drain(struct nvme_queue *q) {
  mtx_enter(&q->lock);
  while (q->inflight > 0) {
    nvme_reap(q);
    if (q->inflight > 0)
      msleep(q, &q->lock, PRIBIO, "nvdrain", 0);
  }
  mtx_leave(&q->lock);
}

// SLEEP
static int nvme_suspend(struct device *dev) {
  struct nvme_softc *sc = dev->driver_data;

  if (sc == 0)
    return 0;
  sc->suspended = 1;
  nvme_drain(&sc->io_q);
  nvme_drain(&sc->admin_q);
  nvme_shutdown(dev);
  return 0;
}

static int nvme_resume(struct device *dev) {
  struct nvme_softc *sc = dev->driver_data;
  int err = 0;

  if (sc == 0)
    return 0;
  nvme_disable(sc);
  nvme_queue_reset(&sc->admin_q);
  nvme_queue_reset(&sc->io_q);
  if (nvme_enable(sc) < 0 || nvme_create_io(sc) < 0) {
    printf("%s: did not come back from sleep\n", dev->name);
    err = -1;
  }
  sc->suspended = 0;
  wakeup(&sc->io_q);
  return err;
}

static int nvme_major;

static uint64_t nvme_unit_size(uint unit) { return nvme_size(unit); }
static int nvme_unit_flush(uint unit);

static uint nvme_maxphys(uint unit) {
  struct nvme_ns *ns = unit_ns(unit);

  if (ns != 0 && ns->sc->maxphys != 0 && ns->sc->maxphys < MAXPHYS)
    return ns->sc->maxphys;
  return MAXPHYS;
}

static int nvme_ident(uint unit, struct fnu_diskinfo *out) {
  struct nvme_ns *ns = unit_ns(unit);

  if (ns == 0)
    return -1;
  strlcpy(out->model, ns->sc->model[0] ? ns->sc->model : "NVMe",
             FNU_DISK_MODEL_MAX);
  if (ns->sc->serial[0] != 0 && ns->nsid == 1)
    strlcpy(out->serial, ns->sc->serial, FNU_DISK_SERIAL_MAX);
  else if (ns->sc->serial[0] != 0)
    snprintf(out->serial, FNU_DISK_SERIAL_MAX, "%s-%d", ns->sc->serial, ns->nsid);
  out->kind = FNU_DISK_NVME;
  return 0;
}

static struct blockdrv nvme_blockdrv = {
    .name = "nvme",
    .prefix = "sd",
    .units = 1,
    .strategy = nvmerw,
    .maxphys = nvme_maxphys,
    .size = nvme_unit_size,
    .flush = nvme_unit_flush,
    .ident = nvme_ident,
};

static struct driver nvme_driver = {
    .name = "nvme",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_BLOCK,
    .probe = nvme_probe_device,
    .attach = nvme_attach_device,
    .intr = nvme_intr,
    .shutdown = nvme_shutdown,
    .suspend = nvme_suspend,
    .resume = nvme_resume,
};

void nvme_driver_init(void) {
  driver_register(&nvme_driver);
  nvme_major = blockdev_register(&nvme_blockdrv);
}

uint64_t nvme_size(uint unit) {
  struct nvme_ns *ns = unit_ns(unit);

  return ns != 0 ? ns->sector_count : 0;
}

static void nvme_prp(struct nvme_tracker *t, struct nvme_sqe *cmd, void *buf,
                     uint bytes) {
  uint64_t phys = (uint64_t)V2P(buf), page = phys & ~(uint64_t)(PGSIZE - 1);
  uint first = PGSIZE - (uint)(phys & (PGSIZE - 1)), i, n;

  cmd->prp1 = phys;
  cmd->prp2 = 0;
  if (bytes <= first)
    return;
  n = (bytes - first + PGSIZE - 1) / PGSIZE;
  if (n == 1) {
    cmd->prp2 = page + PGSIZE;
    return;
  }
  for (i = 0; i < n; i++)
    t->prpl[i] = page + (uint64_t)(i + 1) * PGSIZE;
  cmd->prp2 = (uint64_t)V2P(t->prpl);
}

static int nvme_io(struct nvme_ns *ns, uint8_t opcode, uint64_t lba, uint count,
                   void *buf) {
  struct nvme_queue *q = &ns->sc->io_q;
  struct nvme_sqe cmd;
  struct nvme_tracker *t;

  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = opcode;
  cmd.nsid = (uint32_t)ns->nsid;
  if ((t = nvme_slot(q, &cmd.cid)) == 0)
    return -1;
  nvme_prp(t, &cmd, buf, count * NVME_SECTOR_SIZE);
  cmd.cdw10 = (uint32_t)lba;
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = (count - 1);
  return nvme_submit(q, t, &cmd, 0, NVME_TIMO_IO);
}

void nvmerw(struct buf *b) {
  struct nvme_ns *ns = unit_ns(minor(b->dev));
  uint sectors = bufcount(b) / NVME_SECTOR_SIZE;
  uint64_t lba = b->blockno * (BSIZE / NVME_SECTOR_SIZE);

  if (ns == 0) {
    b->flags |= B_ERROR;
    return;
  }
  if (b->flags & B_DIRTY) {
    if (nvme_io(ns, NVME_NVM_WRITE, lba, sectors, bufdata(b)) < 0) {
      b->flags |= B_ERROR;
      return;
    }
    ns->dirty = 1;
    b->flags &= ~B_DIRTY;
  } else {
    if (nvme_io(ns, NVME_NVM_READ, lba, sectors, bufdata(b)) < 0) {
      b->flags |= B_ERROR;
      return;
    }
    b->flags |= B_VALID;
  }
}

static int nvme_unit_flush(uint unit) {
  struct nvme_ns *ns = unit_ns(unit);
  struct nvme_sqe cmd;
  struct nvme_tracker *t;

  if (ns == 0)
    return -1;
  if (!ns->dirty)
    return 0;
  ns->dirty = 0;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_NVM_FLUSH;
  cmd.nsid = (uint32_t)ns->nsid;
  t = nvme_slot(&ns->sc->io_q, &cmd.cid);
  if (t == 0 || nvme_submit(&ns->sc->io_q, t, &cmd, 0, NVME_TIMO_FLUSH) < 0) {
    ns->dirty = 1;
    return -1;
  }
  return 0;
}
