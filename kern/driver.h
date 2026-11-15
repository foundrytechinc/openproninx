#ifndef PRONINX_DRIVER_H
#define PRONINX_DRIVER_H

#include "inc/types.h"
#include "pci.h"

#define DEVICE_NAME_MAX 32

enum device_type {
  DEVICE_TYPE_UNKNOWN = 0,
  DEVICE_TYPE_NET,
  DEVICE_TYPE_BLOCK,
  DEVICE_TYPE_CHAR,
  DEVICE_TYPE_DISPLAY,
  DEVICE_TYPE_BUS,
};

enum bus_type {
  BUS_TYPE_UNKNOWN = 0,
  BUS_TYPE_PCI,
  BUS_TYPE_ISA,
  BUS_TYPE_SYSTEM,
};

struct device;

struct driver {
  const char *name;
  enum bus_type bus_type;
  enum device_type dev_type;
  int (*probe)(struct device *dev);
  int (*attach)(struct device *dev);
  int (*detach)(struct device *dev);
  int (*intr)(struct device *dev);
  void (*shutdown)(struct device *dev);
  int (*suspend)(struct device *dev);
  int (*resume)(struct device *dev);
  struct driver *next;
};

struct device {
  struct device *next;
  char name[DEVICE_NAME_MAX]; // nameunit
  int unit;
  enum device_type type;
  enum bus_type bus;
  struct driver *driver;
  void *driver_data;
  struct pci_device pci;
  struct pci_intr intr;
  int irq;
  void *mmio_vaddr;
  uint64_t mmio_paddr;
  uint64_t mmio_size;
  uint16_t io_base;
  int active;
  volatile int asleep;
};

void driver_framework_init(void);
int driver_register(struct driver *drv);
int driver_attach_pci_devices(void);
int driver_setup_intr(struct device *dev);
int driver_setup_intr_best(struct device *dev, int best);
int driver_suspend_all(void);
void driver_resume_all(void);
int driver_device_count(void);

#endif /* PRONINX_DRIVER_H */
