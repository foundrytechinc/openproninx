#include "pci.h"
#include "defs.h"
#include "trap.h"
#include "x86.h"

uint32_t pci_read32(uint bus, uint dev, uint func, uint offset) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (dev << 11) |
                               (func << 8) | (offset & 0xfc));
  return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint bus, uint dev, uint func, uint offset) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (dev << 11) |
                               (func << 8) | (offset & 0xfc));
  return inw(PCI_CONFIG_DATA + (offset & 2));
}

uint8_t pci_read8(uint bus, uint dev, uint func, uint offset) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (dev << 11) |
                               (func << 8) | (offset & 0xfc));
  return inb(PCI_CONFIG_DATA + (offset & 3));
}

void pci_write32(uint bus, uint dev, uint func, uint offset, uint32_t value) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (dev << 11) |
                               (func << 8) | (offset & 0xfc));
  outl(PCI_CONFIG_DATA, value);
}

void pci_write16(uint bus, uint dev, uint func, uint offset, uint16_t value) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (dev << 11) |
                               (func << 8) | (offset & 0xfc));
  outw(PCI_CONFIG_DATA + (offset & 2), value);
}

void pci_write8(uint bus, uint dev, uint func, uint offset, uint8_t value) {
  outl(PCI_CONFIG_ADDRESS, 0x80000000U | (bus << 16) | (dev << 11) |
                               (func << 8) | (offset & 0xfc));
  outb(PCI_CONFIG_DATA + (offset & 3), value);
}

static void pci_populate_device(uint bus, uint dev, uint func, uint16_t vendor,
                                uint16_t device, struct pci_device *out) {
  if (!out)
    return;

  out->vendor_id = vendor;
  out->device_id = device;
  out->bus = (uint8_t)bus;
  out->device = (uint8_t)dev;
  out->function = (uint8_t)func;
  out->irq = pci_read8(bus, dev, func, PCI_REG_INTERRUPT_LINE);
  out->class_code = pci_read8(bus, dev, func, PCI_REG_CLASS);
  out->subclass = pci_read8(bus, dev, func, PCI_REG_SUBCLASS);
  out->prog_if = pci_read8(bus, dev, func, PCI_REG_PROG_IF);

  for (int b = 0; b < 6; b++) {
    uint offset = PCI_REG_BAR0 + b * 4;
    uint32_t bar = pci_read32(bus, dev, func, offset);
    if (bar & 1) {
      out->bar_is_io[b] = 1;
      out->bar[b] = bar & ~3U;
      pci_write32(bus, dev, func, offset, 0xffffffff);
      uint32_t mask = pci_read32(bus, dev, func, offset);
      pci_write32(bus, dev, func, offset, bar);
      out->bar_size[b] = (~(mask & ~3U)) + 1;
    } else {
      out->bar_is_io[b] = 0;
      out->bar[b] = bar & ~0xfU;
      pci_write32(bus, dev, func, offset, 0xffffffff);
      uint32_t mask = pci_read32(bus, dev, func, offset);
      pci_write32(bus, dev, func, offset, bar);
      out->bar_size[b] = (~(mask & ~0xfU)) + 1;
    }
  }

  // Set legacy fields based on BAR0
  if (out->bar_is_io[0]) {
    out->io_base = (uint16_t)out->bar[0];
    out->mmio_base = 0;
    out->mmio_size = 0;
  } else {
    out->io_base = 0;
    out->mmio_base = out->bar[0];
    out->mmio_size = out->bar_size[0];
  }
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id,
                    struct pci_device *out) {
  uint bus, dev, func;

  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      for (func = 0; func < 8; func++) {
        uint32_t id = pci_read32(bus, dev, func, PCI_REG_VENDOR_ID);
        uint16_t vendor = (uint16_t)(id & 0xffff);
        uint16_t device = (uint16_t)(id >> 16);

        if (vendor == 0xffff || vendor == 0) {
          if (func == 0)
            break; // Skip non-existent device
          continue;
        }

        if (vendor == vendor_id && device == device_id) {
          pci_populate_device(bus, dev, func, vendor, device, out);
          return 0;
        }

        // If not multi-function, only test function 0
        if (func == 0) {
          uint8_t header = pci_read8(bus, dev, 0, PCI_REG_HEADER_TYPE);
          if ((header & PCI_HEADER_TYPE_MULTIFUNC) == 0)
            break;
        }
      }
    }
  }
  return -1;
}

int pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                            struct pci_device *out) {
  uint bus, dev, func;

  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      for (func = 0; func < 8; func++) {
        uint32_t id = pci_read32(bus, dev, func, PCI_REG_VENDOR_ID);
        uint16_t vendor = (uint16_t)(id & 0xffff);
        uint16_t device = (uint16_t)(id >> 16);

        if (vendor == 0xffff || vendor == 0) {
          if (func == 0)
            break;
          continue;
        }

        uint8_t c = pci_read8(bus, dev, func, PCI_REG_CLASS);
        uint8_t s = pci_read8(bus, dev, func, PCI_REG_SUBCLASS);
        uint8_t p = pci_read8(bus, dev, func, PCI_REG_PROG_IF);

        if (c == class_code && s == subclass && (prog_if == 0xff || p == prog_if)) {
          pci_populate_device(bus, dev, func, vendor, device, out);
          return 0;
        }

        if (func == 0) {
          uint8_t header = pci_read8(bus, dev, 0, PCI_REG_HEADER_TYPE);
          if ((header & PCI_HEADER_TYPE_MULTIFUNC) == 0)
            break;
        }
      }
    }
  }
  return -1;
}

void pci_enable_bus_master(const struct pci_device *dev) {
  if (!dev)
    return;
  uint16_t cmd = pci_read16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND);
  cmd |= (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
  pci_write16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND, cmd);
}
