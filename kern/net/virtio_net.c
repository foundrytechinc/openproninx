// Legacy VirtIO 1.0 PCI transport for the QEMU reference NIC.  This is
// PRONINX driver code; it feeds the PRONINX adapter, not FreeBSD internals.
#include "adapter.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "trap.h"
#include "x86.h"

#define PCI_CONFIG_ADDRESS 0xcf8
#define PCI_CONFIG_DATA 0xcfc
#define PCI_VENDOR_VIRTIO 0x1af4
#define PCI_DEVICE_VIRTIO_NET 0x1000

#define VIRTIO_HOST_FEATURES 0
#define VIRTIO_GUEST_FEATURES 4
#define VIRTIO_QUEUE_PFN 8
#define VIRTIO_QUEUE_NUM 12
#define VIRTIO_QUEUE_SEL 14
#define VIRTIO_QUEUE_NOTIFY 16
#define VIRTIO_STATUS 18
#define VIRTIO_ISR 19
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4

#define VIRTQ_SIZE 8
#define VIRTQ_ALIGN PGSIZE
#define VIRTIO_NET_HEADER_SIZE 10

struct vring_desc {
  uint64_t address;
  uint length;
  ushort flags;
  ushort next;
} __attribute__((packed));

struct vring_avail {
  ushort flags;
  ushort index;
  ushort ring[VIRTQ_SIZE];
} __attribute__((packed));

struct vring_used_element {
  uint id;
  uint length;
} __attribute__((packed));

struct vring_used {
  ushort flags;
  ushort index;
  struct vring_used_element ring[VIRTQ_SIZE];
} __attribute__((packed));

struct virtqueue {
  struct vring_desc *descriptors;
  struct vring_avail *available;
  struct vring_used *used;
  ushort used_index;
  ushort number;
};

static struct {
  struct spinlock lock;
  ushort io_base;
  int present;
  struct virtqueue receive;
  struct virtqueue transmit;
  struct proninx_net_interface interface;
  char receive_ring[VIRTQ_ALIGN * 2] __attribute__((aligned(VIRTQ_ALIGN)));
  char transmit_ring[VIRTQ_ALIGN * 2] __attribute__((aligned(VIRTQ_ALIGN)));
  char *receive_buffers[VIRTQ_SIZE];
  char transmit_buffer[PRONINX_NET_FRAME_MAX + VIRTIO_NET_HEADER_SIZE];
  int transmit_busy;
} virtio_net;

static void barrier(void) { __sync_synchronize(); }

static uint pci_read(uint bus, uint device, uint function, uint offset) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (device << 11) |
                               (function << 8) | (offset & 0xfc));
  return inl(PCI_CONFIG_DATA);
}

static ushort find_virtio_net(void) {
  uint bus, device, function;
  for (bus = 0; bus < 256; bus++) {
    for (device = 0; device < 32; device++) {
      for (function = 0; function < 8; function++) {
        uint id = pci_read(bus, device, function, 0);
        if ((id & 0xffff) == PCI_VENDOR_VIRTIO &&
            (id >> 16) == PCI_DEVICE_VIRTIO_NET) {
          uint bar = pci_read(bus, device, function, 0x10);
          if (bar & 1)
            return (ushort)(bar & ~3U);
        }
        if (function == 0 && (pci_read(bus, device, 0, 0x0c) & 0x00800000) == 0)
          break;
      }
    }
  }
  return 0;
}

static void queue_select(ushort index) { outw(virtio_net.io_base + VIRTIO_QUEUE_SEL, index); }

static int queue_setup(struct virtqueue *queue, char *memory, ushort index) {
  queue_select(index);
  queue->number = inw(virtio_net.io_base + VIRTIO_QUEUE_NUM);
  if (queue->number < VIRTQ_SIZE)
    return -1;
  queue->number = VIRTQ_SIZE;
  queue->descriptors = (struct vring_desc *)memory;
  queue->available = (struct vring_avail *)(memory + sizeof(struct vring_desc) * VIRTQ_SIZE);
  queue->used = (struct vring_used *)(memory + VIRTQ_ALIGN);
  queue->used_index = 0;
  memset(memory, 0, VIRTQ_ALIGN * 2);
  outl(virtio_net.io_base + VIRTIO_QUEUE_PFN, V2P(memory) / PGSIZE);
  return 0;
}

