#include "ahci.h"
#include "inc/abi.h"
#include "blockdev.h"
#include "callout.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "malloc.h"
#include "mutex.h"
#include "x86.h"

struct ahci_softc;

struct ahci_port_state {
  struct ahci_softc *sc;
  int unit;
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
  struct mutex lock;
  struct buf *queue;
  int busy;
  int resetting;
  struct callout timeout;
  int sync_busy;
  int sync_error;
  int dirty;
  uint8_t flush_cmd;
};

struct ahci_softc {
  struct ahci_softc *next;
  struct hba_mem *hba;
  struct device *dev;
  struct ahci_port_state ports[AHCI_MAX_PORTS];
  int active_port_count;
  int wide_dma;
  volatile int suspended; // what is queued waits
  int waking; // the ports still coming up after a sleep, under ahci_wake_lock
};

static struct ahci_port_state **ahci_units;
static uint ahci_nunits;

static struct blockdrv ahci_blockdrv;

static struct ahci_port_state *unit_port(uint unit) {
  return unit < ahci_nunits ? ahci_units[unit] : 0;
}

static int drain_chan;
static struct mutex ahci_wake_lock = MUTEX_INITIALIZER("ahciwake");

static void ahci_timeout(void *arg);
static int ahci_port_restart(struct ahci_port_state *ps);

#define AHCI_TIMO_XFER 60000
#define AHCI_TIMO_FLUSH 30000

static void ahci_handoff(struct hba_mem *hba) {
  int i;

  if (hba->vs < 0x00010200 || !(hba->cap2 & HBA_CAP2_BOH) ||
      (hba->bohc & HBA_BOHC_OOS))
    return;
  hba->bohc |= HBA_BOHC_OOS;
  for (i = 0; i < 80; i++) {
    delay(25000);
    if (!(hba->bohc & HBA_BOHC_BOS) || !(hba->bohc & HBA_BOHC_BB))
      break;
  }
}

static int ahci_stop_port(struct hba_port *port) {
  int i;

  if (port->cmd == 0xffffffffu)
    return -1;
  port->cmd &= ~(HBA_PxCMD_ICC_MASK | HBA_PxCMD_ST | HBA_PxCMD_FRE);

  for (i = 0; i < 500 && (port->cmd & HBA_PxCMD_CR); i++)
    delay(1000);
  if (port->cmd & HBA_PxCMD_CR)
    return -1;
  for (i = 0; i < 500 && (port->cmd & HBA_PxCMD_FR); i++)
    delay(1000);
  return (port->cmd & HBA_PxCMD_FR) ? -1 : 0;
}

static void ahci_release_port(struct ahci_port_state *ps) {
  struct hba_port *port = ps->port_regs;

  if (port) {
    port->ie = 0;
    if (ahci_stop_port(port) < 0) {
      port->sctl = HBA_SCTL_IPM_DISABLED | HBA_SCTL_DET_INIT;
      delay(10000);
      port->sctl = HBA_SCTL_IPM_DISABLED | HBA_SCTL_DET_NONE;
      delay(10000);
      if (ahci_stop_port(port) < 0) {
        printf("%s: port %d will not stop, its memory is abandoned\n",
                ps->sc->dev->name, ps->port_no);
        ps->cmd_headers_virt = ps->fis_virt = 0;
        ps->tbl_pages[0] = ps->tbl_pages[1] = 0;
      }
    }
    port->clb = 0;
    port->clbu = 0;
    port->fb = 0;
    port->fbu = 0;
  }
  if (ps->cmd_headers_virt)
    page_free(ps->cmd_headers_virt);
  if (ps->fis_virt)
    page_free(ps->fis_virt);
  if (ps->tbl_pages[0])
    page_free(ps->tbl_pages[0]);
  if (ps->tbl_pages[1])
    page_free(ps->tbl_pages[1]);
  ps->cmd_headers_virt = 0;
  ps->cmd_headers = 0;
  ps->fis_virt = 0;
  ps->tbl_pages[0] = 0;
  ps->tbl_pages[1] = 0;
  ps->present = 0;
}

static int ahci_dma_addr(const struct ahci_port_state *ps, void *va,
                         uint32_t *lo, uint32_t *hi) {
  uint64_t pa = (uint64_t)V2P(va);

  if (!ps->sc->wide_dma && (pa >> 32) != 0)
    return -1;
  *lo = (uint32_t)pa;
  *hi = (uint32_t)(pa >> 32);
  return 0;
}

