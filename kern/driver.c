// DEVICES
#include "driver.h"
#include "defs.h"
#include "malloc.h"
#include "mutex.h"
#include "trap.h"

static int device_irq(void *arg);

static struct {
  struct mutex lock;
  struct driver *drivers, **drivers_tail;
  struct device *devices, **devices_tail;
  int ndevices;
} dm;

static int pci_functions;

void driver_framework_init(void) {
  mtx_init(&dm.lock, "devices");
  dm.drivers_tail = &dm.drivers;
  dm.devices_tail = &dm.devices;
}

int driver_register(struct driver *drv) {
  if (drv == 0 || drv->name == 0 || drv->name[0] == 0 || drv->probe == 0 ||
      drv->attach == 0)
    return -1;
  mtx_enter(&dm.lock);
  drv->next = 0;
  *dm.drivers_tail = drv;
  dm.drivers_tail = &drv->next;
  mtx_leave(&dm.lock);
  return 0;
}

// under the lock
static int unit_alloc(struct driver *drv) {
  struct device *d;
  int unit;

  for (unit = 0;; unit++) {
    for (d = dm.devices; d != 0; d = d->next)
      if (d->driver == drv && d->unit == unit)
        break;
    if (d == 0)
      return unit;
  }
}

static void device_add(struct device *dev) {
  mtx_enter(&dm.lock);
  dev->unit = unit_alloc(dev->driver);
  snprintf(dev->name, sizeof(dev->name), "%s%d", dev->driver->name, dev->unit);
  dev->next = 0;
  *dm.devices_tail = dev;
  dm.devices_tail = &dev->next;
  dm.ndevices++;
  mtx_leave(&dm.lock);
}

static void device_delete(struct device *dev) {
  struct device **pp;

  mtx_enter(&dm.lock);
  for (pp = &dm.devices; *pp != 0; pp = &(*pp)->next)
    if (*pp == dev) {
      *pp = dev->next;
      if (dm.devices_tail == &dev->next)
        dm.devices_tail = pp;
      dm.ndevices--;
      break;
    }
  mtx_leave(&dm.lock);
  free(dev, M_DEVBUF);
}

static void pci_function_read(struct pci_device *p, uint bus, uint slot,
                              uint func, uint32_t id) {
  memset(p, 0, sizeof(*p));
  p->vendor_id = (uint16_t)(id & 0xffff);
  p->device_id = (uint16_t)(id >> 16);
  p->bus = (uint8_t)bus;
  p->device = (uint8_t)slot;
  p->function = (uint8_t)func;
  p->irq = pci_read8(bus, slot, func, PCI_REG_INTERRUPT_LINE);
  p->class_code = pci_read8(bus, slot, func, PCI_REG_CLASS);
  p->subclass = pci_read8(bus, slot, func, PCI_REG_SUBCLASS);
  p->prog_if = pci_read8(bus, slot, func, PCI_REG_PROG_IF);
  pci_read_bars(bus, slot, func, p);
  if (p->bar_is_io[0])
    p->io_base = (uint16_t)p->bar[0];
  else {
    p->mmio_base = p->bar[0];
    p->mmio_size = p->bar_size[0];
  }
}

static int pci_function_attach(const struct pci_device *p, void *regs) {
  struct device *dev;
  struct driver *drv, *best = 0;
  int score, best_score = 0;

  dev = malloc(sizeof(*dev), M_DEVBUF, M_WAITOK | M_ZERO);
  dev->bus = BUS_TYPE_PCI;
  dev->pci = *p;
  dev->irq = p->irq;
  dev->io_base = p->io_base;
  dev->mmio_paddr = p->mmio_base;
  dev->mmio_size = p->mmio_size;
  for (drv = dm.drivers; drv != 0; drv = drv->next)
    if (drv->bus_type == BUS_TYPE_PCI || drv->bus_type == BUS_TYPE_UNKNOWN)
      if ((score = drv->probe(dev)) > best_score) {
        best_score = score;
        best = drv;
      }
  if (best == 0) {
    free(dev, M_DEVBUF);
    return 0;
  }
  dev->driver = best;
  dev->type = best->dev_type;
  device_add(dev);
  if (regs != 0)
    dev->mmio_vaddr = regs;
  else if (dev->mmio_paddr != 0 && dev->mmio_size != 0)
    dev->mmio_vaddr = ioremap(dev->mmio_paddr, dev->mmio_size);
  pci_enable_bus_master(&dev->pci);
  if (best->attach(dev) != 0) {
    printf("%s: attach failed at pci %d:%d.%d\n", dev->name, p->bus, p->device,
            p->function);
    if (dev->intr.kind != PCI_INTR_NONE)
      pci_intr_release(&dev->pci, &dev->intr);
    pci_disable_bus_master(&dev->pci);
    device_delete(dev);
    return 0;
  }
  if (dev->intr.kind == PCI_INTR_NONE)
    dev->irq = -1;
  dev->active = 1;
  if (dev->intr.kind != PCI_INTR_NONE && best->intr != 0) {
    intr_establish(dev->intr.vector, device_irq, dev, dev->name);
    printf("%s: at pci %d:%d.%d, %s, vector %d\n", dev->name, p->bus,
            p->device, p->function, pci_intr_name(&dev->intr), dev->intr.vector);
  } else
    printf("%s: at pci %d:%d.%d\n", dev->name, p->bus, p->device, p->function);
  return 1;
}

