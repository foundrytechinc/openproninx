// Legacy VirtIO PCI transport for the QEMU reference NIC.  This is PRONINX
// driver code; it feeds the PRONINX adapter, not FreeBSD internals.
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
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MASTER 0x0004
#define PCI_INTERRUPT_LINE 0x3c

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
#define VIRTIO_STATUS_FAILED 128
#define VIRTIO_NET_F_MAC 5
#define VIRTIO_DEVICE_CONFIG 20

#define VIRTQ_SIZE 8
#define VIRTQ_ALIGN PGSIZE
#define VIRTIO_NET_HEADER_SIZE 10
#define VRING_DESC_F_WRITE 2

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

struct virtio_pci_device {
  ushort io_base;
  uchar irq;
  uchar bus;
  uchar device;
  uchar function;
};

static struct {
  struct spinlock lock;
  ushort io_base;
  uchar irq;
  int present;
  struct virtqueue receive;
  struct virtqueue transmit;
  struct proninx_net_interface interface;
  char receive_ring[VIRTQ_ALIGN * 2] __attribute__((aligned(VIRTQ_ALIGN)));
  char transmit_ring[VIRTQ_ALIGN * 2] __attribute__((aligned(VIRTQ_ALIGN)));
  char *receive_buffers[VIRTQ_SIZE];
  /* One private buffer per descriptor: callers may transmit again before the
     device completes the previous frame. */
  char transmit_buffers[VIRTQ_SIZE][PRONINX_NET_FRAME_MAX + VIRTIO_NET_HEADER_SIZE];
  uchar transmit_inflight[VIRTQ_SIZE];
} virtio_net;

static void barrier(void) { __sync_synchronize(); }

static uint pci_read(uint bus, uint device, uint function, uint offset) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (device << 11) |
                               (function << 8) | (offset & 0xfc));
  return inl(PCI_CONFIG_DATA);
}

static void pci_write16(uint bus, uint device, uint function, uint offset,
                        ushort value) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (device << 11) |
                               (function << 8) | (offset & 0xfc));
  outw(PCI_CONFIG_DATA + (offset & 2), value);
}

static int find_virtio_net(struct virtio_pci_device *found) {
  uint bus, device, function;
  for (bus = 0; bus < 256; bus++) {
    for (device = 0; device < 32; device++) {
      for (function = 0; function < 8; function++) {
        uint id = pci_read(bus, device, function, 0);
        if ((id & 0xffff) == PCI_VENDOR_VIRTIO &&
            (id >> 16) == PCI_DEVICE_VIRTIO_NET) {
          uint bar = pci_read(bus, device, function, 0x10);
          uint irq = pci_read(bus, device, function, PCI_INTERRUPT_LINE);
          if ((bar & 1) && (irq & 0xff) < IRQ_SPURIOUS) {
            found->io_base = (ushort)(bar & ~3U);
            found->irq = (uchar)irq;
            found->bus = bus;
            found->device = device;
            found->function = function;
            return 0;
          }
        }
        if (function == 0 && (pci_read(bus, device, 0, 0x0c) & 0x00800000) == 0)
          break;
      }
    }
  }
  return -1;
}

static void queue_select(ushort index) { outw(virtio_net.io_base + VIRTIO_QUEUE_SEL, index); }

static int queue_setup(struct virtqueue *queue, char *memory, ushort index) {
  queue_select(index);
  outl(virtio_net.io_base + VIRTIO_QUEUE_PFN, 0);
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
  // The device owns receive buffers and writes the virtio header plus frame.
  queue->descriptors[index].flags = VRING_DESC_F_WRITE;
  queue->descriptors[index].next = 0;
  barrier();
  queue->available->ring[queue->available->index % VIRTQ_SIZE] = index;
  barrier();
  queue->available->index++;
}

static int virtio_net_transmit(struct proninx_net_interface *interface,
                               const void *frame, uint length) {
  struct virtqueue *queue = &virtio_net.transmit;
  uint index;
  (void)interface;
  if (length > PRONINX_NET_FRAME_MAX)
    return -1;
  acquire(&virtio_net.lock);
  for (index = 0; index < VIRTQ_SIZE; index++)
    if (!virtio_net.transmit_inflight[index]) break;
  if (index == VIRTQ_SIZE) {
    release(&virtio_net.lock);
    return -1;
  }
  memset(virtio_net.transmit_buffers[index], 0, VIRTIO_NET_HEADER_SIZE);
  memmove(virtio_net.transmit_buffers[index] + VIRTIO_NET_HEADER_SIZE, frame, length);
  queue->descriptors[index].address = V2P(virtio_net.transmit_buffers[index]);
  queue->descriptors[index].length = length + VIRTIO_NET_HEADER_SIZE;
  queue->descriptors[index].flags = 0;
  queue->descriptors[index].next = 0;
  barrier();
  queue->available->ring[queue->available->index % VIRTQ_SIZE] = index;
  barrier();
  queue->available->index++;
  virtio_net.transmit_inflight[index] = 1;
  queue_notify(1);
  release(&virtio_net.lock);
  return 0;
}

