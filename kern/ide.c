// IDE
#include "buf.h"
#include "blockdev.h"
#include "defs.h"
#include "driver.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "proc.h"
#include "param.h"
#include "mutex.h"
#include "trap.h"
#include "x86.h"

#define SECTOR_SIZE 512
#define IDE_CHANNELS 2
#define IDE_DEVICES 4
#define IDE_SPINS 1000000

#define SR_BSY 0x80
#define SR_DRDY 0x40
#define SR_DWF 0x20
#define SR_DRQ 0x08
#define SR_ERR 0x01

// device control: no interrupts, soft reset
#define CTL_NIEN 0x02
#define CTL_SRST 0x04

#define IDE_PRIMARY_COMMAND 0x1f0
#define IDE_PRIMARY_CONTROL 0x3f6
#define IDE_SECONDARY_COMMAND 0x170
#define IDE_SECONDARY_CONTROL 0x376

#define REG_DATA 0x00
#define REG_SECCOUNT0 0x02
#define REG_LBA0 0x03
#define REG_LBA1 0x04
#define REG_LBA2 0x05
#define REG_HDDEVSEL 0x06
#define REG_COMMAND 0x07
#define REG_STATUS 0x07

#define IDE_CMD_READ 0x20
#define IDE_CMD_READ_EXT 0x24
#define IDE_CMD_READDMA_EXT 0x25
#define IDE_CMD_RDMUL_EXT 0x29
#define IDE_CMD_WRITE 0x30
#define IDE_CMD_WRITE_EXT 0x34
#define IDE_CMD_WRITEDMA_EXT 0x35
#define IDE_CMD_WRMUL_EXT 0x39
#define IDE_CMD_RDMUL 0xc4
#define IDE_CMD_WRMUL 0xc5
#define IDE_CMD_SETMUL 0xc6
#define IDE_CMD_READDMA 0xc8
#define IDE_CMD_WRITEDMA 0xca
#define IDE_CMD_IDENTIFY 0xec
#define IDE_CMD_FLUSHCACHE 0xe7
#define IDE_CMD_FLUSHCACHE_EXT 0xea

#define LBA48_THRESHOLD 0x0fffffffull
#define IDE_FLUSH_MS 30000

#define BM_COMMAND 0
#define BM_STATUS 2
#define BM_PRDT 4
#define BM_CMD_START 0x01
#define BM_CMD_READ 0x08
#define BM_STATUS_ERROR 0x02
#define BM_STATUS_INTR 0x04

struct prd {
  uint32_t addr;
  uint16_t count;
  uint16_t flags;
} __packed;
#define PRD_EOT 0x8000

struct idechannel {
  struct mutex lock;
  struct buf *queue;
  ushort command_base;
  ushort control_base;
  ushort bm_base;
  uint xfer;
  uint total;
  uint block;
  int dma;
  int active;
};

static struct idechannel channels[IDE_CHANNELS] = {
    {{0}, 0, IDE_PRIMARY_COMMAND, IDE_PRIMARY_CONTROL, 0, 0, 0, 0, 0, 0},
    {{0}, 0, IDE_SECONDARY_COMMAND, IDE_SECONDARY_CONTROL, 0, 0, 0, 0, 0, 0},
};
static uint64_t disk_sectors[IDE_DEVICES];
static uint disk_multiple[IDE_DEVICES];
static int disk_dma[IDE_DEVICES];
static int disk_lba48[IDE_DEVICES];
static int disk_flush[IDE_DEVICES];
static char disk_serial[IDE_DEVICES][21];
static char disk_model[IDE_DEVICES][41];
static volatile int disk_dirty[IDE_DEVICES];

static uchar bounce[IDE_CHANNELS][MAXPHYS] __aligned(65536);
#define PRD_MAX 65536
#define PRD_ENTRIES (MAXPHYS / PRD_MAX + 1)
static struct prd prdt[IDE_CHANNELS][PRD_ENTRIES] __aligned(16);

