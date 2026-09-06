#include "ahci.h"
#include "blockdev.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "spinlock.h"
#include "x86.h"

struct ahci_port_state {
  struct hba_port *port_regs;
  struct hba_cmd_header *cmd_headers;
  void *cmd_headers_virt;
  void *fis_virt;
  struct hba_cmd_tbl *cmd_tbl[AHCI_NUM_SLOTS];
  void *tbl_pages[2];
  uint64_t sector_count;
  int present;
  int port_no;
  int sata_gen;
  char model[41];
  char serial[21];
  struct spinlock lock;
  struct buf *queue;
  int busy;
};

static struct {
  struct spinlock lock;
  struct hba_mem *hba;
  struct device *dev;
  struct ahci_port_state ports[AHCI_MAX_PORTS];
  int active_port_count;
  int initialized;
} ahci_state;

static void ahci_stop_port(struct hba_port *port) {
  port->cmd &= ~HBA_PxCMD_ST;
  port->cmd &= ~HBA_PxCMD_FRE;

  for (int i = 0; i < 500; i++) {
    if ((port->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR)) == 0)
      break;
    microdelay(100);
  }
}

static void ahci_start_port(struct hba_port *port) {
  while (port->cmd & HBA_PxCMD_CR)
    microdelay(10);
  port->cmd |= HBA_PxCMD_FRE;
  port->cmd |= HBA_PxCMD_ST;
}

// Reset port via SATA PHY COMRESET sequence
static int ahci_reset_port(struct hba_port *port) {
  ahci_stop_port(port);

  // Assert COMRESET
  port->sctl = (port->sctl & ~0x0F) | 1;
  microdelay(1000); // 1 ms
  port->sctl = (port->sctl & ~0x0F);

  int spin;
  for (spin = 0; spin < 1000; spin++) {
    if ((port->ssts & 0x0F) == HBA_PORT_DET_PRESENT)
      break;
    microdelay(100);
  }

  port->serr = 0xFFFFFFFF;
  port->is = 0xFFFFFFFF;

  ahci_start_port(port);
  return ((port->ssts & 0x0F) == HBA_PORT_DET_PRESENT) ? 0 : -1;
}

// Build Scatter-Gather PRDT entries for an arbitrary buffer (handling page boundaries)
static int ahci_setup_prdt(struct hba_cmd_tbl *tbl, void *buf, uint bytes) {
  uintptr_t vaddr = (uintptr_t)buf;
  int prdt_idx = 0;

  while (bytes > 0 && prdt_idx < AHCI_PRDT_PER_CMD) {
    uintptr_t paddr = V2P(vaddr);
    uint bytes_in_page = PGSIZE - (vaddr % PGSIZE);
    uint chunk = (bytes < bytes_in_page) ? bytes : bytes_in_page;

    tbl->prdt[prdt_idx].dba = (uint32_t)paddr;
    tbl->prdt[prdt_idx].dbau = (uint32_t)((uint64_t)paddr >> 32);
    tbl->prdt[prdt_idx].dbc = chunk - 1; // 0-based byte count
    tbl->prdt[prdt_idx].i = (chunk == bytes) ? 1 : 0; // Interrupt on completion of final PRDT entry
    tbl->prdt[prdt_idx].rsv0 = 0;
    tbl->prdt[prdt_idx].rsv1 = 0;

    vaddr += chunk;
    bytes -= chunk;
    prdt_idx++;
  }

  if (bytes > 0)
    return -1; // Buffer too large or too fragmented for PRDT table

  return prdt_idx;
}

