#include "nvme.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "spinlock.h"
#include "x86.h"

struct nvme_queue {
  struct nvme_sqe *sq;
  struct nvme_cqe *cq;
  void *sq_virt;
  void *cq_virt;
  uint16_t sq_tail;
  uint16_t cq_head;
  uint8_t  cq_phase;
  uint16_t qid;
  uint16_t size;
  volatile uint32_t *sq_db;
  volatile uint32_t *cq_db;
  struct spinlock lock;
};

struct nvme_namespace_state {
  uint64_t sector_count;
  uint32_t sector_size;
  int present;
  int nsid;
};

static struct {
  struct spinlock lock;
  volatile uint32_t *mmio;
  struct device *dev;
  uint32_t db_stride;
  struct nvme_queue admin_q;
  struct nvme_queue io_q;
  struct nvme_namespace_state namespaces[NVME_MAX_NAMESPACES];
  int active_namespace_count;
  int initialized;
} nvme_state;

static inline uint32_t nvme_read32(uint32_t reg) {
  return *(volatile uint32_t *)((uintptr_t)nvme_state.mmio + reg);
}

static inline void nvme_write32(uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)((uintptr_t)nvme_state.mmio + reg) = val;
}

static inline uint64_t nvme_read64(uint32_t reg) {
  uint32_t low = nvme_read32(reg);
  uint32_t high = nvme_read32(reg + 4);
  return (uint64_t)low | ((uint64_t)high << 32);
}

static inline void nvme_write64(uint32_t reg, uint64_t val) {
  nvme_write32(reg, (uint32_t)val);
  nvme_write32(reg + 4, (uint32_t)(val >> 32));
}

static int nvme_submit_admin_cmd(struct nvme_sqe *cmd, struct nvme_cqe *resp) {
  struct nvme_queue *q = &nvme_state.admin_q;
  acquire(&q->lock);

  uint16_t tail = q->sq_tail;
  cmd->cid = tail;
  q->sq[tail] = *cmd;

  tail = (tail + 1) % q->size;
  q->sq_tail = tail;
  *q->sq_db = tail;

  int spin;
  volatile struct nvme_cqe *cqe = &q->cq[q->cq_head];
  for (spin = 0; spin < 500000; spin++) {
    if ((cqe->status & 1) == q->cq_phase)
      break;
    delay(10);
  }

  if (spin == 500000) {
    cprintf("NVMe: admin command timed out\n");
    release(&q->lock);
    return -1;
  }

  if (resp)
    *resp = *cqe;

  uint16_t status = cqe->status >> 1;
  q->cq_head = (q->cq_head + 1) % q->size;
  if (q->cq_head == 0)
    q->cq_phase ^= 1;

  *q->cq_db = q->cq_head;
  release(&q->lock);

  return status == 0 ? 0 : -1;
}

static int nvme_submit_io_cmd(struct nvme_sqe *cmd, struct nvme_cqe *resp) {
  struct nvme_queue *q = &nvme_state.io_q;
  acquire(&q->lock);

  uint16_t tail = q->sq_tail;
  cmd->cid = tail;
  q->sq[tail] = *cmd;

  tail = (tail + 1) % q->size;
  q->sq_tail = tail;
  *q->sq_db = tail;

  int spin;
  volatile struct nvme_cqe *cqe = &q->cq[q->cq_head];
  for (spin = 0; spin < 500000; spin++) {
    if ((cqe->status & 1) == q->cq_phase)
      break;
    delay(10);
  }

  if (spin == 500000) {
    cprintf("NVMe: IO command timed out\n");
    release(&q->lock);
    return -1;
  }

  if (resp)
    *resp = *cqe;

  uint16_t status = cqe->status >> 1;
  q->cq_head = (q->cq_head + 1) % q->size;
  if (q->cq_head == 0)
    q->cq_phase ^= 1;

  *q->cq_db = q->cq_head;
  release(&q->lock);

  return status == 0 ? 0 : -1;
}

static int nvme_init_queue(struct nvme_queue *q, uint16_t qid, uint16_t size,
                           uint32_t db_stride) {
  q->qid = qid;
  q->size = size;
  q->sq_tail = 0;
  q->cq_head = 0;
  q->cq_phase = 1;
  initlock(&q->lock, "nvme_q");

  void *sq = kalloc();
  void *cq = kalloc();
  if (!sq || !cq) {
    if (sq)
      kfree(sq);
    if (cq)
      kfree(cq);
    return -1;
  }
  memset(sq, 0, PGSIZE);
  memset(cq, 0, PGSIZE);

  q->sq_virt = sq;
  q->cq_virt = cq;
  q->sq = (struct nvme_sqe *)sq;
  q->cq = (struct nvme_cqe *)cq;

  uintptr_t db_base = (uintptr_t)nvme_state.mmio + NVME_REG_DBS;
  q->sq_db = (volatile uint32_t *)(db_base + (2 * qid) * db_stride);
  q->cq_db = (volatile uint32_t *)(db_base + (2 * qid + 1) * db_stride);

  return 0;
}