static volatile int asleep;

static int channel_for(uint device) { return device / 2; }
static int channel_index(struct idechannel *c) { return (int)(c - channels); }
static int unit_for(uint device) { return device & 1; }

static int idewait(struct idechannel *channel, int checkerr) {
  int status, spins;

  for (spins = 0; spins < IDE_SPINS; spins++) {
    status = inb(channel->command_base + REG_STATUS);
    if ((status & (SR_BSY | SR_DRDY)) == SR_DRDY) {
      if (checkerr && (status & (SR_DWF | SR_ERR)) != 0)
        return -1;
      return 0;
    }
  }
  return -1;
}

// 400 ns after selecting
static void ideselect(struct idechannel *channel, int unit, uint head) {
  int i;

  outb(channel->command_base + REG_HDDEVSEL, 0xe0 | (unit << 4) | head);
  for (i = 0; i < 4; i++)
    inb(channel->control_base);
}

static void ideset_multiple(uint device, uint most) {
  struct idechannel *channel = &channels[channel_for(device)];
  uint want = BCLUSTER;

  disk_multiple[device] = 1;
  if (most == 0 || most == 0xff)
    return;
  if (want > most)
    want = most;
  if (want < 2)
    return;
  ideselect(channel, unit_for(device), 0);
  outb(channel->command_base + REG_SECCOUNT0, want);
  outb(channel->command_base + REG_COMMAND, IDE_CMD_SETMUL);
  if (idewait(channel, 1) < 0)
    return;
  disk_multiple[device] = want;
}

static void ata_string(char *out, const uchar *raw, int n) {
  int i;

  for (i = 0; i < n; i += 2) {
    out[i] = (char)raw[i + 1];
    out[i + 1] = (char)raw[i];
  }
  for (i = n; i > 0 && out[i - 1] == ' '; i--)
    ;
  out[i] = 0;
}

static uint64_t ideidentify(uint device) {
  struct idechannel *channel;
  uint identify[128];
  int status, spins;

  if (device >= IDE_DEVICES)
    return 0;
  channel = &channels[channel_for(device)];
  // polled: no interrupt for a command nobody waits on
  outb(channel->control_base, CTL_NIEN);
  ideselect(channel, unit_for(device), 0);
  outb(channel->command_base + REG_SECCOUNT0, 0);
  outb(channel->command_base + REG_LBA0, 0);
  outb(channel->command_base + REG_LBA1, 0);
  outb(channel->command_base + REG_LBA2, 0);
  outb(channel->command_base + REG_COMMAND, IDE_CMD_IDENTIFY);
  status = inb(channel->command_base + REG_STATUS);
  if (status == 0)
    return 0;
  for (spins = 0; spins < 100000; spins++) {
    status = inb(channel->command_base + REG_STATUS);
    if ((status & SR_BSY) == 0)
      break;
  }
  if (spins == 100000 || (status & SR_ERR) != 0 ||
      inb(channel->command_base + REG_LBA1) != 0 ||
      inb(channel->command_base + REG_LBA2) != 0)
    return 0;
  for (spins = 0; spins < 100000 && (status & SR_DRQ) == 0; spins++)
    status = inb(channel->command_base + REG_STATUS);
  if ((status & SR_DRQ) == 0 || (status & SR_ERR) != 0)
    return 0;
  insl(channel->command_base + REG_DATA, identify, 128);
  ata_string(disk_serial[device], (const uchar *)identify + 20, 20);
  ata_string(disk_model[device], (const uchar *)identify + 54, 40);
  ideset_multiple(device, (identify[23] >> 16) & 0xff);
  disk_dma[device] = ((identify[24] >> 16) & (1u << 8)) != 0;
  disk_lba48[device] = ((identify[41] >> 16) & (1u << 10)) != 0;
  if (((identify[41] >> 16) & 0xc000) == 0x4000)
    disk_flush[device] = ((identify[41] >> 16) & (1u << 13)) ? IDE_CMD_FLUSHCACHE_EXT
                         : ((identify[41] >> 16) & (1u << 12)) ? IDE_CMD_FLUSHCACHE
                                                              : 0;
  else if ((identify[40] & 0xffff) != 0xffff && (identify[40] & 0xfff0) != 0)
    disk_flush[device] = IDE_CMD_FLUSHCACHE;
  if (disk_lba48[device])
    return (uint64_t)identify[50] | ((uint64_t)identify[51] << 32);
  return identify[30];
}