// Start DMA transfer for a buffer from the port queue (uses slot 0)
static int ahci_start_buf(struct ahci_port_state *ps, struct buf *b) {
  int slot = 0;
  struct hba_port *port = ps->port_regs;
  struct hba_cmd_tbl *cmdtbl = ps->cmd_tbl[slot];
  struct hba_cmd_header *cmdheader = &ps->cmd_headers[slot];

  int is_write = (b->flags & B_DIRTY) ? 1 : 0;
  uint sectors_per_block = BSIZE / AHCI_SECTOR_SIZE;
  uint64_t lba = (uint64_t)b->blockno * sectors_per_block;

  memset(cmdtbl, 0, sizeof(struct hba_cmd_tbl));
  int prdt_count = ahci_setup_prdt(cmdtbl, b->data, BSIZE);
  if (prdt_count < 0) {
    cprintf("AHCI: failed to setup PRDT for block %d\n", b->blockno);
    return -1;
  }

  cmdheader->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
  cmdheader->w = is_write ? 1 : 0;
  cmdheader->a = 0;
  cmdheader->p = 0;
  cmdheader->r = 0;
  cmdheader->b = 0;
  cmdheader->c = 1;
  cmdheader->pmp = 0;
  cmdheader->prdtl = prdt_count;
  cmdheader->prdbc = 0;

  struct fis_reg_h2d *cmdfis = (struct fis_reg_h2d *)(&cmdtbl->cfis[0]);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;

  cmdfis->lba0 = (uint8_t)lba;
  cmdfis->lba1 = (uint8_t)(lba >> 8);
  cmdfis->lba2 = (uint8_t)(lba >> 16);
  cmdfis->device = 1 << 6; // LBA mode

  cmdfis->lba3 = (uint8_t)(lba >> 24);
  cmdfis->lba4 = (uint8_t)(lba >> 32);
  cmdfis->lba5 = (uint8_t)(lba >> 40);

  cmdfis->countl = (uint8_t)(sectors_per_block & 0xff);
  cmdfis->counth = (uint8_t)((sectors_per_block >> 8) & 0xff);

  ps->busy = 1;
  port->ci = (1U << slot);
  return 0;
}

// Complete active queued buffer on slot 0 (must be called with ps->lock held)
static void ahci_check_port_completion(struct ahci_port_state *ps) {
  struct buf *b = ps->queue;
  if (!b)
    return;

  struct hba_port *port = ps->port_regs;
  int slot = 0;

  if ((port->ci & (1U << slot)) == 0) {
    uint32_t is = port->is;
    if ((is & HBA_PxIS_ERRORS) || (port->tfd & 0x01)) {
      cprintf("AHCI: port %d I/O error (is 0x%x, tfd 0x%x, serr 0x%x)\n",
              ps->port_no, is, port->tfd, port->serr);
      port->serr = 0xFFFFFFFF;
      port->is = is;
    }

    ps->queue = b->qnext;
    ps->busy = 0;

    b->flags |= B_VALID;
    b->flags &= ~B_DIRTY;
    wakeup(b);

    if (ps->queue != 0) {
      ahci_start_buf(ps, ps->queue);
    }
  }
}

// Synchronous command execution for direct ahci_read/ahci_write calls (uses slot 31)
static int ahci_issue_command_sync(struct ahci_port_state *ps, int is_write,
                                  uint64_t lba, uint count, void *buf) {
  int slot = 31; // Dedicated slot for direct synchronous transfers
  struct hba_port *port = ps->port_regs;
  struct hba_cmd_header *cmdheader = &ps->cmd_headers[slot];
  struct hba_cmd_tbl *cmdtbl = ps->cmd_tbl[slot];

  memset(cmdtbl, 0, sizeof(struct hba_cmd_tbl));
  int prdt_count = ahci_setup_prdt(cmdtbl, buf, count * AHCI_SECTOR_SIZE);
  if (prdt_count < 0)
    return -1;

  cmdheader->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
  cmdheader->w = is_write ? 1 : 0;
  cmdheader->a = 0;
  cmdheader->p = 0;
  cmdheader->r = 0;
  cmdheader->b = 0;
  cmdheader->c = 1;
  cmdheader->pmp = 0;
  cmdheader->prdtl = prdt_count;
  cmdheader->prdbc = 0;

  struct fis_reg_h2d *cmdfis = (struct fis_reg_h2d *)(&cmdtbl->cfis[0]);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;

  cmdfis->lba0 = (uint8_t)lba;
  cmdfis->lba1 = (uint8_t)(lba >> 8);
  cmdfis->lba2 = (uint8_t)(lba >> 16);
  cmdfis->device = 1 << 6;

  cmdfis->lba3 = (uint8_t)(lba >> 24);
  cmdfis->lba4 = (uint8_t)(lba >> 32);
  cmdfis->lba5 = (uint8_t)(lba >> 40);

  cmdfis->countl = (uint8_t)(count & 0xff);
  cmdfis->counth = (uint8_t)((count >> 8) & 0xff);

  port->ci = (1U << slot);

  int spin;
  for (spin = 0; spin < 500000; spin++) {
    if ((port->ci & (1U << slot)) == 0)
      break;
    if ((port->is & HBA_PxIS_ERRORS) || (port->tfd & 0x01)) {
      cprintf("AHCI: sync command error on port %d\n", ps->port_no);
      port->serr = 0xFFFFFFFF;
      port->is = port->is;
      return -1;
    }
    microdelay(10);
  }

  if (spin == 500000) {
    cprintf("AHCI: port %d sync command timed out\n", ps->port_no);
    return -1;
  }

  return 0;
}