// PRDT
static int ahci_setup_prdt(const struct ahci_port_state *ps,
                           struct hba_cmd_tbl *tbl, void *buf, uint bytes) {
  uintptr_t vaddr = (uintptr_t)buf;
  uint64_t next = 0;
  int prdt_idx = 0;

  while (bytes > 0 && prdt_idx <= AHCI_PRDT_PER_CMD) {
    uint bytes_in_page = PGSIZE - (vaddr % PGSIZE);
    uint chunk = (bytes < bytes_in_page) ? bytes : bytes_in_page;

    if (prdt_idx > 0 && (uint64_t)V2P(vaddr) == next &&
        tbl->prdt[prdt_idx - 1].dbc + chunk < AHCI_PRDT_MAX) {
      tbl->prdt[prdt_idx - 1].dbc += chunk;
    } else {
      if (prdt_idx == AHCI_PRDT_PER_CMD)
        return -1;
      if (ahci_dma_addr(ps, (void *)vaddr, &tbl->prdt[prdt_idx].dba,
                        &tbl->prdt[prdt_idx].dbau) < 0)
        return -1;
      tbl->prdt[prdt_idx].dbc = chunk - 1;
      tbl->prdt[prdt_idx].i = 0;
      tbl->prdt[prdt_idx].rsv0 = 0;
      tbl->prdt[prdt_idx].rsv1 = 0;
      prdt_idx++;
    }
    next = (uint64_t)V2P(vaddr) + chunk;
    vaddr += chunk;
    bytes -= chunk;
  }
  if (prdt_idx > 0)
    tbl->prdt[prdt_idx - 1].i = 1;

  if (bytes > 0)
    return -1;

  return prdt_idx;
}

static int ahci_start_buf(struct ahci_port_state *ps, struct buf *b) {
  int slot = 0;
  struct hba_port *port = ps->port_regs;
  struct hba_cmd_tbl *cmdtbl = ps->cmd_tbl[slot];
  struct hba_cmd_header *cmdheader = &ps->cmd_headers[slot];

  int is_write = (b->flags & B_DIRTY) ? 1 : 0;
  uint sectors_per_block = bufcount(b) / AHCI_SECTOR_SIZE;
  uint64_t lba = b->blockno * (BSIZE / AHCI_SECTOR_SIZE);

  memset(cmdtbl, 0, sizeof(struct hba_cmd_tbl));
  int prdt_count = ahci_setup_prdt(ps, cmdtbl, bufdata(b), bufcount(b));
  if (prdt_count < 0) {
    printf("%s: port %d: no PRDT for block %lu\n", ps->sc->dev->name,
            ps->port_no, b->blockno);
    return -1;
  }

  cmdheader->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
  cmdheader->w = is_write ? 1 : 0;
  cmdheader->a = 0;
  cmdheader->p = 0;
  cmdheader->r = 0;
  cmdheader->b = 0;
  cmdheader->c = 0;
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

  cmdfis->countl = (uint8_t)(sectors_per_block & 0xff);
  cmdfis->counth = (uint8_t)((sectors_per_block >> 8) & 0xff);

  ps->busy = 1;
  port->ci = (1U << slot);
  callout_reset(&ps->timeout, AHCI_TIMO_XFER * HZ / 1000, ahci_timeout, ps);
  return 0;
}

static void ahci_start_next(struct ahci_port_state *ps) {
  while (ps->queue != 0 && !ps->busy && !ps->resetting && !ps->sc->suspended &&
         ahci_start_buf(ps, ps->queue) < 0) {
    struct buf *bad = ps->queue;

    ps->queue = bad->qnext;
    ps->busy = 0;
    bad->flags |= B_ERROR;
    bad->flags &= ~B_DIRTY;
    wakeup(bad);
  }
}

// ps->lock held
static void ahci_check_port_completion(struct ahci_port_state *ps) {
  struct buf *b = ps->queue;
  if (!b || !ps->busy || ps->resetting)
    return;

  struct hba_port *port = ps->port_regs;
  int slot = 0;

  if ((port->ci & (1U << slot)) == 0) {
    uint32_t is = port->is;
    int failed = 0;

    if ((is & HBA_PxIS_ERRORS) || (port->tfd & 0x01)) {
      printf("%s: port %d: I/O error (is 0x%x, tfd 0x%x, serr 0x%x)\n",
              ps->sc->dev->name, ps->port_no, is, port->tfd, port->serr);
      port->serr = 0xFFFFFFFF;
      port->is = is;
      failed = 1;
    }

    callout_stop_async(&ps->timeout);
    ps->queue = b->qnext;
    ps->busy = 0;
    if (ps->sc->suspended)
      wakeup(&drain_chan);

    if (failed)
      b->flags |= B_ERROR;
    else
      b->flags |= B_VALID;
    if (b->flags & B_DIRTY)
      ps->dirty = 1;
    b->flags &= ~B_DIRTY;
    wakeup(b);
    ahci_start_next(ps);
  }
}

// lock held
static void ahci_check_sync_completion(struct ahci_port_state *ps) {
  struct hba_port *port = ps->port_regs;
  uint32_t is;

  if (!ps->sync_busy || ps->resetting || (port->ci & (1U << AHCI_SYNC_SLOT)))
    return;
  is = port->is;
  if ((is & HBA_PxIS_ERRORS) || (port->tfd & 0x01)) {
    printf("%s: port %d: command error (is 0x%x, tfd 0x%x)\n",
            ps->sc->dev->name, ps->port_no, is, port->tfd);
    port->serr = 0xFFFFFFFF;
    port->is = is;
    ps->sync_error = -1;
  }
  ps->sync_busy = 0;
  wakeup(&ps->sync_busy);
  if (ps->sc->suspended)
    wakeup(&drain_chan);
}