uint64_t ide_size(uint device) {
  return device < IDE_DEVICES ? disk_sectors[device] : 0;
}

static int ide_major;

static uint64_t ide_unit_size(uint unit) { return ide_size(unit); }
static int ide_flush(uint unit);

static int ide_dma_ok(uint unit) {
  return unit < IDE_DEVICES && disk_dma[unit] &&
         channels[channel_for(unit)].bm_base != 0;
}

// READ MULTIPLE
static uint ide_maxphys(uint unit) {
  (void)unit;
  return MAXPHYS < 256 * FNU_BLOCKDEV_SECTOR_SIZE
             ? MAXPHYS
             : 256 * FNU_BLOCKDEV_SECTOR_SIZE;
}

static int ide_ident(uint unit, struct fnu_diskinfo *out) {
  if (unit >= IDE_DEVICES || disk_sectors[unit] == 0)
    return -1;
  strlcpy(out->model, disk_model[unit][0] ? disk_model[unit] : "Generic ATA",
             FNU_DISK_MODEL_MAX);
  strlcpy(out->serial, disk_serial[unit], FNU_DISK_SERIAL_MAX);
  out->kind = FNU_DISK_IDE;
  return 0;
}

static const struct blockdrv ide_blockdrv = {
    .name = "ide",
    .prefix = "wd",
    .units = IDE_DEVICES,
    .strategy = wd_strategy,
    .maxphys = ide_maxphys,
    .size = ide_unit_size,
    .flush = ide_flush,
    .ident = ide_ident,
};

// ATTACH

static int native[IDE_CHANNELS];
static int attached;

static int ide_irq(void *channel) { return wdc_intr((int)(intptr_t)channel); }

static int pciide_intr(struct device *dev __unused) {
  int c, claimed = 0;

  for (c = 0; c < IDE_CHANNELS; c++)
    if (native[c] && wdc_intr(c))
      claimed = 1;
  return claimed;
}

static int pciide_probe(struct device *dev) {
  return dev->pci.class_code == PCI_CLASS_STORAGE &&
                 dev->pci.subclass == PCI_SUBCLASS_IDE
             ? 50
             : 0;
}

