#include "driver.h"
#include "defs.h"
#include "spinlock.h"

static struct {
  struct spinlock lock;
  struct driver drivers[MAX_DRIVERS];
  int driver_count;
  struct device devices[MAX_DEVICES];
  int device_count;
} driver_mgr;

void driver_framework_init(void) {
  initlock(&driver_mgr.lock, "driver_mgr");
  driver_mgr.driver_count = 0;
  driver_mgr.device_count = 0;
  memset(driver_mgr.drivers, 0, sizeof(driver_mgr.drivers));
  memset(driver_mgr.devices, 0, sizeof(driver_mgr.devices));
  cprintf("DRIVER: framework initialized\n");
}

int driver_register(struct driver *drv) {
  if (!drv || !drv->name[0] || !drv->probe || !drv->attach)
    return -1;

  acquire(&driver_mgr.lock);
  if (driver_mgr.driver_count >= MAX_DRIVERS) {
    release(&driver_mgr.lock);
    cprintf("DRIVER: maximum driver limit reached\n");
    return -1;
  }

  driver_mgr.drivers[driver_mgr.driver_count++] = *drv;
  release(&driver_mgr.lock);
  cprintf("DRIVER: registered driver '%s'\n", drv->name);
  return 0;
}

static struct device *driver_allocate_device(void) {
  if (driver_mgr.device_count >= MAX_DEVICES)
    return 0;
  struct device *dev = &driver_mgr.devices[driver_mgr.device_count++];
  memset(dev, 0, sizeof(*dev));
  return dev;
}

static int pci_functions;

int driver_attach_pci_devices(void) {
  uint bus, dev_idx, func;
  int attached_count = 0;

  cprintf("DRIVER: probing PCI devices...\n");
  pci_functions = 0;

  for (bus = 0; bus < 256; bus++) {
    for (dev_idx = 0; dev_idx < 32; dev_idx++) {
      for (func = 0; func < 8; func++) {
        uint32_t id = pci_read32(bus, dev_idx, func, PCI_REG_VENDOR_ID);
        uint16_t vendor = (uint16_t)(id & 0xffff);
        uint16_t device_id = (uint16_t)(id >> 16);

        if (vendor == 0xffff || vendor == 0) {
          if (func == 0)
            break;
          continue;
        }
        pci_functions++;

        struct pci_device pci_dev;
        memset(&pci_dev, 0, sizeof(pci_dev));
        pci_dev.vendor_id = vendor;
        pci_dev.device_id = device_id;
        pci_dev.bus = (uint8_t)bus;
        pci_dev.device = (uint8_t)dev_idx;
        pci_dev.function = (uint8_t)func;
        pci_dev.irq = pci_read8(bus, dev_idx, func, PCI_REG_INTERRUPT_LINE);

        pci_dev.class_code = pci_read8(bus, dev_idx, func, PCI_REG_CLASS);
        pci_dev.subclass = pci_read8(bus, dev_idx, func, PCI_REG_SUBCLASS);
        pci_dev.prog_if = pci_read8(bus, dev_idx, func, PCI_REG_PROG_IF);

        pci_read_bars(bus, dev_idx, func, &pci_dev);

        if (pci_dev.bar_is_io[0]) {
          pci_dev.io_base = (uint16_t)pci_dev.bar[0];
          pci_dev.mmio_base = 0;
          pci_dev.mmio_size = 0;
        } else {
          pci_dev.io_base = 0;
          pci_dev.mmio_base = pci_dev.bar[0];
          pci_dev.mmio_size = pci_dev.bar_size[0];
        }

        // Try probing registered drivers
        int best_score = 0;
        struct driver *best_driver = 0;

        struct device temp_dev;
        memset(&temp_dev, 0, sizeof(temp_dev));
        temp_dev.bus = BUS_TYPE_PCI;
        temp_dev.pci = pci_dev;
        temp_dev.irq = pci_dev.irq;
        temp_dev.io_base = pci_dev.io_base;
        temp_dev.mmio_paddr = pci_dev.mmio_base;
        temp_dev.mmio_size = pci_dev.mmio_size;

        for (int i = 0; i < driver_mgr.driver_count; i++) {
          struct driver *drv = &driver_mgr.drivers[i];
          if (drv->bus_type == BUS_TYPE_PCI ||
              drv->bus_type == BUS_TYPE_UNKNOWN) {
            int score = drv->probe(&temp_dev);
            if (score > best_score) {
              best_score = score;
              best_driver = drv;
            }
          }
        }

        if (best_driver) {
          acquire(&driver_mgr.lock);
          struct device *dev = driver_allocate_device();
          if (!dev) {
            release(&driver_mgr.lock);
            cprintf("DRIVER: out of device slots\n");
            return attached_count;
          }

          *dev = temp_dev;
          dev->driver = best_driver;
          dev->type = best_driver->dev_type;

          if (!dev->pci.bar_is_io[0])
            dev->mmio_size = dev->pci.bar_size[0];

          if (dev->mmio_paddr != 0 && dev->mmio_size != 0) {
            dev->mmio_vaddr = ioremap(dev->mmio_paddr, dev->mmio_size);
          }

          pci_enable_bus_master(&dev->pci);

          if (dev->irq > 0 && dev->irq < 255) {
            ioapicenable(dev->irq, 0);
          }

          if (best_driver->attach(dev) == 0) {
            dev->active = 1;
            attached_count++;
            cprintf("DRIVER: attached device '%s' (driver '%s', IRQ %d)\n",
                    dev->name[0] ? dev->name : "unnamed", best_driver->name,
                    dev->irq);
          } else {
            cprintf("DRIVER: attach failed for device on PCI %d:%d.%d\n", bus,
                    dev_idx, func);
          }
          release(&driver_mgr.lock);
        }

        if (func == 0) {
          uint8_t header = pci_read8(bus, dev_idx, 0, PCI_REG_HEADER_TYPE);
          if ((header & PCI_HEADER_TYPE_MULTIFUNC) == 0)
            break;
        }
      }
    }
  }

  return attached_count;
}