// lock held
static void ahci_port_recover(struct ahci_port_state *ps) {
  ps->resetting = 1;
  mtx_leave(&ps->lock);
  if (ahci_port_restart(ps) < 0)
    printf("%s: port %d: no link after its reset\n", ps->sc->dev->name,
            ps->port_no);
  mtx_enter(&ps->lock);
  ps->resetting = 0;
  if (ps->busy)
    ps->port_regs->ci = 1U << 0;
  if (ps->sync_busy)
    ps->port_regs->ci = 1U << AHCI_SYNC_SLOT;
  ahci_start_next(ps);
}

static void ahci_xfer_timedout(struct ahci_port_state *ps) {
  struct buf *b = ps->queue;

  ahci_check_port_completion(ps);
  if (!ps->busy || ps->queue != b)
    return;
  printf("%s: port %d: transfer timed out, resetting\n", ps->sc->dev->name,
          ps->port_no);
  callout_stop_async(&ps->timeout);
  ps->queue = b->qnext;
  ps->busy = 0;
  b->flags |= B_ERROR;
  b->flags &= ~B_DIRTY;
  wakeup(b);
  if (ps->sc->suspended)
    wakeup(&drain_chan);
  ahci_port_recover(ps);
}

static void ahci_timeout(void *arg) {
  struct ahci_port_state *ps = arg;

  mtx_enter(&ps->lock);
  if (ps->busy && !ps->resetting)
    ahci_xfer_timedout(ps);
  mtx_leave(&ps->lock);
}

// lock held
static int ahci_sync_wait(struct ahci_port_state *ps, uint ms) {
  uint64_t until = ticks + (uint64_t)ms * HZ / 1000 + 1;

  while (ps->sync_busy) {
    ahci_check_sync_completion(ps);
    if (!ps->sync_busy)
      break;
    if (curproc != 0 && panicstr == NULL && (int64_t)(until - ticks) > 0)
      msleep(&ps->sync_busy, &ps->lock, PRIBIO, "ahcisync",
             (int)(until - ticks));
    else if ((curproc == 0 || panicstr != NULL) && ms > 0) {
      ms--;
      delay(1000);
    } else {
      ps->sync_busy = 0;
      ps->sync_error = -1;
      ahci_port_recover(ps);
      return -1;
    }
  }
  return 0;
}

// FLUSH CACHE
static int ahci_flush_port(struct ahci_port_state *ps) {
  struct hba_port *port = ps->port_regs;
  struct hba_cmd_header *cmdheader = &ps->cmd_headers[AHCI_SYNC_SLOT];
  struct hba_cmd_tbl *cmdtbl = ps->cmd_tbl[AHCI_SYNC_SLOT];
  struct fis_reg_h2d *cmdfis = (struct fis_reg_h2d *)&cmdtbl->cfis[0];

  if (ps->flush_cmd == 0 || !ps->dirty)
    return 0;
  while (ps->resetting)
    msleep(&ps->resetting, &ps->lock, PRIBIO, "ahcirst", HZ);
  if (ahci_sync_wait(ps, AHCI_TIMO_FLUSH) < 0)
    return -1;
  ps->dirty = 0;
  memset(cmdtbl, 0, sizeof(struct hba_cmd_tbl));
  cmdheader->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
  cmdheader->w = 0;
  cmdheader->a = 0;
  cmdheader->p = 0;
  cmdheader->r = 0;
  cmdheader->b = 0;
  cmdheader->c = 0;
  cmdheader->pmp = 0;
  cmdheader->prdtl = 0;
  cmdheader->prdbc = 0;
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = ps->flush_cmd;
  cmdfis->device = 1 << 6;
  ps->sync_busy = 1;
  ps->sync_error = 0;
  port->ci = 1U << AHCI_SYNC_SLOT;
  if (ahci_sync_wait(ps, AHCI_TIMO_FLUSH) < 0) {
    printf("%s: port %d: cache flush timed out\n", ps->sc->dev->name,
            ps->port_no);
    ps->dirty = 1;
    return -1;
  }
  if (ps->sync_error < 0) {
    printf("%s: port %d: cache flush failed\n", ps->sc->dev->name,
            ps->port_no);
    ps->dirty = 1;
    return -1;
  }
  return 0;
}