// Identify drive: parse geometry, LBA48 support, serial number, model name, SATA generation
static uint64_t ahci_identify(struct ahci_port_state *ps) {
  int slot = 31;
  struct hba_port *port = ps->port_regs;

  char *ident_buf = kalloc();
  if (!ident_buf)
    return 0;
  memset(ident_buf, 0, PGSIZE);

  struct hba_cmd_header *cmdheader = &ps->cmd_headers[slot];
  cmdheader->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
  cmdheader->w = 0;
  cmdheader->a = 0;
  cmdheader->p = 0;
  cmdheader->r = 0;
  cmdheader->b = 0;
  cmdheader->c = 1;
  cmdheader->pmp = 0;
  cmdheader->prdtl = 1;
  cmdheader->prdbc = 0;

  struct hba_cmd_tbl *cmdtbl = ps->cmd_tbl[slot];
  memset(cmdtbl, 0, sizeof(struct hba_cmd_tbl));

  uintptr_t buf_phys = V2P(ident_buf);
  cmdtbl->prdt[0].dba = (uint32_t)buf_phys;
  cmdtbl->prdt[0].dbau = (uint32_t)((uint64_t)buf_phys >> 32);
  cmdtbl->prdt[0].dbc = 512 - 1;
  cmdtbl->prdt[0].i = 1;

  struct fis_reg_h2d *cmdfis = (struct fis_reg_h2d *)(&cmdtbl->cfis[0]);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = ATA_CMD_IDENTIFY;
  cmdfis->device = 0;

  port->ci = (1U << slot);

  int spin;
  for (spin = 0; spin < 100000; spin++) {
    if ((port->ci & (1U << slot)) == 0)
      break;
    microdelay(10);
  }

  uint64_t sectors = 0;
  if (spin < 100000 && !(port->is & HBA_PxIS_TFES)) {
    uint16_t *words = (uint16_t *)ident_buf;

    // Decode Model (words 27..46, byte swapped)
    char *model_src = (char *)&words[27];
    for (int i = 0; i < 40; i += 2) {
      ps->model[i] = model_src[i + 1];
      ps->model[i + 1] = model_src[i];
    }
    ps->model[40] = '\0';
    for (int i = 39; i >= 0 && ps->model[i] == ' '; i--)
      ps->model[i] = '\0';

    // Decode Serial Number (words 10..19, byte swapped)
    char *serial_src = (char *)&words[10];
    for (int i = 0; i < 20; i += 2) {
      ps->serial[i] = serial_src[i + 1];
      ps->serial[i + 1] = serial_src[i];
    }
    ps->serial[20] = '\0';
    for (int i = 19; i >= 0 && ps->serial[i] == ' '; i--)
      ps->serial[i] = '\0';

    // Check LBA48 support (word 83 bit 10)
    if (words[83] & (1 << 10)) {
      sectors = (uint64_t)words[100] |
                ((uint64_t)words[101] << 16) |
                ((uint64_t)words[102] << 32) |
                ((uint64_t)words[103] << 48);
    } else {
      sectors = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
    }

    // Negotiated SATA interface speed
    ps->sata_gen = (port->ssts >> 4) & 0x0F;
  }

  kfree(ident_buf);
  return sectors;
}