// compat channels: ISA ports. native: BARs 0-3
static int pciide_attach(struct device *dev) {
  const struct pci_device *pd = &dev->pci;
  int c, wired = 0, found = 0;
  uint device;

  if (attached) {
    printf("%s: one IDE controller is all this driver keeps\n", dev->name);
    return -1;
  }
  for (c = 0; c < IDE_CHANNELS; c++) {
    mtx_init(&channels[c].lock, c == 0 ? "ide0" : "ide1");
    native[c] = (pd->prog_if & (1 << (2 * c))) != 0;
    if (native[c]) {
      if (!pd->bar_is_io[2 * c] || !pd->bar_is_io[2 * c + 1] || pd->bar[2 * c] == 0) {
        native[c] = 0;
        channels[c].command_base = 0;
        continue;
      }
      channels[c].command_base = (ushort)pd->bar[2 * c];
      channels[c].control_base = (ushort)(pd->bar[2 * c + 1] + 2);
    }
  }
  if (pd->bar_is_io[4] && pd->bar[4] != 0 && pd->bar[4] <= 0xfff0) {
    pci_enable_bus_master(pd);
    for (c = 0; c < IDE_CHANNELS; c++)
      channels[c].bm_base = (ushort)(pd->bar[4] + 8 * c);
  }
  for (device = 0; device < IDE_DEVICES; device++)
    if (channels[channel_for(device)].command_base != 0 &&
        (disk_sectors[device] = ideidentify(device)) != 0)
      found = 1;
  if (!found) {
    printf("%s: no drives\n", dev->name);
    return -1;
  }
  for (c = 0; c < IDE_CHANNELS; c++) {
    if (disk_sectors[2 * c] == 0 && disk_sectors[2 * c + 1] == 0) {
      native[c] = 0;
      continue;
    }
    if (native[c])
      wired = 1;
    else
      intr_establish(ioapic_isa_vector(c == 0 ? IRQ_IDE_PRIMARY : IRQ_IDE_SECONDARY,
                                       ncpus - 1),
                     ide_irq, (void *)(intptr_t)c, c == 0 ? "ide0" : "ide1");
  }
  if (wired && driver_setup_intr(dev) < 0) {
    printf("%s: no interrupt for the native channels\n", dev->name);
    return -1;
  }
  printf("%s: %s/%s channels, %d/%d and %d/%d sectors%s\n", dev->name,
         native[0] ? "native" : "compat", native[1] ? "native" : "compat",
         (uint)disk_sectors[0], (uint)disk_sectors[1], (uint)disk_sectors[2],
         (uint)disk_sectors[3], channels[0].bm_base != 0 ? ", bus master dma" : "");
  ide_major = blockdev_register(&ide_blockdrv);
  attached = 1;
  return 0;
}

static int ide_suspend(void);
static void ide_resume(void);

static int pciide_suspend(struct device *dev __unused) { return ide_suspend(); }

static int pciide_resume(struct device *dev __unused) {
  ide_resume();
  return 0;
}

static struct driver pciide_driver = {
    .name = "pciide",
    .bus_type = BUS_TYPE_PCI,
    .dev_type = DEVICE_TYPE_BLOCK,
    .probe = pciide_probe,
    .attach = pciide_attach,
    .intr = pciide_intr,
    .suspend = pciide_suspend,
    .resume = pciide_resume,
};

void pciide_driver_init(void) { driver_register(&pciide_driver); }

// one block of a write
static int idepush(struct idechannel *channel, struct buf *buffer) {
  uint left = channel->total - channel->xfer;
  uint n = left < channel->block ? left : channel->block;
  int status, spins;

  for (spins = 0; spins < IDE_SPINS; spins++) {
    status = inb(channel->command_base + REG_STATUS);
    if ((status & (SR_DWF | SR_ERR)) != 0)
      return -1;
    if ((status & (SR_BSY | SR_DRQ)) == SR_DRQ)
      break;
  }
  if (spins == IDE_SPINS)
    return -1;
  outsl(channel->command_base + REG_DATA,
        bufdata(buffer) + channel->xfer * SECTOR_SIZE,
        n * SECTOR_SIZE / 4);
  channel->xfer += n;
  return 0;
}

