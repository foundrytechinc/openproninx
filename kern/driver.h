#ifndef PRONINX_DRIVER_H
#define PRONINX_DRIVER_H

#include "inc/types.h"
#include "pci.h"

#define DRIVER_NAME_MAX 32
#define DEVICE_NAME_MAX 32
#define MAX_DRIVERS 16
#define MAX_DEVICES 32

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
  char name[DRIVER_NAME_MAX];
  enum bus_type bus_type;
  enum device_type dev_type;
  int (*probe)(struct device *dev);
  int (*attach)(struct device *dev);
  int (*detach)(struct device *dev);
  void (*intr)(struct device *dev);
  void (*poll)(struct device *dev);
};

struct device {
  char name[DEVICE_NAME_MAX];
  enum device_type type;
  enum bus_type bus;
  struct driver *driver;
  void *driver_data;
  struct pci_device pci;
  int irq;
  void *mmio_vaddr;
  uint32_t mmio_paddr;
  uint32_t mmio_size;
  uint16_t io_base;
  int active;
};

void driver_framework_init(void);
int driver_register(struct driver *drv);
int driver_attach_pci_devices(void);
int driver_dispatch_irq(int irq);
void driver_poll_all(void);
struct device *driver_find_device(const char *name);
struct device *driver_find_device_by_type(enum device_type type);

#endif /* PRONINX_DRIVER_H */