int driver_dispatch_irq(int irq) {
  int handled = 0;
  for (int i = 0; i < driver_mgr.device_count; i++) {
    struct device *dev = &driver_mgr.devices[i];
    if (dev->active && dev->irq == irq && dev->driver && dev->driver->intr) {
      dev->driver->intr(dev);
      handled = 1;
    }
  }
  return handled;
}

int driver_device_count(void) { return driver_mgr.device_count; }

int driver_pci_function_count(void) { return pci_functions; }

// nothing masters the bus across a power change
void driver_shutdown_all(void) {
  for (int i = 0; i < driver_mgr.device_count; i++) {
    struct device *dev = &driver_mgr.devices[i];
    if (dev->active && dev->driver && dev->driver->shutdown)
      dev->driver->shutdown(dev);
  }
}

void driver_poll_all(void) {
  for (int i = 0; i < driver_mgr.device_count; i++) {
    struct device *dev = &driver_mgr.devices[i];
    if (dev->active && dev->driver && dev->driver->poll) {
      dev->driver->poll(dev);
    }
  }
}

struct device *driver_find_device(const char *name) {
  if (!name)
    return 0;
  for (int i = 0; i < driver_mgr.device_count; i++) {
    struct device *dev = &driver_mgr.devices[i];
    if (dev->active && strncmp(dev->name, name, DEVICE_NAME_MAX) == 0) {
      return dev;
    }
  }
  return 0;
}

struct device *driver_find_device_by_type(enum device_type type) {
  for (int i = 0; i < driver_mgr.device_count; i++) {
    struct device *dev = &driver_mgr.devices[i];
    if (dev->active && dev->type == type) {
      return dev;
    }
  }
  return 0;
}