static int wdc_start(struct idechannel *channel, struct buf *buffer) {
  uint unit = minor(buffer->dev);
  uint sectors = bufcount(buffer) / SECTOR_SIZE;
  uint64_t sector = buffer->blockno * (BSIZE / SECTOR_SIZE);
  int write = (buffer->flags & B_DIRTY) != 0, ext;
  int command;

  if (buffer->flags & B_FLUSH) {
    outb(channel->control_base, 0);
    ideselect(channel, unit_for(unit), 0);
    if (idewait(channel, 0) < 0)
      return -1;
    channel->dma = 0;
    channel->xfer = channel->total = 0;
    outb(channel->command_base + REG_COMMAND, disk_flush[unit]);
    return 0;
  }
  if (sectors == 0 || sectors > 65536)
    panic("wdc_start");
  ext = sector + sectors - 1 > LBA48_THRESHOLD || sectors > 256;
  if (ext && !disk_lba48[unit])
    return -1;
  channel->xfer = 0;
  channel->total = sectors;
  channel->block = disk_multiple[unit] < 2 ? 1 : disk_multiple[unit];
  if (channel->block > sectors)
    channel->block = sectors;
  outb(channel->control_base, 0);
  // select before reading status
  ideselect(channel, unit_for(unit), ext ? 0 : (sector >> 24) & 0x0f);
  if (idewait(channel, 0) < 0)
    return -1;
  if (ext) {
    // two-deep fifo: high bytes first
    outb(channel->command_base + REG_SECCOUNT0, (sectors >> 8) & 0xff);
    outb(channel->command_base + REG_LBA0, (sector >> 24) & 0xff);
    outb(channel->command_base + REG_LBA1, (sector >> 32) & 0xff);
    outb(channel->command_base + REG_LBA2, (sector >> 40) & 0xff);
  }
  outb(channel->command_base + REG_SECCOUNT0, sectors & 0xff);
  outb(channel->command_base + REG_LBA0, sector & 0xff);
  outb(channel->command_base + REG_LBA1, (sector >> 8) & 0xff);
  outb(channel->command_base + REG_LBA2, (sector >> 16) & 0xff);
  channel->dma = ide_dma_ok(unit);
  if (channel->dma) {
    int c = channel_index(channel);

    if (write)
      memmove(bounce[c], bufdata(buffer), sectors * SECTOR_SIZE);
    uint left = sectors * SECTOR_SIZE, off = 0;
    int n = 0;

    while (left != 0 && n < PRD_ENTRIES) {
      uint chunk = left > PRD_MAX ? PRD_MAX : left;

      prdt[c][n].addr = (uint32_t)V2P(bounce[c] + off);
      prdt[c][n].count = (uint16_t)chunk;
      prdt[c][n].flags = 0;
      off += chunk;
      left -= chunk;
      n++;
    }
    prdt[c][n - 1].flags = PRD_EOT;
    outl(channel->bm_base + BM_PRDT, (uint32_t)V2P(&prdt[c][0]));
    outb(channel->bm_base + BM_COMMAND, write ? 0 : BM_CMD_READ);
    outb(channel->bm_base + BM_STATUS, BM_STATUS_INTR | BM_STATUS_ERROR);
    if (write)
      command = ext ? IDE_CMD_WRITEDMA_EXT : IDE_CMD_WRITEDMA;
    else
      command = ext ? IDE_CMD_READDMA_EXT : IDE_CMD_READDMA;
    outb(channel->command_base + REG_COMMAND, command);
    outb(channel->bm_base + BM_COMMAND,
         (write ? 0 : BM_CMD_READ) | BM_CMD_START);
    return 0;
  }
  if (write) {
    if (channel->block > 1)
      command = ext ? IDE_CMD_WRMUL_EXT : IDE_CMD_WRMUL;
    else
      command = ext ? IDE_CMD_WRITE_EXT : IDE_CMD_WRITE;
    outb(channel->command_base + REG_COMMAND, command);
    if (idepush(channel, buffer) < 0)
      return -1;
  } else {
    if (channel->block > 1)
      command = ext ? IDE_CMD_RDMUL_EXT : IDE_CMD_RDMUL;
    else
      command = ext ? IDE_CMD_READ_EXT : IDE_CMD_READ;
    outb(channel->command_base + REG_COMMAND, command);
  }
  return 0;
}

static void ide_next(struct idechannel *channel) {
  struct buf *b;

  while (!asleep && (b = channel->queue) != 0) {
    if (wdc_start(channel, b) == 0) {
      channel->active = 1;
      return;
    }
    channel->queue = b->qnext;
    b->flags |= B_ERROR;
    wakeup(b);
  }
}