static void virtio_net_service(void) {
  struct virtqueue *queue;
  int receive_refilled = 0;
  if (!virtio_net.present)
    return;
  acquire(&virtio_net.lock);
  // Reclaim TX first: an incoming DHCP or ARP packet can cause lwIP to send
  // immediately, and the previous packet may have completed in this IRQ.
  queue = &virtio_net.transmit;
  while (queue->used_index != queue->used->index) {
    uint id = queue->used->ring[queue->used_index % VIRTQ_SIZE].id;
    if (id < VIRTQ_SIZE)
      virtio_net.transmit_inflight[id] = 0;
    queue->used_index++;
  }
  queue = &virtio_net.receive;
  while (queue->used_index != queue->used->index) {
    struct vring_used_element *used = &queue->used->ring[queue->used_index % VIRTQ_SIZE];
    uint id = used->id;
    uint length = used->length;
    if (id < VIRTQ_SIZE && length > VIRTIO_NET_HEADER_SIZE && length <= PGSIZE) {
      // lwIP may synchronously emit ARP or ICMP through this driver's TX path.
      // Do not invoke it while holding the VirtIO lock.
      release(&virtio_net.lock);
      proninx_net_receive(&virtio_net.interface,
                          virtio_net.receive_buffers[id] + VIRTIO_NET_HEADER_SIZE,
                          length - VIRTIO_NET_HEADER_SIZE);
      acquire(&virtio_net.lock);
      queue = &virtio_net.receive;
    }
    if (id < VIRTQ_SIZE)
      receive_refill(id);
    if (id < VIRTQ_SIZE)
      receive_refilled = 1;
    queue->used_index++;
  }
  if (receive_refilled)
    queue_notify(0);
  release(&virtio_net.lock);
}

void virtio_net_intr(void) {
  if (!virtio_net.present)
    return;
  /* Reading ISR acknowledges a legacy VirtIO PCI interrupt. */
  if (inb(virtio_net.io_base + VIRTIO_ISR) & 1)
    virtio_net_service();
}

void virtio_net_poll(void) {
  virtio_net_service();
}

int virtio_net_handles_irq(int irq) {
  return virtio_net.present && irq == virtio_net.irq;
}

static void virtio_net_fail(void) {
  outb(virtio_net.io_base + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FAILED);
}

void virtio_net_init(void) {
  uint i;
  struct virtio_pci_device device;
  if (find_virtio_net(&device) < 0) {
    cprintf("NET: no legacy virtio-net PCI device\n");
    return;
  }
  initlock(&virtio_net.lock, "virtio-net");
  virtio_net.io_base = device.io_base;
  virtio_net.irq = device.irq;
  pci_write16(device.bus, device.device, device.function, 0x04,
              (ushort)pci_read(device.bus, device.device, device.function,
                               0x04) |
                  PCI_COMMAND_IO | PCI_COMMAND_MASTER);
  outb(virtio_net.io_base + VIRTIO_STATUS, 0);
  outb(virtio_net.io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
  outb(virtio_net.io_base + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
  if ((inl(virtio_net.io_base + VIRTIO_HOST_FEATURES) & (1U << VIRTIO_NET_F_MAC)) ==
      0) {
    cprintf("NET: virtio-net device has no MAC address feature\n");
    virtio_net_fail();
    return;
  }
  outl(virtio_net.io_base + VIRTIO_GUEST_FEATURES, 1U << VIRTIO_NET_F_MAC);
  if (queue_setup(&virtio_net.receive, virtio_net.receive_ring, 0) < 0 ||
      queue_setup(&virtio_net.transmit, virtio_net.transmit_ring, 1) < 0) {
    virtio_net_fail();
    return;
  }
  for (i = 0; i < VIRTQ_SIZE; i++) {
    if ((virtio_net.receive_buffers[i] = kalloc()) == 0) {
      virtio_net_fail();
      return;
    }
    receive_refill(i);
  }
  queue_notify(0);
  safestrcpy(virtio_net.interface.name, "vtnet0", sizeof(virtio_net.interface.name));
  virtio_net.interface.mtu = PRONINX_NET_MTU;
  for (i = 0; i < sizeof(virtio_net.interface.hardware_address); i++)
    virtio_net.interface.hardware_address[i] =
        inb(virtio_net.io_base + VIRTIO_DEVICE_CONFIG + i);
  virtio_net.interface.transmit = virtio_net_transmit;
  if (proninx_net_register(&virtio_net.interface) < 0) {
    virtio_net_fail();
    return;
  }
  virtio_net.present = 1;
  ioapicenable(virtio_net.irq, 0);
  outb(virtio_net.io_base + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_DRIVER_OK);
  cprintf("NET: virtio-net interface vtnet0 ready (IRQ %d)\n", virtio_net.irq);
}
