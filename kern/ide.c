// PIO ATA transport for up to four QEMU IDE devices. Devices 0/1 occupy the
// primary channel; devices 2/3 occupy the secondary channel. The legacy root
// remains device 1, while device 2 is reserved for optional FNU Data media.
#include "buf.h"
#include "defs.h"
#include "proc.h"
#include "spinlock.h"
#include "trap.h"
#include "x86.h"

#define SECTOR_SIZE 512
#define IDE_CHANNELS 2
#define IDE_DEVICES 4

#define SR_BSY 0x80
#define SR_DRDY 0x40
#define SR_DWF 0x20
#define SR_DRQ 0x08
#define SR_ERR 0x01

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
#define IDE_CMD_WRITE 0x30
#define IDE_CMD_RDMUL 0xc4
#define IDE_CMD_WRMUL 0xc5
#define IDE_CMD_IDENTIFY 0xec

struct idechannel {
  struct spinlock lock;
  struct buf *queue;
  ushort command_base;
  ushort control_base;
};

static struct idechannel channels[IDE_CHANNELS] = {
    {{0}, 0, IDE_PRIMARY_COMMAND, IDE_PRIMARY_CONTROL},
    {{0}, 0, IDE_SECONDARY_COMMAND, IDE_SECONDARY_CONTROL},
};
static uint64_t disk_sectors[IDE_DEVICES];

static int channel_for(uint device) { return device / 2; }
static int unit_for(uint device) { return device & 1; }

static int idewait(struct idechannel *channel, int checkerr) {
  int status;

  while (((status = inb(channel->command_base + REG_STATUS)) &
          (SR_BSY | SR_DRDY)) != SR_DRDY)
    ;
  if (checkerr && (status & (SR_DWF | SR_ERR)) != 0)
    return -1;
  return 0;
}

static uint64_t ideidentify(uint device) {
  struct idechannel *channel;
  uint identify[128];
  int status, spins;

  if (device >= IDE_DEVICES)
    return 0;
  channel = &channels[channel_for(device)];
  outb(channel->command_base + REG_HDDEVSEL, 0xe0 | (unit_for(device) << 4));
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
  return (uint64_t)identify[30] | ((uint64_t)identify[31] << 32);
}

uint64_t ide_size(uint device) {
  return device < IDE_DEVICES ? disk_sectors[device] : 0;
}

void ideinit(void) {
  uint device;

  initlock(&channels[0].lock, "ide0");
  initlock(&channels[1].lock, "ide1");
  ioapicenable(IRQ_IDE_PRIMARY, ncpu - 1);
  ioapicenable(IRQ_IDE_SECONDARY, ncpu - 1);
  for (device = 0; device < IDE_DEVICES; device++)
    disk_sectors[device] = ideidentify(device);
  cprintf("IDE: primary %d/%d sectors, secondary %d/%d sectors\n",
          (uint)disk_sectors[0], (uint)disk_sectors[1],
          (uint)disk_sectors[2], (uint)disk_sectors[3]);
}

static void idestart(struct idechannel *channel, struct buf *buffer) {
  uint sectors_per_block = BSIZE / SECTOR_SIZE;
  uint sector = buffer->blockno * sectors_per_block;
  int command = sectors_per_block == 1 ? IDE_CMD_READ : IDE_CMD_RDMUL;

  if (sectors_per_block == 0 || sectors_per_block > 7)
    panic("idestart");
  if (buffer->blockno > 0x0fffffffU / sectors_per_block)
    panic("idestart: LBA28 overflow");
  idewait(channel, 0);
  outb(channel->control_base, 0);
  outb(channel->command_base + REG_SECCOUNT0, sectors_per_block);
  outb(channel->command_base + REG_LBA0, sector & 0xff);
  outb(channel->command_base + REG_LBA1, (sector >> 8) & 0xff);
  outb(channel->command_base + REG_LBA2, (sector >> 16) & 0xff);
  outb(channel->command_base + REG_HDDEVSEL,
       0xe0 | (unit_for(buffer->dev) << 4) | ((sector >> 24) & 0x0f));
  if (buffer->flags & B_DIRTY) {
    command = sectors_per_block == 1 ? IDE_CMD_WRITE : IDE_CMD_WRMUL;
    outb(channel->command_base + REG_COMMAND, command);
    outsl(channel->command_base + REG_DATA, buffer->data, BSIZE / 4);
  } else {
    outb(channel->command_base + REG_COMMAND, command);
  }
}

void ideintr(int channel_index) {
  struct idechannel *channel;
  struct buf *buffer;

  if (channel_index < 0 || channel_index >= IDE_CHANNELS)
    return;
  channel = &channels[channel_index];
  acquire(&channel->lock);
  if ((buffer = channel->queue) == 0) {
    release(&channel->lock);
    return;
  }
  channel->queue = buffer->qnext;
  if (!(buffer->flags & B_DIRTY) && idewait(channel, 1) >= 0)
    insl(channel->command_base + REG_DATA, buffer->data, BSIZE / 4);
  buffer->flags |= B_VALID;
  buffer->flags &= ~B_DIRTY;
  wakeup(buffer);
  if (channel->queue != 0)
    idestart(channel, channel->queue);
  release(&channel->lock);
}

void iderw(struct buf *buffer) {
  struct idechannel *channel;
  struct buf **cursor;

  if (!holdingsleep(&buffer->lock))
    panic("iderw: buf not locked");
  if ((buffer->flags & (B_VALID | B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if (buffer->dev >= IDE_DEVICES || disk_sectors[buffer->dev] == 0)
    panic("iderw: disk not present");
  if ((uint64_t)buffer->blockno >= disk_sectors[buffer->dev] / (BSIZE / SECTOR_SIZE))
    panic("iderw: sector beyond disk");

  channel = &channels[channel_for(buffer->dev)];
  acquire(&channel->lock);
  buffer->qnext = 0;
  for (cursor = &channel->queue; *cursor; cursor = &(*cursor)->qnext)
    ;
  *cursor = buffer;
  if (channel->queue == buffer)
    idestart(channel, buffer);
  while ((buffer->flags & (B_VALID | B_DIRTY)) != B_VALID)
    sleep(buffer, &channel->lock);
  release(&channel->lock);
}
