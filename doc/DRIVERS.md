# OpenProninx Driver Framework & Subsystem Architecture

## 1. Overview & Architectural Goals

The **OpenProninx Driver Framework** provides an extensible, unified hardware abstraction layer (HAL) for device discovery, driver matching, resource allocation (MMIO, Port I/O, IRQ lines), and interrupt dispatching.

### Key Objectives
- **Decoupling hardware drivers from kernel core:** Drivers register through standardized hooks rather than requiring hardcoded checks in the interrupt handlers or entry points.
- **Automated PCI enumeration and probing:** Automatic discovery, resource extraction, and driver attachment for all devices across all PCI buses and functions.
- **Safe Memory-Mapped I/O (MMIO):** Dynamic mapping of device register spaces into the kernel's virtual memory map (`DEVSPACE`) via `ioremap()`.
- **Centralized interrupt & polling dispatching:** Hardware IRQ lines are routed directly to registered device handlers without manual vector modifications.
- **Unified Network Interface Integration:** Network drivers easily bind to the kernel networking stack (`lwIP`) via `proninx_net_interface`.

---

## 2. Core Data Structures

The framework is defined in [`kern/driver.h`](../kern/driver.h) and implemented in [`kern/driver.c`](../kern/driver.c).

### `struct driver`
Represents a device driver capable of controlling one or more devices.

```c
struct driver {
  char name[DRIVER_NAME_MAX];       // Driver identification name (e.g., "intel_e1000")
  enum bus_type bus_type;           // Target bus type: BUS_TYPE_PCI, BUS_TYPE_ISA, etc.
  enum device_type dev_type;        // Device classification: NET, BLOCK, CHAR, DISPLAY
  int (*probe)(struct device *dev); // Probe hook: returns positive match priority score
  int (*attach)(struct device *dev);// Attach hook: initializes hardware
  int (*detach)(struct device *dev);// Detach hook: graceful teardown
  void (*intr)(struct device *dev); // Hardware interrupt callback
  void (*poll)(struct device *dev); // Fallback / periodic timer polling hook
};
```

### `struct device`
Represents an instance of an attached hardware device.

```c
struct device {
  char name[DEVICE_NAME_MAX];       // Interface or device name (e.g., "em0", "vtnet0")
  enum device_type type;            // Device classification
  enum bus_type bus;                // Parent bus
  struct driver *driver;            // Bound driver pointer
  void *driver_data;                // Driver-private state pointer
  struct pci_device pci;            // PCI configuration metadata (if on PCI)
  int irq;                          // Assigned interrupt line
  void *mmio_vaddr;                 // Kernel virtual address for MMIO registers
  uint64_t mmio_paddr;              // Physical base address of MMIO space (64-bit)
  uint64_t mmio_size;               // Size of MMIO window in bytes (64-bit)
  uint16_t io_base;                 // Base I/O port (for Port-mapped I/O)
  int active;                       // Activation flag (1 = operational)
};
```

---

## 3. Driver Lifecycle & Attachment Process

### 1. Registration (`driver_register`)
Drivers register themselves during kernel boot (e.g. `e1000_driver_init()`, `ahci_driver_init()`, `nvme_driver_init()` in `main.c`). The driver structure is stored in the global driver table.

### 2. Enumeration & Probing
`driver_attach_pci_devices()` scans buses 0-255, devices 0-31 and functions
0-7. Before any driver attaches, USB host controllers are taken over from the
firmware (`kern/usb/usb_takeover.c`): BIOS ownership is released, legacy SMIs
are turned off and the controller is halted. Then, for each function:
1. Reads PCI Vendor ID and Device ID.
2. Uses a unified 64-bit BAR decoder (`pci_read_bars()`) to extract physical MMIO bases and sizes into `dev->mmio_paddr` and `dev->mmio_size`.
3. Invokes the `probe()` callback of each registered PCI driver. The driver evaluates compatibility and returns a match priority score (e.g. 100 for high-fidelity native match, 50 for generic fallback).

### 3. Resource Mapping & Activation
When a driver is selected:
1. An official `struct device` slot is allocated.
2. If the device uses MMIO (`dev->mmio_paddr != 0`), `ioremap()` or `ioremap_wc()` maps the physical BAR range into the kernel's dedicated `IOMAP` virtual address region.
3. PCI Bus Mastering and Memory/IO access are enabled via `pci_enable_bus_master()`.
4. If an IRQ line is assigned, the corresponding IOAPIC entry is unmasked via `ioapicenable(dev->irq, 0)`.
5. `drv->attach(dev)` is called to perform device-specific register setup.
6. The device is marked active.

### 4. Teardown & Power State Change (`driver_shutdown_all`)
Before kernel reboot or poweroff, `driver_shutdown_all()` traverses all registered and active devices, invoking their `.detach` hooks (e.g., AHCI graceful port idle, NVMe 1.4 CC.SHN shutdown handshake) to ensure dirty buffers are flushed and hardware controllers reach quiescent states.

---

## 4. Memory-Mapped I/O (MMIO) & `ioremap`