// one irq = one block; 0 if not our channel
int wdc_intr(int channel_index) {
  struct idechannel *channel;
  struct buf *buffer;
  uint n;

  if (channel_index < 0 || channel_index >= IDE_CHANNELS)
    return 0;
  channel = &channels[channel_index];
  mtx_enter(&channel->lock);
  if ((buffer = channel->queue) == 0 || !channel->active) {
    mtx_leave(&channel->lock);
    return 0;
  }
  if (!channel->dma && (inb(channel->control_base) & SR_BSY)) {
    mtx_leave(&channel->lock);
    return 0;
  }
  if (buffer->flags & B_FLUSH) {
    if (idewait(channel, 1) < 0)
      buffer->flags |= B_ERROR;
    else
      buffer->flags |= B_VALID;
  } else if (channel->dma) {
    int c = channel_index;
    int status = inb(channel->bm_base + BM_STATUS);

    if (!(status & BM_STATUS_INTR) || ((inb(channel->control_base) & SR_BSY) &&
                                       (delay(100), inb(channel->control_base) & SR_BSY))) {
      mtx_leave(&channel->lock);
      return 0;
    }
    outb(channel->bm_base + BM_COMMAND, 0);
    outb(channel->bm_base + BM_STATUS, BM_STATUS_INTR | BM_STATUS_ERROR);
    if ((status & BM_STATUS_ERROR) != 0 || idewait(channel, 1) < 0) {
      buffer->flags |= B_ERROR;
    } else {
      if ((buffer->flags & B_DIRTY) == 0)
        memmove(bufdata(buffer), bounce[c], channel->total * SECTOR_SIZE);
      buffer->flags |= B_VALID;
    }
  } else if (idewait(channel, 1) < 0) {
    buffer->flags |= B_ERROR;
  } else if (buffer->flags & B_DIRTY) {
    if (channel->xfer < channel->total) {
      if (idepush(channel, buffer) == 0) {
        mtx_leave(&channel->lock);
        return 1;
      }
      buffer->flags |= B_ERROR;
    } else {
      buffer->flags |= B_VALID;
    }
  } else {
    n = channel->total - channel->xfer;
    if (n > channel->block)
      n = channel->block;
    insl(channel->command_base + REG_DATA,
         bufdata(buffer) + channel->xfer * SECTOR_SIZE,
         n * SECTOR_SIZE / 4);
    channel->xfer += n;
    if (channel->xfer < channel->total) {
      mtx_leave(&channel->lock);
      return 1;
    }
    buffer->flags |= B_VALID;
  }
  channel->queue = buffer->qnext;
  channel->active = 0;
  if ((buffer->flags & (B_DIRTY | B_ERROR)) == B_DIRTY)
    disk_dirty[minor(buffer->dev)] = 1;
  buffer->flags &= ~B_DIRTY;
  wakeup(buffer);
  ide_next(channel);
  mtx_leave(&channel->lock);
  return 1;
}

static void ide_flush_quiet(void) {
  uint unit;
  int ms;

  for (unit = 0; unit < IDE_DEVICES; unit++) {
    struct idechannel *channel = &channels[channel_for(unit)];

    if (disk_sectors[unit] == 0 || disk_flush[unit] == 0 || !disk_dirty[unit])
      continue;
    outb(channel->control_base, CTL_NIEN);
    ideselect(channel, unit_for(unit), 0);
    if (idewait(channel, 0) == 0) {
      outb(channel->command_base + REG_COMMAND, disk_flush[unit]);
      for (ms = 0; ms < IDE_FLUSH_MS &&
                   (inb(channel->command_base + REG_STATUS) & SR_BSY);
           ms++)
        delay(1000);
      if (idewait(channel, 1) == 0)
        disk_dirty[unit] = 0;
    }
    if (disk_dirty[unit])
      printf("IDE: drive %d flush cache command failed\n", unit);
  }
}

// SLEEP
static int ide_suspend(void) {
  int c, i;

  asleep = 1;
  for (i = 0; i < 5000; i++) {
    if (!channels[0].active && !channels[1].active) {
      ide_flush_quiet();
      return 0;
    }
    delay(1000);
  }
  asleep = 0;
  for (c = 0; c < IDE_CHANNELS; c++) {
    mtx_enter(&channels[c].lock);
    if (!channels[c].active)
      ide_next(&channels[c]);
    mtx_leave(&channels[c].lock);
  }
  return -1;
}