static int nvme_probe_device(struct device *dev) {
  if (dev->pci.class_code == PCI_CLASS_STORAGE &&
      dev->pci.subclass == PCI_SUBCLASS_NVME &&
      dev->pci.prog_if == PCI_PROGIF_NVME) {
    return 100;
  }
  return 0;
}

static int nvme_attach_device(struct device *dev) {
  uint64_t bar0_phys = dev->pci.bar[0];
  if (bar0_phys == 0 || dev->pci.bar_is_io[0]) {
    cprintf("NVMe: invalid BAR0 MMIO\n");
    return -1;
  }

  volatile uint32_t *regs = (volatile uint32_t *)ioremap(bar0_phys, 0x2000);
  initlock(&nvme_state.lock, "nvme");
  nvme_state.mmio = regs;
  nvme_state.dev = dev;

  uint64_t cap = nvme_read64(NVME_REG_CAP);
  uint32_t dstrd = (uint32_t)((cap >> 32) & 0x0F);
  nvme_state.db_stride = 4 << dstrd;

  // Reset controller if enabled
  uint32_t csts = nvme_read32(NVME_REG_CSTS);
  if (csts & NVME_CSTS_RDY) {
    nvme_write32(NVME_REG_CC, 0);
    for (int i = 0; i < 500 && (nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY);
         i++)
      delay(100);
  }

  // Setup Admin Queues
  if (nvme_init_queue(&nvme_state.admin_q, 0, NVME_ADMIN_Q_SIZE,
                      nvme_state.db_stride) < 0) {
    cprintf("NVMe: failed to allocate admin queues\n");
    return -1;
  }

  nvme_write32(NVME_REG_AQA,
               ((NVME_ADMIN_Q_SIZE - 1) << 16) | (NVME_ADMIN_Q_SIZE - 1));
  nvme_write64(NVME_REG_ASQ, V2P(nvme_state.admin_q.sq_virt));
  nvme_write64(NVME_REG_ACQ, V2P(nvme_state.admin_q.cq_virt));

  // Enable Controller
  uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_IOCQES |
                NVME_CC_IOSQES;
  nvme_write32(NVME_REG_CC, cc);

  for (int i = 0; i < 500 && !(nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY); i++)
    delay(100);

  if (!(nvme_read32(NVME_REG_CSTS) & NVME_CSTS_RDY)) {
    cprintf("NVMe: controller failed to become ready\n");
    return -1;
  }

  // Setup I/O Queues
  if (nvme_init_queue(&nvme_state.io_q, 1, NVME_IO_Q_SIZE,
                      nvme_state.db_stride) < 0) {
    cprintf("NVMe: failed to allocate IO queues\n");
    return -1;
  }

  // Create I/O Completion Queue
  struct nvme_sqe cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_CREATE_IO_CQ;
  cmd.prp1 = (uint64_t)V2P(nvme_state.io_q.cq_virt);
  cmd.cdw10 = ((NVME_IO_Q_SIZE - 1) << 16) | 1; // QID 1, size
  cmd.cdw11 = 1; // Physically contiguous, interrupts enabled
  if (nvme_submit_admin_cmd(&cmd, 0) < 0) {
    cprintf("NVMe: failed to create IO completion queue\n");
    return -1;
  }

  // Create I/O Submission Queue
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_ADMIN_CREATE_IO_SQ;
  cmd.prp1 = (uint64_t)V2P(nvme_state.io_q.sq_virt);
  cmd.cdw10 = ((NVME_IO_Q_SIZE - 1) << 16) | 1; // QID 1, size
  cmd.cdw11 = (1 << 16) | 1; // CQID 1, physically contiguous
  if (nvme_submit_admin_cmd(&cmd, 0) < 0) {
    cprintf("NVMe: failed to create IO submission queue\n");
    return -1;
  }

  // Identify Namespace 1
  char *ident_buf = kalloc();
  if (ident_buf) {
    memset(ident_buf, 0, PGSIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)V2P(ident_buf);
    cmd.cdw10 = 0; // CNS 0 = Identify Namespace

    if (nvme_submit_admin_cmd(&cmd, 0) == 0) {
      uint64_t nsze = *(uint64_t *)(ident_buf + 0); // Namespace size in blocks
      uint8_t flbas = *(uint8_t *)(ident_buf + 26);
      uint8_t lbads = *(uint8_t *)(ident_buf + 128 + (flbas & 0x0F) * 4 + 2);
      uint32_t sector_size = (1U << (lbads ? lbads : 9));

      nvme_state.namespaces[0].nsid = 1;
      nvme_state.namespaces[0].sector_count = nsze;
      nvme_state.namespaces[0].sector_size = sector_size;
      nvme_state.namespaces[0].present = (nsze > 0);
      nvme_state.active_namespace_count = 1;

      cprintf("NVMe: Namespace 1 ready, %u MB (%lu sectors of %u bytes)\n",
              (uint)(nsze * sector_size / (1024 * 1024)), (uint64_t)nsze,
              (uint)sector_size);
    }
    kfree(ident_buf);
  }

  safestrcpy(dev->name, "nvme0", sizeof(dev->name));
  nvme_state.initialized = 1;
  cprintf("NVMe: PCIe controller initialized\n");
  return 0;
}