In OpenProninx 64-bit virtual memory layout:
- `DMAP_BASE` (`0xffff800000000000`): Direct physical memory mapping.
- `IOMAP_BASE` (`0xffffc00000000000`): Dedicated higher-half MMIO region for device registers and video framebuffers.
- `KERNBASE` (`0xffffffff80000000`): Kernel code and data.

```c
void *ioremap(uintptr_t phys_addr, uint64_t size);
void *ioremap_wc(uintptr_t phys_addr, uint64_t size); /* Write-Combining via PAT */
```

Drivers access device registers via volatile pointer types (structures in `ahci.h`, `nvme.h`, `virtio_gpu.h` are marked `volatile`):
```c
volatile uint32_t *regs = (volatile uint32_t *)dev->mmio_vaddr;
#define REG_READ(offset)  (*(volatile uint32_t *)((uintptr_t)regs + (offset)))
#define REG_WRITE(offset, val) (*(volatile uint32_t *)((uintptr_t)regs + (offset)) = (val))
```

---

## 5. Interrupt & Polling Dispatching

Hardware interrupts routed by the IOAPIC trigger the kernel trap handler in [`kern/trap.c`](../kern/trap.c).

### Interrupt Handler
```c
// In kern/trap.c
default:
  if (tf->trapno >= T_IRQ0 && tf->trapno < T_IRQ0 + IRQ_SPURIOUS &&
      driver_dispatch_irq(tf->trapno - T_IRQ0)) {
    lapiceoi();
    break;
  }
```

`driver_dispatch_irq(irq)` traverses active devices and invokes `dev->driver->intr(dev)`.

### Polling Fallback
To safeguard against lost interrupts or firmware latency, the timer tick (10 ms period) executes `driver_poll_all()`, which invokes `dev->driver->poll(dev)` for active devices.

### How a device gets its interrupt
`driver_setup_intr` tries MSI-X, then MSI, then the INTx pin. A pin is
routed through the ACPI `_PRT` (`kern/acpi/prt.c`), then the MP table, then
the interrupt line register. PCI pins are level triggered, active low,
unless the firmware says otherwise.

---

## 6. Examples

### Intel PRO/1000 (`kern/net/e1000.c`)

Modelled on DragonFly's `if_em.c`, on top of Intel's shared code in
`third_party/e1000/`. The shared code does reset, NVM, PHY and link. The
driver adds `e1000_osdep.[ch]`, the descriptor rings and `em_reset`. Device
ids come from FreeBSD's `em` and `igb`.

### PS/2 mouse (`kern/psm.c`, `kern/atkbdc.c`)

FreeBSD's `psm(4)` on its `atkbdc(4)`. The controller keeps a queue per
device so bytes are not lost. Vendor probes run in FreeBSD's order. Bad
packets cause a resync or a reset. Touchpad gestures are turned into mouse
events in the driver and come out on `/dev/wsmouseN`. See
[`VIDEO.md`](VIDEO.md).

### USB host controllers (`kern/usb/xhci.c`, `kern/usb/ehci.c`)

Both implement `struct usb_bus_methods` for `kern/usb/usbdi.c`. xHCI uses
slots, contexts and rings. EHCI uses queue heads and qTDs, a periodic tree
of 255 queue heads, iTDs and siTDs. EHCI descriptors come from `kalloc_low()`
and data buffers from the DMA arena below 4 GB. Low and full speed ports go
to the companion controller. Up to four EHCI controllers attach.

### Serial lines (`kern/uart.c`)

A port of OpenBSD's `com(4)`. It probes the four legacy ports, detects the
UART type and measures the FIFO. Each port is a terminal, `/dev/ttyuN`, and
`stty` sets speed, size, parity, stop bits and flow control. Kernel messages
go to the first port by polling, so `printf` works before anything else is
set up. The modem control ioctls drive DTR, RTS and break.

## 7. How to Write a New Device Driver

To implement a new driver in OpenProninx:

### Step 1: Define Driver Operations
```c
#include "driver.h"
#include "defs.h"

static int mydriver_probe(struct device *dev) {
  if (dev->bus == BUS_TYPE_PCI && dev->pci.vendor_id == 0x1234 && dev->pci.device_id == 0x5678)
    return 100; // Match score
  return 0;
}

static int mydriver_attach(struct device *dev) {
  printf("MYDEV: Initializing hardware at MMIO %p\n", dev->mmio_vaddr);
  // Perform device initialization...
  return 0;
}

static void mydriver_intr(struct device *dev) {
  // Handle interrupt...
}

static struct driver mydriver = {
  .name = "my_custom_driver",
  .bus_type = BUS_TYPE_PCI,
  .dev_type = DEVICE_TYPE_CHAR,
  .probe = mydriver_probe,
  .attach = mydriver_attach,
  .detach = 0,
  .intr = mydriver_intr,
  .poll = 0,
};

void mydriver_init(void) {
  driver_register(&mydriver);
}
```

### Step 2: Register in `kern/defs.h` & `kern/main.c`
In `kern/main.c`:
```c
mydriver_init();
```
The driver will automatically be probed and attached when `driver_attach_pci_devices()` runs!
