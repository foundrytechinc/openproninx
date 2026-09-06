#ifndef PRONINX_PCI_H
#define PRONINX_PCI_H

#include "inc/types.h"

#define PCI_CONFIG_ADDRESS 0x0cf8
#define PCI_CONFIG_DATA    0x0cfc

// Common PCI Configuration Registers
#define PCI_REG_VENDOR_ID      0x00
#define PCI_REG_DEVICE_ID      0x02
#define PCI_REG_COMMAND        0x04
#define PCI_REG_STATUS         0x06
#define PCI_REG_REVISION_ID    0x08
#define PCI_REG_PROG_IF        0x09
#define PCI_REG_SUBCLASS       0x0a
#define PCI_REG_CLASS          0x0b
#define PCI_REG_HEADER_TYPE    0x0e
#define PCI_REG_BAR0           0x10
#define PCI_REG_BAR1           0x14
#define PCI_REG_BAR2           0x18
#define PCI_REG_BAR3           0x1c
#define PCI_REG_BAR4           0x20
#define PCI_REG_BAR5           0x24
#define PCI_REG_INTERRUPT_LINE 0x3c
#define PCI_REG_INTERRUPT_PIN  0x3d

// Command Register Bits
#define PCI_COMMAND_IO         0x0001
#define PCI_COMMAND_MEMORY     0x0002
#define PCI_COMMAND_MASTER     0x0004

// Header Types
#define PCI_HEADER_TYPE_MULTIFUNC 0x80

// Well-known Vendor / Device IDs
#define PCI_VENDOR_INTEL        0x8086
#define PCI_DEVICE_E1000_82540EM 0x100e
#define PCI_DEVICE_E1000_82545EM 0x100f
#define PCI_DEVICE_E1000_82543GC 0x1004
#define PCI_DEVICE_E1000_82574L  0x10d3
#define PCI_DEVICE_E1000_82540EP 0x101e
#define PCI_DEVICE_E1000_82541EI 0x1013
#define PCI_DEVICE_E1000_82541ER 0x107c

#define PCI_VENDOR_VIRTIO       0x1af4
#define PCI_DEVICE_VIRTIO_NET   0x1000
#define PCI_DEVICE_VIRTIO_BLOCK 0x1001

// Storage Class Codes
#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_IDE        0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_SUBCLASS_NVME       0x08
#define PCI_PROGIF_SATA_AHCI    0x01
#define PCI_PROGIF_NVME         0x02

struct pci_device {
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t  bus;
  uint8_t  device;
  uint8_t  function;
  uint8_t  irq;
  uint8_t  class_code;
  uint8_t  subclass;
  uint8_t  prog_if;
  uint16_t io_base;
  uint32_t mmio_base;
  uint32_t mmio_size;
  uint32_t bar[6];
  uint32_t bar_size[6];
  uint8_t  bar_is_io[6];
};

// Low-level Configuration Space Access
uint32_t pci_read32(uint bus, uint dev, uint func, uint offset);
uint16_t pci_read16(uint bus, uint dev, uint func, uint offset);
uint8_t  pci_read8(uint bus, uint dev, uint func, uint offset);
void     pci_write32(uint bus, uint dev, uint func, uint offset, uint32_t value);
void     pci_write16(uint bus, uint dev, uint func, uint offset, uint16_t value);
void     pci_write8(uint bus, uint dev, uint func, uint offset, uint8_t value);

// Device Discovery & Control
int  pci_find_device(uint16_t vendor_id, uint16_t device_id, struct pci_device *out);
int  pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, struct pci_device *out);
void pci_enable_bus_master(const struct pci_device *dev);

#endif /* PRONINX_PCI_H */