static void ide_resume(void) {
  uint device;
  int c, i;

  for (c = 0; c < IDE_CHANNELS; c++) {
    if (disk_sectors[2 * c] == 0 && disk_sectors[2 * c + 1] == 0)
      continue;
    outb(channels[c].control_base, CTL_SRST | CTL_NIEN);
    delay(10);
    outb(channels[c].control_base, CTL_NIEN);
    for (i = 0; i < 31000 && (inb(channels[c].command_base + REG_STATUS) & SR_BSY); i++)
      delay(1000);
  }
  for (device = 0; device < IDE_DEVICES; device++)
    if (disk_sectors[device] != 0 && ideidentify(device) != disk_sectors[device])
      printf("IDE: drive %d is not what it was before the sleep\n", device);
  asleep = 0;
  for (c = 0; c < IDE_CHANNELS; c++) {
    mtx_enter(&channels[c].lock);
    channels[c].active = 0;
    ide_next(&channels[c]);
    mtx_leave(&channels[c].lock);
  }
}

// FLUSH CACHE
static int ide_flush(uint unit) {
  struct idechannel *channel;
  struct buf flush, **cursor;
  int r;

  if (unit >= IDE_DEVICES || disk_sectors[unit] == 0 || disk_flush[unit] == 0 ||
      !disk_dirty[unit])
    return 0;
  disk_dirty[unit] = 0;
  memset(&flush, 0, sizeof(flush));
  initsleeplock(&flush.lock, "ideflush");
  acquiresleep(&flush.lock);
  flush.flags = B_FLUSH;
  flush.dev = makedev(ide_major, unit);
  channel = &channels[channel_for(unit)];
  mtx_enter(&channel->lock);
  for (cursor = &channel->queue; *cursor; cursor = &(*cursor)->qnext)
    ;
  *cursor = &flush;
  if (channel->queue == &flush)
    ide_next(channel);
  while ((flush.flags & (B_VALID | B_ERROR)) == 0)
    msleep(&flush, &channel->lock, PRIBIO, "ideflush", 0);
  mtx_leave(&channel->lock);
  releasesleep(&flush.lock);
  r = (flush.flags & B_ERROR) ? -1 : 0;
  if (r < 0) {
    printf("IDE: drive %d flush cache command failed\n", unit);
    disk_dirty[unit] = 1;
  }
  return r;
}

void wd_strategy(struct buf *buffer) {
  struct idechannel *channel;
  struct buf **cursor;
  uint unit;

  if (!holdingsleep(&buffer->lock))
    panic("wd_strategy: buf not locked");
  if ((buffer->flags & (B_VALID | B_DIRTY)) == B_VALID)
    panic("wd_strategy: nothing to do");
  unit = minor(buffer->dev);
  if (unit >= IDE_DEVICES || disk_sectors[unit] == 0 ||
      bufcount(buffer) % SECTOR_SIZE != 0 ||
      bufcount(buffer) > ide_maxphys(unit) ||
      buffer->blockno * (BSIZE / SECTOR_SIZE) +
              bufcount(buffer) / SECTOR_SIZE >
          disk_sectors[unit]) {
    buffer->flags |= B_ERROR;
    return;
  }

  channel = &channels[channel_for(unit)];
  mtx_enter(&channel->lock);
  buffer->qnext = 0;
  for (cursor = &channel->queue; *cursor; cursor = &(*cursor)->qnext)
    ;
  *cursor = buffer;
  if (channel->queue == buffer)
    ide_next(channel);
  while ((buffer->flags & B_ERROR) == 0 &&
         (buffer->flags & (B_VALID | B_DIRTY)) != B_VALID)
    msleep(buffer, &channel->lock, PRIBIO, "ide", 0);
  mtx_leave(&channel->lock);
}