// identify
static uint64_t ahci_identify(struct ahci_port_state *ps) {
  int slot = 31;
  struct hba_port *port = ps->port_regs;

  char *ident_buf = page_alloc();
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
  cmdheader->c = 0;
  cmdheader->pmp = 0;
  cmdheader->prdtl = 1;
  cmdheader->prdbc = 0;

  struct hba_cmd_tbl *cmdtbl = ps->cmd_tbl[slot];
  memset(cmdtbl, 0, sizeof(struct hba_cmd_tbl));

  if (ahci_dma_addr(ps, ident_buf, &cmdtbl->prdt[0].dba,
                    &cmdtbl->prdt[0].dbau) < 0) {
    page_free(ident_buf);
    return 0;
  }
  cmdtbl->prdt[0].dbc = 512 - 1;
  cmdtbl->prdt[0].i = 1;

  struct fis_reg_h2d *cmdfis = (struct fis_reg_h2d *)(&cmdtbl->cfis[0]);
  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1;
  cmdfis->command = ATA_CMD_IDENTIFY;
  cmdfis->device = 0;

  port->ci = (1U << slot);

  int us;
  for (us = AHCI_TIMO_XFER * 1000; us > 0; us -= 10) {
    if ((port->ci & (1U << slot)) == 0)
      break;
    delay(10);
  }

  uint64_t sectors = 0;
  if ((port->ci & (1U << slot)) == 0 && !(port->is & HBA_PxIS_TFES)) {
    uint16_t *words = (uint16_t *)ident_buf;

    char *model_src = (char *)&words[27];
    for (int i = 0; i < 40; i += 2) {
      ps->model[i] = model_src[i + 1];
      ps->model[i + 1] = model_src[i];
    }
    ps->model[40] = '\0';
    for (int i = 39; i >= 0 && ps->model[i] == ' '; i--)
      ps->model[i] = '\0';

    char *serial_src = (char *)&words[10];
    for (int i = 0; i < 20; i += 2) {
      ps->serial[i] = serial_src[i + 1];
      ps->serial[i + 1] = serial_src[i];
    }
    ps->serial[20] = '\0';
    for (int i = 19; i >= 0 && ps->serial[i] == ' '; i--)
      ps->serial[i] = '\0';

    if ((words[106] & 0xc000) == 0x4000 && (words[106] & (1 << 12))) {
      uint32_t logical = (uint32_t)words[117] | ((uint32_t)words[118] << 16);

      if (logical * 2 != AHCI_SECTOR_SIZE) {
        printf("AHCI: port %d has %u-byte sectors; only %u-byte ones are "
                "supported, not attached\n",
                ps->port_no, logical * 2, AHCI_SECTOR_SIZE);
        page_free(ident_buf);
        return 0;
      }
    }

    if ((words[83] & 0xc000) == 0x4000)
      ps->flush_cmd = (words[83] & (1 << 13))   ? ATA_CMD_FLUSH_CACHE_EXT
                      : (words[83] & (1 << 12)) ? ATA_CMD_FLUSH_CACHE
                                                : 0;
    else if (words[80] != 0xffff && (words[80] & 0xfff0) != 0)
      ps->flush_cmd = ATA_CMD_FLUSH_CACHE;
    if (words[83] & (1 << 10)) {
      sectors = (uint64_t)words[100] | ((uint64_t)words[101] << 16) |
                ((uint64_t)words[102] << 32) | ((uint64_t)words[103] << 48);
    } else {
      sectors = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
    }

    ps->sata_gen = (port->ssts >> 4) & 0x0F;
  }

  page_free(ident_buf);
  return sectors;
}

// comreset
static int ahci_link_up(struct hba_port *port) {
  int i;

  port->sctl = 0;
  delay(10000);
  port->sctl = HBA_SCTL_IPM_DISABLED | HBA_SCTL_DET_INIT;
  delay(10000);
  port->sctl = HBA_SCTL_IPM_DISABLED | HBA_SCTL_DET_NONE;
  delay(10000);

  for (i = 0; i < 1000; i++) {
    if ((port->ssts & 0x0f) == HBA_PORT_DET_PRESENT)
      break;
    delay(1000);
  }
  if ((port->ssts & 0x0f) != HBA_PORT_DET_PRESENT)
    return -1;

  port->serr = port->serr;
  for (i = 0; i < 3000 && (port->tfd & 0x89); i++)
    delay(1000);
  return (port->tfd & 0x89) ? -1 : 0;
}