// Initialize port data structures, command headers, and command tables
static int ahci_init_port(struct ahci_port_state *ps, int port_no, struct hba_port *port) {
  ps->port_no = port_no;
  ps->port_regs = port;
  ps->queue = 0;
  ps->busy = 0;
  initlock(&ps->lock, "ahci_port");

  ahci_stop_port(port);

  void *clb = kalloc();
  if (!clb)
    return -1;
  memset(clb, 0, PGSIZE);
  ps->cmd_headers_virt = clb;
  ps->cmd_headers = (struct hba_cmd_header *)clb;

  port->clb = (uint32_t)V2P(clb);
  port->clbu = (uint32_t)((uint64_t)V2P(clb) >> 32);

  void *fb = kalloc();
  if (!fb) {
    kfree(clb);
    return -1;
  }
  memset(fb, 0, PGSIZE);
  ps->fis_virt = fb;

  port->fb = (uint32_t)V2P(fb);
  port->fbu = (uint32_t)((uint64_t)V2P(fb) >> 32);

  // Allocate 2 contiguous pages for all 32 256-byte Command Tables
  void *tbl0 = kalloc();
  void *tbl1 = kalloc();
  if (!tbl0 || !tbl1) {
    if (tbl0) kfree(tbl0);
    if (tbl1) kfree(tbl1);
    kfree(clb);
    kfree(fb);
    return -1;
  }
  memset(tbl0, 0, PGSIZE);
  memset(tbl1, 0, PGSIZE);
  ps->tbl_pages[0] = tbl0;
  ps->tbl_pages[1] = tbl1;

  for (int s = 0; s < AHCI_NUM_SLOTS; s++) {
    struct hba_cmd_tbl *tbl = (s < 16)
        ? (struct hba_cmd_tbl *)((uintptr_t)tbl0 + s * sizeof(struct hba_cmd_tbl))
        : (struct hba_cmd_tbl *)((uintptr_t)tbl1 + (s - 16) * sizeof(struct hba_cmd_tbl));
    ps->cmd_tbl[s] = tbl;
    ps->cmd_headers[s].ctba = (uint32_t)V2P(tbl);
    ps->cmd_headers[s].ctbau = (uint32_t)((uint64_t)V2P(tbl) >> 32);
  }

  // Clear pending errors and interrupts
  port->serr = 0xFFFFFFFF;
  port->is = 0xFFFFFFFF;

  // Unmask port interrupts: D2H register, PIO, DMA, Set Device Bits, Descriptor, Errors
  port->ie = HBA_PxIS_DHRS | HBA_PxIS_PSS | HBA_PxIS_DSS | HBA_PxIS_SDBS |
             HBA_PxIS_DPS | HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_IFS;

  ahci_start_port(port);

  ps->sector_count = ahci_identify(ps);
  ps->present = (ps->sector_count > 0);

  if (ps->present) {
    const char *speed = "SATA-I (1.5 Gbps)";
    if (ps->sata_gen == 2) speed = "SATA-II (3.0 Gbps)";
    else if (ps->sata_gen == 3) speed = "SATA-III (6.0 Gbps)";

    cprintf("AHCI: port %d [%s] Model '%s' Serial '%s', %d MB (%d sectors)\n",
            port_no, speed,
            ps->model[0] ? ps->model : "Generic ATA",
            ps->serial[0] ? ps->serial : "N/A",
            (uint)(ps->sector_count * AHCI_SECTOR_SIZE / (1024 * 1024)),
            (uint)ps->sector_count);
  }

  return 0;
}

static int ahci_probe_device(struct device *dev) {
  if (dev->pci.class_code == PCI_CLASS_STORAGE &&
      dev->pci.subclass == PCI_SUBCLASS_SATA &&
      dev->pci.prog_if == PCI_PROGIF_SATA_AHCI) {
    return 100;
  }
  return 0;
}

static int ahci_attach_device(struct device *dev) {
  uint32_t abar_phys = dev->pci.bar[5];
  if (abar_phys == 0 || dev->pci.bar_is_io[5]) {
    cprintf("AHCI: invalid ABAR in BAR5\n");
    return -1;
  }

  safestrcpy(dev->name, "ahci0", sizeof(dev->name));

  struct hba_mem *hba = (struct hba_mem *)DEVSPACE_P2V(abar_phys);
  initlock(&ahci_state.lock, "ahci");
  ahci_state.hba = hba;
  ahci_state.dev = dev;

  // BIOS/OS handoff if supported by controller
  if (hba->cap2 & (1U << 0)) {
    hba->bohc |= (1U << 1); // OS Ownership
    for (int i = 0; i < 500 && (hba->bohc & (1U << 0)); i++)
      microdelay(100);
  }

  // Enable AHCI mode and global interrupts
  hba->ghc |= HBA_GHC_AE;
  hba->ghc |= HBA_GHC_IE;

  uint32_t pi = hba->pi;
  ahci_state.active_port_count = 0;

  for (int p = 0; p < AHCI_MAX_PORTS; p++) {
    if (pi & (1U << p)) {
      struct hba_port *port = &hba->ports[p];
      uint32_t ssts = port->ssts;
      uint8_t ipm = (ssts >> 8) & 0x0F;
      uint8_t det = ssts & 0x0F;

      if (det == HBA_PORT_DET_PRESENT && ipm == HBA_PORT_IPM_ACTIVE) {
        if (port->sig == SATA_SIG_ATA) {
          if (ahci_init_port(&ahci_state.ports[p], p, port) == 0 &&
              ahci_state.ports[p].present) {
            ahci_state.active_port_count++;
          }
        }
      }
    }
  }

  ahci_state.initialized = 1;
  cprintf("AHCI: controller initialized with %d active SATA drive(s)\n",
          ahci_state.active_port_count);
  return 0;
}

