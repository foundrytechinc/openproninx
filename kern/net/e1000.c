#include "e1000.h"
#include "adapter.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "x86.h"

static struct {
  struct spinlock lock;
  volatile uint32_t *mmio;
  struct device *dev;
  struct proninx_net_interface interface;

  // Transmit Ring
  struct e1000_tx_desc tx_ring[E1000_NUM_TX_DESC] __attribute__((aligned(E1000_RING_ALIGN)));
  char tx_buffers[E1000_NUM_TX_DESC][E1000_TX_BUF_SIZE];
  uint tx_tail;

  // Receive Ring
  struct e1000_rx_desc rx_ring[E1000_NUM_RX_DESC] __attribute__((aligned(E1000_RING_ALIGN)));
  char *rx_buffers[E1000_NUM_RX_DESC];
  uint rx_cur;

  int initialized;
} e1000_state;

static inline void e1000_write32(uint32_t reg, uint32_t value) {
  *(volatile uint32_t *)((uintptr_t)e1000_state.mmio + reg) = value;
}

static inline uint32_t e1000_read32(uint32_t reg) {
  return *(volatile uint32_t *)((uintptr_t)e1000_state.mmio + reg);
}

static uint16_t e1000_read_eeprom(uint8_t addr) {
  uint32_t eerd = (1U << 0) | ((uint32_t)addr << 8);
  e1000_write32(E1000_EERD, eerd);
  for (int i = 0; i < 10000; i++) {
    uint32_t val = e1000_read32(E1000_EERD);
    if (val & (1U << 4))
      return (uint16_t)((val >> 16) & 0xffff);
    microdelay(10);
  }
  return 0;
}

static void e1000_read_mac(uchar *mac) {
  uint32_t ral = e1000_read32(E1000_RAL);
  uint32_t rah = e1000_read32(E1000_RAH);

  if ((rah & (1U << 31)) && ral != 0) {
    mac[0] = (ral & 0xff);
    mac[1] = ((ral >> 8) & 0xff);
    mac[2] = ((ral >> 16) & 0xff);
    mac[3] = ((ral >> 24) & 0xff);
    mac[4] = (rah & 0xff);
    mac[5] = ((rah >> 8) & 0xff);
    return;
  }

  // Fallback to EEPROM
  uint16_t w0 = e1000_read_eeprom(0);
  uint16_t w1 = e1000_read_eeprom(1);
  uint16_t w2 = e1000_read_eeprom(2);

  mac[0] = (w0 & 0xff);
  mac[1] = ((w0 >> 8) & 0xff);
  mac[2] = (w1 & 0xff);
  mac[3] = ((w1 >> 8) & 0xff);
  mac[4] = (w2 & 0xff);
  mac[5] = ((w2 >> 8) & 0xff);
}

static int e1000_transmit(struct proninx_net_interface *iface,
                          const void *frame, uint length) {
  (void)iface;
  if (length > PRONINX_NET_FRAME_MAX || !e1000_state.initialized)
    return -1;

  acquire(&e1000_state.lock);

  uint tail = e1000_state.tx_tail;
  struct e1000_tx_desc *desc = &e1000_state.tx_ring[tail];

  // Wait / verify previous packet on this descriptor finished
  if (desc->cmd && !(desc->status & E1000_TXD_STAT_DD)) {
    release(&e1000_state.lock);
    return -1;
  }

  memmove(e1000_state.tx_buffers[tail], frame, length);
  desc->addr = V2P(e1000_state.tx_buffers[tail]);
  desc->length = length;
  desc->cso = 0;
  desc->css = 0;
  desc->special = 0;
  desc->status = 0;
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;

  __sync_synchronize();

  tail = (tail + 1) % E1000_NUM_TX_DESC;
  e1000_state.tx_tail = tail;
  e1000_write32(E1000_TDT, tail);

  release(&e1000_state.lock);
  return 0;
}

static void e1000_service(void) {
  if (!e1000_state.initialized)
    return;

  // Acknowledge interrupts
  e1000_read32(E1000_ICR);

  acquire(&e1000_state.lock);

  while (e1000_state.rx_ring[e1000_state.rx_cur].status & E1000_RXD_STAT_DD) {
    uint cur = e1000_state.rx_cur;
    struct e1000_rx_desc *desc = &e1000_state.rx_ring[cur];
    uint length = desc->length;

    if (length > 0 && (desc->status & E1000_RXD_STAT_EOP)) {
      release(&e1000_state.lock);
      proninx_net_receive(&e1000_state.interface, e1000_state.rx_buffers[cur], length);
      acquire(&e1000_state.lock);
    }

    desc->status = 0;
    __sync_synchronize();

    e1000_write32(E1000_RDT, cur);
    e1000_state.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
  }

  release(&e1000_state.lock);
}

static void e1000_intr(struct device *dev) {
  (void)dev;
  e1000_service();
}

static void e1000_poll(struct device *dev) {
  (void)dev;
  e1000_service();
}