// 10.1.2 order
static int ahci_init_port(struct ahci_port_state *ps, int port_no,
                          struct hba_port *port) {
  void *clb, *fb, *tbl0, *tbl1;
  uint32_t cmd, lo, hi;
  int s;

  ps->port_no = port_no;
  ps->port_regs = port;
  ps->queue = 0;
  ps->busy = 0;
  ps->present = 0;
  mtx_init(&ps->lock, "ahci_port");
  callout_init(&ps->timeout);

  if ((fb = page_alloc()) == 0)
    return -1;
  memset(fb, 0, PGSIZE);
  ps->fis_virt = fb;
  if (ahci_dma_addr(ps, fb, &lo, &hi) < 0) {
    printf("AHCI: port %d sits above 4G and the controller is 32 bit only\n",
            port_no);
    ahci_release_port(ps);
    return -1;
  }
  port->fb = lo;
  port->fbu = hi;

  cmd = port->cmd & ~HBA_PxCMD_ICC_MASK;
  cmd |= HBA_PxCMD_FRE | HBA_PxCMD_POD | HBA_PxCMD_SUD;
  port->cmd = cmd | HBA_PxCMD_ICC_ACTIVE;
  if (!(port->cmd & HBA_PxCMD_FRE)) {
    ahci_release_port(ps);
    return -1;
  }

  clb = page_alloc();
  tbl0 = page_alloc();
  tbl1 = page_alloc();
  ps->cmd_headers_virt = clb;
  ps->tbl_pages[0] = tbl0;
  ps->tbl_pages[1] = tbl1;
  if (!clb || !tbl0 || !tbl1) {
    ahci_release_port(ps);
    return -1;
  }
  memset(clb, 0, PGSIZE);
  memset(tbl0, 0, PGSIZE);
  memset(tbl1, 0, PGSIZE);
  ps->cmd_headers = (struct hba_cmd_header *)clb;

  if (ahci_dma_addr(ps, clb, &lo, &hi) < 0) {
    ahci_release_port(ps);
    return -1;
  }
  port->clb = lo;
  port->clbu = hi;

  for (s = 0; s < AHCI_NUM_SLOTS; s++) {
    struct hba_cmd_tbl *tbl =
        (s < 16)
            ? (struct hba_cmd_tbl *)((uintptr_t)tbl0 +
                                     s * sizeof(struct hba_cmd_tbl))
            : (struct hba_cmd_tbl *)((uintptr_t)tbl1 +
                                     (s - 16) * sizeof(struct hba_cmd_tbl));
    ps->cmd_tbl[s] = tbl;
    if (ahci_dma_addr(ps, tbl, &lo, &hi) < 0) {
      ahci_release_port(ps);
      return -1;
    }
    ps->cmd_headers[s].ctba = lo;
    ps->cmd_headers[s].ctbau = hi;
  }

  if (ahci_link_up(port) < 0) {
    if ((port->ssts & 0x0f) != 0)
      printf("AHCI: port %d: ssts 0x%x tfd 0x%x, no link\n", port_no,
              port->ssts, port->tfd);
    ahci_release_port(ps);
    return -1;
  }
  if (port->sig != SATA_SIG_ATA) {
    printf("AHCI: port %d: signature 0x%x, not a disk\n", port_no, port->sig);
    ahci_release_port(ps);
    return -1;
  }

  port->serr = 0xFFFFFFFF;
  port->is = 0xFFFFFFFF;
  port->ie = HBA_PxIS_DHRS | HBA_PxIS_PSS | HBA_PxIS_DSS | HBA_PxIS_SDBS |
             HBA_PxIS_DPS | HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_IFS;

  for (s = 0; s < 100000 && (port->cmd & HBA_PxCMD_CR); s++)
    delay(10);
  if (port->cmd & HBA_PxCMD_CR) {
    ahci_release_port(ps);
    return -1;
  }
  port->cmd |= HBA_PxCMD_ST;

  ps->sector_count = ahci_identify(ps);
  if (ps->sector_count == 0) {
    printf("AHCI: port %d: identify failed, is 0x%x tfd 0x%x serr 0x%x\n",
            port_no, port->is, port->tfd, port->serr);
    ahci_release_port(ps);
    return -1;
  }
  ps->present = 1;

  {
    const char *speed = "SATA-I (1.5 Gbps)";
    if (ps->sata_gen == 2)
      speed = "SATA-II (3.0 Gbps)";
    else if (ps->sata_gen == 3)
      speed = "SATA-III (6.0 Gbps)";

    printf("AHCI: port %d [%s] Model '%s' Serial '%s', %u MB (%lu sectors)\n",
            port_no, speed, ps->model[0] ? ps->model : "Generic ATA",
            ps->serial[0] ? ps->serial : "N/A",
            (uint)(ps->sector_count * AHCI_SECTOR_SIZE / (1024 * 1024)),
            (uint64_t)ps->sector_count);
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
  uint64_t abar_phys = dev->pci.bar[5];
  if (abar_phys == 0 || dev->pci.bar_is_io[5]) {
    printf("%s: no ABAR in BAR5\n", dev->name);
    return -1;
  }

  struct ahci_softc *sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
  struct hba_mem *hba = (struct hba_mem *)ioremap(abar_phys, 0x1100);

  sc->hba = hba;
  sc->dev = dev;

  ahci_handoff(hba);

  uint32_t pi = hba->pi;
  if (hba->ghc & HBA_GHC_AE) {
    int i;

    hba->ghc = HBA_GHC_HR;
    for (i = 0; i < 1000 && (hba->ghc & HBA_GHC_HR); i++)
      delay(1000);
    if (hba->ghc & HBA_GHC_HR) {
      printf("%s: the controller will not reset\n", dev->name);
      free(sc, M_DEVBUF);
      return -1;
    }
  }
  hba->ghc = HBA_GHC_AE;
  hba->pi = pi;
  sc->wide_dma = (hba->cap & HBA_CAP_S64A) != 0;

  for (int p = 0; p < AHCI_MAX_PORTS; p++) {
    if (!(pi & (1U << p)))
      continue;

    struct hba_port *port = &hba->ports[p];

    port->ie = 0;
    ahci_stop_port(port);
    port->clb = 0;
    port->clbu = 0;
    port->fb = 0;
    port->fbu = 0;

    sc->ports[p].sc = sc;
    if (ahci_init_port(&sc->ports[p], p, port) == 0)
      sc->active_port_count++;
  }

  if (sc->active_port_count > 0) {
    int p;

    if (driver_setup_intr(dev) < 0) {
      printf("%s: no interrupt\n", dev->name);
      for (p = 0; p < AHCI_MAX_PORTS; p++)
        if (sc->ports[p].present)
          ahci_release_port(&sc->ports[p]);
      free(sc, M_DEVBUF);
      return -1;
    }
    for (p = 0; p < AHCI_MAX_PORTS; p++)
      if (sc->ports[p].present)
        sc->ports[p].port_regs->ie = HBA_PxIE_WANTED;
    hba->ghc |= HBA_GHC_IE;
  }

  for (int p = 0; p < AHCI_MAX_PORTS; p++)
    if (sc->ports[p].present) {
      struct ahci_port_state **grown =
          malloc((ahci_nunits + 1) * sizeof(*grown), M_DEVBUF, M_WAITOK);

      if (ahci_nunits > 0)
        memmove(grown, ahci_units, ahci_nunits * sizeof(*grown));
      if (ahci_units != 0)
        free(ahci_units, M_DEVBUF);
      ahci_units = grown;
      sc->ports[p].unit = (int)ahci_nunits;
      ahci_units[ahci_nunits++] = &sc->ports[p];
    }
  ahci_blockdrv.units = ahci_nunits > 0 ? ahci_nunits : 1;
  dev->driver_data = sc;
  printf("%s: %d drive(s)\n", dev->name,
          sc->active_port_count);
  return 0;
}

static int ahci_intr(struct device *dev) {
  struct ahci_softc *sc = dev->driver_data;
  uint32_t is, pis;
  int p;

  if (sc == 0)
    return 0;
  is = sc->hba->is;
  if (is == 0 || is == 0xffffffffu)
    return 0;
  for (p = 0; p < AHCI_MAX_PORTS; p++) {
    struct ahci_port_state *ps = &sc->ports[p];

    if (!(is & (1U << p)))
      continue;
    if (!ps->present) {
      sc->hba->ports[p].is = sc->hba->ports[p].is;
      continue;
    }
    mtx_enter(&ps->lock);
    pis = ps->port_regs->is;
    ahci_check_port_completion(ps);
    ahci_check_sync_completion(ps);
    ps->port_regs->is = pis;
    mtx_leave(&ps->lock);
  }
  sc->hba->is = is;
  return 1;
}

static void ahci_flush_all(struct ahci_softc *sc) {
  int p;

  for (p = 0; p < AHCI_MAX_PORTS; p++) {
    struct ahci_port_state *ps = &sc->ports[p];

    if (!ps->present)
      continue;
    mtx_enter(&ps->lock);
    ahci_flush_port(ps);
    mtx_leave(&ps->lock);
  }
}

static void ahci_shutdown(struct device *dev) {
  struct ahci_softc *sc = dev->driver_data;

  if (sc == 0)
    return;

  sc->hba->ghc &= ~HBA_GHC_IE;
  for (int p = 0; p < AHCI_MAX_PORTS; p++) {
    struct ahci_port_state *ps = &sc->ports[p];
    if (ps->port_regs) {
      ps->port_regs->ie = 0;
      ahci_stop_port(ps->port_regs);
    }
  }
}

// SLEEP
static int ahci_suspend(struct device *dev) {
  struct ahci_softc *sc = dev->driver_data;
  int p;

  if (sc == 0)
    return 0;
  mtx_enter(&ahci_wake_lock);
  while (sc->waking)
    msleep(&sc->waking, &ahci_wake_lock, PRIBIO, "ahciwake", 0);
  mtx_leave(&ahci_wake_lock);
  sc->suspended = 1;
  for (p = 0; p < AHCI_MAX_PORTS; p++) {
    struct ahci_port_state *ps = &sc->ports[p];

    if (!ps->present)
      continue;
    mtx_enter(&ps->lock);
    for (;;) {
      ahci_check_port_completion(ps);
      ahci_check_sync_completion(ps);
      if (!ps->busy && !ps->sync_busy)
        break;
      msleep(&drain_chan, &ps->lock, PRIBIO, "ahcisusp", 0);
    }
    mtx_leave(&ps->lock);
  }
  ahci_flush_all(sc);
  ahci_shutdown(dev);
  return 0;
}

static int ahci_port_restart(struct ahci_port_state *ps) {
  struct hba_port *port = ps->port_regs;
  uint32_t lo, hi;
  int s;

  port->ie = 0;
  ahci_stop_port(port);
  ahci_dma_addr(ps, ps->cmd_headers_virt, &lo, &hi);
  port->clb = lo;
  port->clbu = hi;
  ahci_dma_addr(ps, ps->fis_virt, &lo, &hi);
  port->fb = lo;
  port->fbu = hi;
  port->cmd = (port->cmd & ~HBA_PxCMD_ICC_MASK) | HBA_PxCMD_FRE |
              HBA_PxCMD_POD | HBA_PxCMD_SUD | HBA_PxCMD_ICC_ACTIVE;
  if (ahci_link_up(port) < 0)
    return -1;
  port->serr = 0xFFFFFFFF;
  port->is = 0xFFFFFFFF;
  for (s = 0; s < 100000 && (port->cmd & HBA_PxCMD_CR); s++)
    delay(10);
  port->cmd |= HBA_PxCMD_ST;
  port->ie = HBA_PxIE_WANTED;
  return 0;
}

#define WAKE_LINK_MS 4000 // the link, then BSY and DRQ, as ahci_link_up allows

// port is up: engine on, what waited goes
static void port_up(struct ahci_port_state *ps) {
  struct hba_port *port = ps->port_regs;
  int s;

  port->serr = 0xFFFFFFFF;
  port->is = 0xFFFFFFFF;
  for (s = 0; s < 100000 && (port->cmd & HBA_PxCMD_CR); s++)
    delay(10);
  port->cmd |= HBA_PxCMD_ST;
  port->ie = HBA_PxIE_WANTED;
  mtx_enter(&ps->lock);
  ps->resetting = 0;
  ps->busy = 0;
  wakeup(&ps->resetting);
  ahci_start_next(ps);
  mtx_leave(&ps->lock);
}

// COMRESET all ports, then wait per port; the wake does not wait for the disks
static void ahci_ports_up(void *arg) {
  struct ahci_softc *sc = arg;
  uint64_t until = ticks + (uint64_t)WAKE_LINK_MS * HZ / 1000;
  uint32_t lo, hi, left = 0, linked = 0;
  int p;

  for (p = 0; p < AHCI_MAX_PORTS; p++) {
    struct ahci_port_state *ps = &sc->ports[p];
    struct hba_port *port = ps->port_regs;

    if (!ps->present)
      continue;
    port->ie = 0;
    ahci_stop_port(port);
    ahci_dma_addr(ps, ps->cmd_headers_virt, &lo, &hi);
    port->clb = lo;
    port->clbu = hi;
    ahci_dma_addr(ps, ps->fis_virt, &lo, &hi);
    port->fb = lo;
    port->fbu = hi;
    port->cmd = (port->cmd & ~HBA_PxCMD_ICC_MASK) | HBA_PxCMD_FRE | HBA_PxCMD_POD |
                HBA_PxCMD_SUD | HBA_PxCMD_ICC_ACTIVE;
    port->sctl = 0;
    left |= 1u << p;
  }
  tsleep(&sc->waking, PRIBIO, "comreset", 1);
  for (p = 0; p < AHCI_MAX_PORTS; p++)
    if (left & (1u << p))
      sc->ports[p].port_regs->sctl = HBA_SCTL_IPM_DISABLED | HBA_SCTL_DET_INIT;
  tsleep(&sc->waking, PRIBIO, "comreset", 1);
  for (p = 0; p < AHCI_MAX_PORTS; p++)
    if (left & (1u << p))
      sc->ports[p].port_regs->sctl = HBA_SCTL_IPM_DISABLED | HBA_SCTL_DET_NONE;

  while (left != 0) {
    for (p = 0; p < AHCI_MAX_PORTS; p++) {
      struct hba_port *port = sc->ports[p].port_regs;

      if (!(left & (1u << p)) || (port->ssts & 0x0f) != HBA_PORT_DET_PRESENT)
        continue;
      // DIAG.X holds back the first FIS. clear it once linked
      if (!(linked & (1u << p))) {
        port->serr = port->serr;
        linked |= 1u << p;
      }
      if (port->tfd & 0x89)
        continue;
      port_up(&sc->ports[p]);
      left &= ~(1u << p);
    }
    if (left == 0 || ticks >= until)
      break;
    tsleep(&sc->waking, PRIBIO, "ahciup", 1);
  }
  for (p = 0; p < AHCI_MAX_PORTS; p++)
    if (left & (1u << p)) {
      printf("%s: port %d has no link after sleep\n", sc->dev->name, p);
      port_up(&sc->ports[p]);
    }
  mtx_enter(&ahci_wake_lock);
  sc->waking = 0;
  wakeup(&sc->waking);
  mtx_leave(&ahci_wake_lock);
  kthread_exit(0);
}

static int ahci_resume(struct device *dev) {
  struct ahci_softc *sc = dev->driver_data;
  struct hba_mem *hba;
  uint32_t pi;
  int p;

  if (sc == 0)
    return 0;
  hba = sc->hba;
  ahci_handoff(hba);
  pi = hba->pi;
  hba->ghc = HBA_GHC_AE;
  hba->pi = pi;
  for (p = 0; p < AHCI_MAX_PORTS; p++)
    if (sc->ports[p].present) {
      mtx_enter(&sc->ports[p].lock);
      sc->ports[p].resetting = 1;
      mtx_leave(&sc->ports[p].lock);
    }
  hba->ghc |= HBA_GHC_IE;
  sc->suspended = 0;
  mtx_enter(&ahci_wake_lock);
  sc->waking = 1;
  mtx_leave(&ahci_wake_lock);
  if (kthread_create(ahci_ports_up, sc, 0, "ahciup") != 0) {
    printf("%s: no thread to bring the ports up\n", dev->name);
    mtx_enter(&ahci_wake_lock);
    sc->waking = 0;
    mtx_leave(&ahci_wake_lock);
    for (p = 0; p < AHCI_MAX_PORTS; p++)
      if (sc->ports[p].present && ahci_port_restart(&sc->ports[p]) == 0)
        port_up(&sc->ports[p]);
  }
  return 0;
}

static int ahci_ident(uint unit, struct fnu_diskinfo *out) {
  struct ahci_port_state *ps = unit_port(unit);

  if (ps == 0)
    return -1;
  strlcpy(out->model, ps->model[0] ? ps->model : "Generic ATA",
             FNU_DISK_MODEL_MAX);
  strlcpy(out->serial, ps->serial, FNU_DISK_SERIAL_MAX);
  out->kind = FNU_DISK_SATA;
  out->link_gen = (uint32_t)ps->sata_gen;
  return 0;
}

static int ahci_major;

static uint64_t ahci_unit_size(uint unit) { return ahci_size(unit); }

static uint ahci_maxphys(uint unit) {
  (void)unit;
  return MAXPHYS;
}

static int ahci_unit_flush(uint unit) {
  struct ahci_port_state *ps = unit_port(unit);
  int r;

  if (ps == 0)
    return -1;
  mtx_enter(&ps->lock);
  r = ahci_flush_port(ps);
  mtx_leave(&ps->lock);
  return r;
}

static struct blockdrv ahci_blockdrv = {
    .name = "ahci",
    .prefix = "sd",
    .units = 1,
    .strategy = ahcirw,
    .maxphys = ahci_maxphys,
    .size = ahci_unit_size,
    .flush = ahci_unit_flush,
    .ident = ahci_ident,
};

static struct driver ahci_driver = {
    .name = "ahci",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_BLOCK,
    .probe = ahci_probe_device,
    .attach = ahci_attach_device,
    .intr = ahci_intr,
    .shutdown = ahci_shutdown,
    .suspend = ahci_suspend,
    .resume = ahci_resume,
};

void ahci_driver_init(void) {
  driver_register(&ahci_driver);
  ahci_major = blockdev_register(&ahci_blockdrv);
}

uint64_t ahci_size(uint unit) {
  struct ahci_port_state *ps = unit_port(unit);

  return ps != 0 ? ps->sector_count : 0;
}

// the buffer cache's strategy routine
void ahcirw(struct buf *b) {
  struct ahci_port_state *ps = unit_port(minor(b->dev));

  if (ps == 0 || bufcount(b) == 0 || bufcount(b) % AHCI_SECTOR_SIZE != 0 ||
      bufcount(b) > MAXPHYS ||
      b->blockno * (BSIZE / AHCI_SECTOR_SIZE) +
              bufcount(b) / AHCI_SECTOR_SIZE >
          ps->sector_count) {
    b->flags |= B_ERROR;
    b->flags &= ~B_DIRTY;
    return;
  }

  if (!holdingsleep(&b->lock))
    panic("ahcirw: buf not locked");
  if ((b->flags & (B_VALID | B_DIRTY)) == B_VALID)
    panic("ahcirw: nothing to do");

  mtx_enter(&ps->lock);

  b->qnext = 0;
  struct buf **cursor;
  for (cursor = &ps->queue; *cursor; cursor = &(*cursor)->qnext)
    ;
  *cursor = b;

  ahci_start_next(ps);

  if (curproc != 0 && panicstr == NULL) {
    while ((b->flags & B_ERROR) == 0 &&
           (b->flags & (B_VALID | B_DIRTY)) != B_VALID) {
      msleep(b, &ps->lock, PRIBIO, "ahci", 0);
    }
  } else {
    int us;

    for (;;) {
      for (us = AHCI_TIMO_XFER * 1000; us > 0; us -= 10) {
        if ((b->flags & B_ERROR) ||
            (b->flags & (B_VALID | B_DIRTY)) == B_VALID)
          break;
        ahci_check_port_completion(ps);
        delay(10);
      }
      if (us > 0 || !ps->busy)
        break;
      ahci_xfer_timedout(ps);
    }
  }

  mtx_leave(&ps->lock);
}