static int pci_walk_functions(struct pci_device *into, int max) {
  uint bus, slot, func;
  uint32_t id;
  int n = 0;

  for (bus = 0; bus < 256; bus++)
    for (slot = 0; slot < 32; slot++)
      for (func = 0; func < 8; func++) {
        id = pci_read32(bus, slot, func, PCI_REG_VENDOR_ID);
        if ((id & 0xffff) == 0xffff || (id & 0xffff) == 0) {
          if (func == 0)
            break;
          continue;
        }
        if (n < max)
          pci_function_read(&into[n], bus, slot, func, id);
        n++;
        if (func == 0 &&
            (pci_read8(bus, slot, 0, PCI_REG_HEADER_TYPE) & PCI_HEADER_TYPE_MULTIFUNC) == 0)
          break;
      }
  return n;
}

static int is_usb_host(const struct pci_device *p) {
  return p->class_code == 0x0c && p->subclass == 0x03;
}

// usb is taken from the firmware before anything attaches
int driver_attach_pci_devices(void) {
  struct pci_device *fn;
  void **regs;
  int n, i, attached = 0;

  n = pci_walk_functions(0, 0);
  pci_functions = n;
  if (n == 0)
    return 0;
  fn = malloc((size_t)n * sizeof(*fn), M_DEVBUF, M_WAITOK | M_ZERO);
  regs = malloc((size_t)n * sizeof(*regs), M_DEVBUF, M_WAITOK | M_ZERO);
  pci_walk_functions(fn, n);
  for (i = 0; i < n; i++)
    pci_msi_quiesce(&fn[i]);
  for (i = 0; i < n; i++)
    if (is_usb_host(&fn[i])) {
      if (fn[i].mmio_base != 0 && fn[i].mmio_size != 0)
        regs[i] = ioremap(fn[i].mmio_base, fn[i].mmio_size);
      usb_early_takeover(&fn[i], regs[i]);
    }
  for (i = 0; i < n; i++)
    attached += pci_function_attach(&fn[i], regs[i]);
  free(regs, M_DEVBUF);
  free(fn, M_DEVBUF);
  return attached;
}

static int device_irq(void *arg) {
  struct device *dev = arg;
  int r;

  if (!dev->active)
    return 0;
  if (dev->asleep) // told to be quiet; one already on its way is still its own
    return 1;
  r = dev->driver->intr(dev);
  return dev->intr.kind == PCI_INTR_INTX ? r : 1;
}

int driver_device_count(void) { return dm.ndevices; }

// pm_trace counts by this: every device, attached or not
const char *driver_device_name(int index) {
  struct device *dev;

  for (dev = dm.devices; dev != 0 && index > 0; dev = dev->next)
    index--;
  return dev != 0 ? dev->name : 0;
}

int driver_pci_function_count(void) { return pci_functions; }

void driver_shutdown_all(void) {
  struct device *dev;

  for (dev = dm.devices; dev != 0; dev = dev->next)
    if (dev->active && dev->driver->shutdown)
      dev->driver->shutdown(dev);
}

// SLEEP

int driver_suspend_all(void) {
  struct device *dev, *d;
  int i = 0;

  for (dev = dm.devices; dev != 0; dev = dev->next, i++) {
    if (!dev->active)
      continue;
    pm_trace_step(PMT_S_DRIVERS, i);
    if (dev->driver->suspend == 0 || dev->driver->suspend(dev) != 0) {
      printf("%s: cannot sleep\n", dev->name);
      for (d = dm.devices; d != dev; d = d->next)
        if (d->active && d->driver->resume) {
          d->asleep = 0;
          d->driver->resume(d);
        }
      return -1;
    }
    dev->asleep = 1;
    if (dev->bus == BUS_TYPE_PCI && dev->intr.kind == PCI_INTR_INTX)
      pci_intx_mask(&dev->pci, 1);
  }
  return 0;
}

void driver_resume_all(void) {
  struct device *dev;
  int i = 0;

  for (dev = dm.devices; dev != 0; dev = dev->next, i++) {
    if (!dev->active)
      continue;
    pm_trace_step(PMT_DRIVERS, i);
    if (dev->bus == BUS_TYPE_PCI)
      pci_intr_restore(&dev->pci, &dev->intr);
    dev->asleep = 0;
    if (dev->driver->resume != 0 && dev->driver->resume(dev) != 0)
      printf("%s: did not come back from sleep\n", dev->name);
  }
}

// MSI-X
int driver_setup_intr(struct device *dev) {
  return driver_setup_intr_best(dev, PCI_INTR_MSIX);
}

int driver_setup_intr_best(struct device *dev, int best) {
  if (dev == 0 || dev->bus != BUS_TYPE_PCI)
    return -1;
  if (pci_intr_alloc(&dev->pci, &dev->intr, best) < 0) {
    dev->irq = -1;
    return -1;
  }
  dev->irq = dev->intr.irq;
  return 0;
}