static int e1000_probe(struct device *dev) {
  if (!dev || dev->bus != BUS_TYPE_PCI)
    return 0;

  if (dev->pci.vendor_id == PCI_VENDOR_INTEL) {
    switch (dev->pci.device_id) {
    case PCI_DEVICE_E1000_82540EM:
    case PCI_DEVICE_E1000_82545EM:
    case PCI_DEVICE_E1000_82543GC:
    case PCI_DEVICE_E1000_82574L:
    case PCI_DEVICE_E1000_82540EP:
    case PCI_DEVICE_E1000_82541EI:
    case PCI_DEVICE_E1000_82541ER:
      return 100; // High match priority for Intel NICs
    default:
      break;
    }
  }

  return 0;
}

static int e1000_attach(struct device *dev) {
  if (!dev || !dev->mmio_vaddr) {
    cprintf("E1000: error, missing MMIO mapping\n");
    return -1;
  }

  initlock(&e1000_state.lock, "e1000");
  e1000_state.dev = dev;
  e1000_state.mmio = (volatile uint32_t *)dev->mmio_vaddr;
  safestrcpy(dev->name, "em0", sizeof(dev->name));

  // Device reset
  e1000_write32(E1000_IMC, 0xffffffff);
  e1000_write32(E1000_CTRL, E1000_CTRL_RST);
  microdelay(20);
  e1000_write32(E1000_CTRL, 0);
  microdelay(20);
  e1000_write32(E1000_IMC, 0xffffffff);

  // Clear Multicast Table Array
  for (int i = 0; i < 128; i++) {
    e1000_write32(E1000_MTA + (i * 4), 0);
  }

  // Set link up and auto speed
  uint32_t ctrl = e1000_read32(E1000_CTRL);
  ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE | E1000_CTRL_FD;
  e1000_write32(E1000_CTRL, ctrl);

  // Setup MAC Address
  e1000_read_mac(e1000_state.interface.hardware_address);
  cprintf("E1000: MAC address %x:%x:%x:%x:%x:%x\n",
          (uint64_t)e1000_state.interface.hardware_address[0],
          (uint64_t)e1000_state.interface.hardware_address[1],
          (uint64_t)e1000_state.interface.hardware_address[2],
          (uint64_t)e1000_state.interface.hardware_address[3],
          (uint64_t)e1000_state.interface.hardware_address[4],
          (uint64_t)e1000_state.interface.hardware_address[5]);


  // Initialize TX descriptors
  memset(e1000_state.tx_ring, 0, sizeof(e1000_state.tx_ring));
  e1000_state.tx_tail = 0;
  uint64_t tx_base = V2P(e1000_state.tx_ring);
  e1000_write32(E1000_TDBAL, (uint32_t)(tx_base & 0xffffffff));
  e1000_write32(E1000_TDBAH, (uint32_t)(tx_base >> 32));
  e1000_write32(E1000_TDLEN, sizeof(e1000_state.tx_ring));
  e1000_write32(E1000_TDH, 0);
  e1000_write32(E1000_TDT, 0);
  e1000_write32(E1000_TIPG, 10 | (8 << 10) | (6 << 20));
  e1000_write32(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (15 << 4) | (64 << 12));

  // Initialize RX descriptors
  memset(e1000_state.rx_ring, 0, sizeof(e1000_state.rx_ring));
  e1000_state.rx_cur = 0;
  for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
    char *buf = kalloc();
    if (!buf) {
      cprintf("E1000: failed to allocate RX buffer\n");
      return -1;
    }
    e1000_state.rx_buffers[i] = buf;
    e1000_state.rx_ring[i].addr = V2P(buf);
    e1000_state.rx_ring[i].status = 0;
  }

  uint64_t rx_base = V2P(e1000_state.rx_ring);
  e1000_write32(E1000_RDBAL, (uint32_t)(rx_base & 0xffffffff));
  e1000_write32(E1000_RDBAH, (uint32_t)(rx_base >> 32));
  e1000_write32(E1000_RDLEN, sizeof(e1000_state.rx_ring));
  e1000_write32(E1000_RDH, 0);
  e1000_write32(E1000_RDT, E1000_NUM_RX_DESC - 1);
  e1000_write32(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

  // Register network interface
  safestrcpy(e1000_state.interface.name, "em0", sizeof(e1000_state.interface.name));
  e1000_state.interface.mtu = PRONINX_NET_MTU;
  e1000_state.interface.transmit = e1000_transmit;
  e1000_state.interface.driver_context = dev;

  if (proninx_net_register(&e1000_state.interface) < 0) {
    cprintf("E1000: network interface registration failed\n");
    return -1;
  }

  // Clear and enable interrupts
  e1000_read32(E1000_ICR);
  e1000_write32(E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_TXDW | E1000_ICR_LSC |
                            E1000_ICR_RXO | E1000_ICR_RXDMT0 | E1000_ICR_RXSEQ);

  e1000_state.initialized = 1;
  return 0;
}

static struct driver e1000_driver = {
  .name = "intel_e1000",
  .bus_type = BUS_TYPE_PCI,
  .dev_type = DEVICE_TYPE_NET,
  .probe = e1000_probe,
  .attach = e1000_attach,
  .detach = 0,
  .intr = e1000_intr,
  .poll = e1000_poll,
};

void e1000_driver_init(void) {
  driver_register(&e1000_driver);
}
