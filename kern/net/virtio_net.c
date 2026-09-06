// Legacy VirtIO PCI transport for the QEMU reference NIC.
#include "adapter.h"
#include "defs.h"
#include "driver.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "spinlock.h"
#include "trap.h"
#include "x86.h"

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
  char transmit_buffers[VIRTQ_SIZE][PRONINX_NET_FRAME_MAX + VIRTIO_NET_HEADER_SIZE];
  uchar transmit_inflight[VIRTQ_SIZE];
} virtio_net;

static void barrier(void) { __sync_synchronize(); }

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

static void virtio_net_intr_cb(struct device *dev) {
  (void)dev;
  if (!virtio_net.present)
    return;
  if (inb(virtio_net.io_base + VIRTIO_ISR) & 1)
    virtio_net_service();
}

static void virtio_net_poll_cb(struct device *dev) {
  (void)dev;
  virtio_net_service();
}

static void virtio_net_fail(void) {
  outb(virtio_net.io_base + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FAILED);
}

static int virtio_net_probe(struct device *dev) {
  if (!dev || dev->bus != BUS_TYPE_PCI)
    return 0;
  if (dev->pci.vendor_id == PCI_VENDOR_VIRTIO &&
      dev->pci.device_id == PCI_DEVICE_VIRTIO_NET) {
    return 50;
  }
  return 0;
}

static int virtio_net_attach(struct device *dev) {
  uint i;
  if (!dev || !dev->io_base)
    return -1;

  initlock(&virtio_net.lock, "virtio-net");
  virtio_net.io_base = dev->io_base;
  virtio_net.irq = dev->irq;
  safestrcpy(dev->name, "vtnet0", sizeof(dev->name));

  outb(virtio_net.io_base + VIRTIO_STATUS, 0);
  outb(virtio_net.io_base + VIRTIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
  outb(virtio_net.io_base + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
  if ((inl(virtio_net.io_base + VIRTIO_HOST_FEATURES) & (1U << VIRTIO_NET_F_MAC)) == 0) {
    cprintf("NET: virtio-net device has no MAC address feature\n");
    virtio_net_fail();
    return -1;
  }
  outl(virtio_net.io_base + VIRTIO_GUEST_FEATURES, 1U << VIRTIO_NET_F_MAC);
  if (queue_setup(&virtio_net.receive, virtio_net.receive_ring, 0) < 0 ||
      queue_setup(&virtio_net.transmit, virtio_net.transmit_ring, 1) < 0) {
    virtio_net_fail();
    return -1;
  }
  for (i = 0; i < VIRTQ_SIZE; i++) {
    if ((virtio_net.receive_buffers[i] = kalloc()) == 0) {
      virtio_net_fail();
      return -1;
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
  virtio_net.interface.driver_context = dev;
  if (proninx_net_register(&virtio_net.interface) < 0) {
    virtio_net_fail();
    return -1;
  }
  virtio_net.present = 1;
  outb(virtio_net.io_base + VIRTIO_STATUS,
       VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_DRIVER_OK);
  cprintf("NET: virtio-net interface vtnet0 ready (IRQ %d)\n", virtio_net.irq);
  return 0;
}

static struct driver virtio_net_driver = {
  .name = "virtio_net",
  .bus_type = BUS_TYPE_PCI,
  .dev_type = DEVICE_TYPE_NET,
  .probe = virtio_net_probe,
  .attach = virtio_net_attach,
  .detach = 0,
  .intr = virtio_net_intr_cb,
  .poll = virtio_net_poll_cb,
};

void virtio_net_driver_init(void) {
  driver_register(&virtio_net_driver);
}