static void queue_notify(ushort index) { outw(virtio_net.io_base + VIRTIO_QUEUE_NOTIFY, index); }

static void receive_refill(uint index) {
  struct virtqueue *queue = &virtio_net.receive;
  queue->descriptors[index].address = V2P(virtio_net.receive_buffers[index]);
  queue->descriptors[index].length = PGSIZE;
  queue->descriptors[index].flags = 0;
  queue->descriptors[index].next = 0;
  barrier();
  queue->available->ring[queue->available->index % VIRTQ_SIZE] = index;
  barrier();
  queue->available->index++;
}

static int virtio_net_transmit(struct proninx_net_interface *interface,
                               const void *frame, uint length) {
  struct virtqueue *queue = &virtio_net.transmit;
  (void)interface;
  if (length > PRONINX_NET_FRAME_MAX - VIRTIO_NET_HEADER_SIZE)
    return -1;
  acquire(&virtio_net.lock);
  if (virtio_net.transmit_busy) {
    release(&virtio_net.lock);
    return -1;
  }
  memset(virtio_net.transmit_buffer, 0, VIRTIO_NET_HEADER_SIZE);
  memmove(virtio_net.transmit_buffer + VIRTIO_NET_HEADER_SIZE, frame, length);
  queue->descriptors[0].address = V2P(virtio_net.transmit_buffer);
  queue->descriptors[0].length = length + VIRTIO_NET_HEADER_SIZE;
  barrier();
  queue->available->ring[queue->available->index % VIRTQ_SIZE] = 0;
  barrier();
  queue->available->index++;
  virtio_net.transmit_busy = 1;
  queue_notify(1);
  release(&virtio_net.lock);
  return 0;
}

void virtio_net_intr(void) {
  struct virtqueue *queue;
  uchar isr;
  if (!virtio_net.present)
    return;
  isr = inb(virtio_net.io_base + VIRTIO_ISR);
  if ((isr & 1) == 0)
    return;
  acquire(&virtio_net.lock);
  queue = &virtio_net.receive;
  while (queue->used_index != queue->used->index) {
    struct vring_used_element *used = &queue->used->ring[queue->used_index % VIRTQ_SIZE];
    uint id = used->id;
    uint length = used->length;
    if (id < VIRTQ_SIZE && length > VIRTIO_NET_HEADER_SIZE && length <= PGSIZE)
      proninx_net_receive(&virtio_net.interface,
                          virtio_net.receive_buffers[id] + VIRTIO_NET_HEADER_SIZE,
                          length - VIRTIO_NET_HEADER_SIZE);
    if (id < VIRTQ_SIZE)
      receive_refill(id);
    queue->used_index++;
  }
  queue = &virtio_net.transmit;
  if (queue->used_index != queue->used->index) {
    queue->used_index = queue->used->index;
    virtio_net.transmit_busy = 0;
  }
  queue_notify(0);
  release(&virtio_net.lock);
}

void virtio_net_init(void) {
  uint i;
  ushort io_base = find_virtio_net();
  if (io_base == 0) {
    cprintf("NET: no legacy virtio-net PCI device\n");
    return;
  }
  initlock(&virtio_net.lock, "virtio-net");
  virtio_net.io_base = io_base;
  outb(io_base + VIRTIO_STATUS, 0);
  outb(io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
  outb(io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
  outl(io_base + VIRTIO_GUEST_FEATURES, 0);
  if (queue_setup(&virtio_net.receive, virtio_net.receive_ring, 0) < 0 ||
      queue_setup(&virtio_net.transmit, virtio_net.transmit_ring, 1) < 0)
    return;
  for (i = 0; i < VIRTQ_SIZE; i++) {
    if ((virtio_net.receive_buffers[i] = kalloc()) == 0)
      return;
    receive_refill(i);
  }
  queue_notify(0);
  safestrcpy(virtio_net.interface.name, "vtnet0", sizeof(virtio_net.interface.name));
  virtio_net.interface.mtu = PRONINX_NET_MTU;
  virtio_net.interface.transmit = virtio_net_transmit;
  if (proninx_net_register(&virtio_net.interface) < 0)
    return;
  virtio_net.present = 1;
  ioapicenable(IRQ_VIRTIO_NET, 0);
  outb(io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                                VIRTIO_STATUS_DRIVER_OK);
  cprintf("NET: virtio-net interface vtnet0 ready\n");
}