// Hardware interrupt handler dispatched by driver framework
void ahci_intr(struct device *dev) {
  if (!ahci_state.hba)
    return;

  uint32_t is = ahci_state.hba->is;
  ahci_state.hba->is = is; // Acknowledge global HBA interrupt

  for (int p = 0; p < AHCI_MAX_PORTS; p++) {
    if (is & (1U << p)) {
      struct ahci_port_state *ps = &ahci_state.ports[p];
      if (!ps->present)
        continue;
      acquire(&ps->lock);
      uint32_t port_is = ps->port_regs->is;
      ps->port_regs->is = port_is; // Acknowledge port interrupt
      ahci_check_port_completion(ps);
      release(&ps->lock);
    }
  }
}

// Fallback timer polling callback registered in driver framework
void ahci_poll(struct device *dev) {
  if (!ahci_state.hba)
    return;

  for (int p = 0; p < AHCI_MAX_PORTS; p++) {
    struct ahci_port_state *ps = &ahci_state.ports[p];
    if (ps->present && ps->queue != 0) {
      acquire(&ps->lock);
      ahci_check_port_completion(ps);
      release(&ps->lock);
    }
  }
}

void ahci_driver_init(void) {
  struct driver drv;
  memset(&drv, 0, sizeof(drv));
  safestrcpy(drv.name, "ahci-sata", sizeof(drv.name));
  drv.bus_type = BUS_TYPE_PCI;
  drv.dev_type = DEVICE_TYPE_BLOCK;
  drv.probe = ahci_probe_device;
  drv.attach = ahci_attach_device;
  drv.intr = ahci_intr;
  drv.poll = ahci_poll;
  driver_register(&drv);
}

void ahci_init(void) {
  struct pci_device pci;
  if (pci_find_device_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_SATA_AHCI, &pci) == 0) {
    struct device dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus = BUS_TYPE_PCI;
    dev.pci = pci;
    pci_enable_bus_master(&pci);
    ahci_attach_device(&dev);
  }
}

uint64_t ahci_size(uint port) {
  if (!ahci_state.initialized || port >= AHCI_MAX_PORTS)
    return 0;
  return ahci_state.ports[port].present ? ahci_state.ports[port].sector_count : 0;
}

int ahci_read(uint port, uint64_t lba, uint count, void *dst) {
  if (!ahci_state.initialized || port >= AHCI_MAX_PORTS || !ahci_state.ports[port].present)
    return -1;
  struct ahci_port_state *ps = &ahci_state.ports[port];
  acquire(&ps->lock);
  int res = ahci_issue_command_sync(ps, 0, lba, count, dst);
  release(&ps->lock);
  return res;
}

int ahci_write(uint port, uint64_t lba, uint count, const void *src) {
  if (!ahci_state.initialized || port >= AHCI_MAX_PORTS || !ahci_state.ports[port].present)
    return -1;
  struct ahci_port_state *ps = &ahci_state.ports[port];
  acquire(&ps->lock);
  int res = ahci_issue_command_sync(ps, 1, lba, count, (void *)src);
  release(&ps->lock);
  return res;
}

// Asynchronous I/O routing for kernel buffer cache (bread/bwrite)
void ahcirw(struct buf *b) {
  uint port_no = (b->dev >= DEV_SATA_START) ? (b->dev - DEV_SATA_START) : b->dev;
  if (port_no >= AHCI_MAX_PORTS || !ahci_state.ports[port_no].present)
    panic("ahcirw: invalid SATA drive");

  if (!holdingsleep(&b->lock))
    panic("ahcirw: buf not locked");
  if ((b->flags & (B_VALID | B_DIRTY)) == B_VALID)
    panic("ahcirw: nothing to do");

  struct ahci_port_state *ps = &ahci_state.ports[port_no];
  acquire(&ps->lock);

  b->qnext = 0;
  struct buf **cursor;
  for (cursor = &ps->queue; *cursor; cursor = &(*cursor)->qnext)
    ;
  *cursor = b;

  if (ps->queue == b)
    ahci_start_buf(ps, b);

  if (myproc() != 0) {
    while ((b->flags & (B_VALID | B_DIRTY)) != B_VALID) {
      sleep(b, &ps->lock);
    }
  } else {
    // Early boot before process scheduler starts
    for (int spin = 0; spin < 500000; spin++) {
      if ((b->flags & (B_VALID | B_DIRTY)) == B_VALID)
        break;
      ahci_check_port_completion(ps);
      microdelay(10);
    }
  }

  release(&ps->lock);
}