static void nvme_intr(struct device *dev) {
  // NVMe interrupts are acknowledged during completion queue polling.
  // In interrupt mode, mask/unmask can be updated here.
}

// NVMe 1.4 sec 7.6.2, as FreeBSD nvme_ctrlr_shutdown does it. a controller
// still fetching commands writes into memory that is no longer ours.
static void nvme_shutdown(struct device *dev) {
  uint32_t cc, csts;
  int i;

  if (!nvme_state.initialized || nvme_state.mmio == 0)
    return;

  cc = nvme_read32(NVME_REG_CC);
  cc = (cc & ~NVME_CC_SHN_MASK) | NVME_CC_SHN_NORMAL;
  nvme_write32(NVME_REG_CC, cc);

  for (i = 0; i < 5000; i++) {
    csts = nvme_read32(NVME_REG_CSTS);
    if (csts == 0xffffffffu)
      return; // the drive left
    if ((csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_DONE)
      return;
    delay(1000);
  }
  cprintf("NVMe: controller did not finish its shutdown\n");
}

void nvme_driver_init(void) {
  struct driver drv;
  memset(&drv, 0, sizeof(drv));
  safestrcpy(drv.name, "nvme-ssd", sizeof(drv.name));
  drv.bus_type = BUS_TYPE_PCI;
  drv.dev_type = DEVICE_TYPE_BLOCK;
  drv.probe = nvme_probe_device;
  drv.attach = nvme_attach_device;
  drv.intr = nvme_intr;
  drv.shutdown = nvme_shutdown;
  driver_register(&drv);
}

void nvme_init(void) {
  struct pci_device pci;
  if (pci_find_device_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_NVME,
                               PCI_PROGIF_NVME, &pci) == 0) {
    struct device dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus = BUS_TYPE_PCI;
    dev.pci = pci;
    pci_enable_bus_master(&pci);
    nvme_attach_device(&dev);
  }
}

uint64_t nvme_size(uint ns_id) {
  if (!nvme_state.initialized || ns_id >= NVME_MAX_NAMESPACES)
    return 0;
  return nvme_state.namespaces[ns_id].present
             ? nvme_state.namespaces[ns_id].sector_count
             : 0;
}

int nvme_read(uint ns_id, uint64_t lba, uint count, void *dst) {
  if (!nvme_state.initialized || ns_id >= NVME_MAX_NAMESPACES ||
      !nvme_state.namespaces[ns_id].present)
    return -1;

  struct nvme_sqe cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_NVM_READ;
  cmd.nsid = nvme_state.namespaces[ns_id].nsid;
  cmd.prp1 = (uint64_t)V2P(dst);
  cmd.cdw10 = (uint32_t)lba;
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = (count - 1); // 0-based sector count

  return nvme_submit_io_cmd(&cmd, 0);
}

int nvme_write(uint ns_id, uint64_t lba, uint count, const void *src) {
  if (!nvme_state.initialized || ns_id >= NVME_MAX_NAMESPACES ||
      !nvme_state.namespaces[ns_id].present)
    return -1;

  struct nvme_sqe cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.opcode = NVME_NVM_WRITE;
  cmd.nsid = nvme_state.namespaces[ns_id].nsid;
  cmd.prp1 = (uint64_t)V2P(src);
  cmd.cdw10 = (uint32_t)lba;
  cmd.cdw11 = (uint32_t)(lba >> 32);
  cmd.cdw12 = (count - 1);

  return nvme_submit_io_cmd(&cmd, 0);
}

void nvmerw(struct buf *b) {
  uint ns = b->dev >= 20 ? b->dev - 20 : b->dev;
  if (ns >= NVME_MAX_NAMESPACES || !nvme_state.namespaces[ns].present)
    panic("nvmerw: invalid NVMe namespace");

  uint sectors_per_block = BSIZE / NVME_SECTOR_SIZE;
  uint64_t lba = (uint64_t)b->blockno * sectors_per_block;

  if (b->flags & B_DIRTY) {
    if (nvme_write(ns, lba, sectors_per_block, b->data) < 0)
      panic("nvmerw: write failed");
    b->flags &= ~B_DIRTY;
  } else {
    if (nvme_read(ns, lba, sectors_per_block, b->data) < 0)
      panic("nvmerw: read failed");
    b->flags |= B_VALID;
  }
}
