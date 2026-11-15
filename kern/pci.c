#include "pci.h"
#include "acpi.h"
#include "defs.h"
#include "proc.h"
#include "malloc.h"
#include "trap.h"
#include "x86.h"

static struct mutex pci_cfg = MUTEX_INITIALIZER("pci");
static int cfg_locking;

void pci_lock_enable(void) { cfg_locking = 1; }

static void cfg_lock(void) {
  if (cfg_locking)
    mtx_enter(&pci_cfg);
}

static void cfg_unlock(void) {
  if (cfg_locking)
    mtx_leave(&pci_cfg);
}

#define CFG_ADDR(bus, dev, func, offset)                                       \
  (0x80000000U | ((bus) << 16) | ((dev) << 11) | ((func) << 8) |               \
   ((offset) & 0xfc))

uint32_t pci_read32(uint bus, uint dev, uint func, uint offset) {
  uint32_t v;

  cfg_lock();
  outl(PCI_CONFIG_ADDRESS, CFG_ADDR(bus, dev, func, offset));
  v = inl(PCI_CONFIG_DATA);
  cfg_unlock();
  return v;
}

uint16_t pci_read16(uint bus, uint dev, uint func, uint offset) {
  uint16_t v;

  cfg_lock();
  outl(PCI_CONFIG_ADDRESS, CFG_ADDR(bus, dev, func, offset));
  v = inw(PCI_CONFIG_DATA + (offset & 2));
  cfg_unlock();
  return v;
}

uint8_t pci_read8(uint bus, uint dev, uint func, uint offset) {
  uint8_t v;

  cfg_lock();
  outl(PCI_CONFIG_ADDRESS, CFG_ADDR(bus, dev, func, offset));
  v = inb(PCI_CONFIG_DATA + (offset & 3));
  cfg_unlock();
  return v;
}

void pci_write32(uint bus, uint dev, uint func, uint offset, uint32_t value) {
  cfg_lock();
  outl(PCI_CONFIG_ADDRESS, CFG_ADDR(bus, dev, func, offset));
  outl(PCI_CONFIG_DATA, value);
  cfg_unlock();
}

void pci_write16(uint bus, uint dev, uint func, uint offset, uint16_t value) {
  cfg_lock();
  outl(PCI_CONFIG_ADDRESS, CFG_ADDR(bus, dev, func, offset));
  outw(PCI_CONFIG_DATA + (offset & 2), value);
  cfg_unlock();
}

void pci_write8(uint bus, uint dev, uint func, uint offset, uint8_t value) {
  cfg_lock();
  outl(PCI_CONFIG_ADDRESS, CFG_ADDR(bus, dev, func, offset));
  outb(PCI_CONFIG_DATA + (offset & 3), value);
  cfg_unlock();
}

void pci_read_bars(uint bus, uint dev, uint func, struct pci_device *out) {
  uint32_t cmd = pci_read32(bus, dev, func, PCI_REG_COMMAND);
  int b;

  pci_write32(bus, dev, func, PCI_REG_COMMAND, cmd & ~3u);
  for (b = 0; b < 6; b++) {
    uint offset = PCI_REG_BAR0 + b * 4;
    uint32_t lo = pci_read32(bus, dev, func, offset);
    uint32_t mask_lo, mask_hi = 0, hi = 0;
    uint64_t base;
    int wide = 0;

    if (lo & 1) {
      out->bar_is_io[b] = 1;
      base = lo & ~3U;
    } else {
      out->bar_is_io[b] = 0;
      base = lo & ~0xfU;
      wide = (lo & 0x6) == 0x4;
    }
    if (wide) {
      hi = pci_read32(bus, dev, func, offset + 4);
      base |= (uint64_t)hi << 32;
    }

    pci_write32(bus, dev, func, offset, 0xffffffff);
    if (wide)
      pci_write32(bus, dev, func, offset + 4, 0xffffffff);
    mask_lo = pci_read32(bus, dev, func, offset);
    if (wide)
      mask_hi = pci_read32(bus, dev, func, offset + 4);
    pci_write32(bus, dev, func, offset, lo);
    if (wide)
      pci_write32(bus, dev, func, offset + 4, hi);

    out->bar[b] = base;
    if (wide) {
      uint64_t m = ((((uint64_t)mask_hi << 32) | mask_lo) & ~(uint64_t)0xf);
      out->bar_size[b] = m == 0 ? 0 : (~m) + 1;
      if (++b >= 6)
        break;
      out->bar[b] = 0;
      out->bar_size[b] = 0;
      out->bar_is_io[b] = 0;
    } else {
      uint32_t m = mask_lo & ((lo & 1) ? ~3U : ~0xfU);
      out->bar_size[b] = m == 0 ? 0 : (uint64_t)(~m) + 1;
    }
  }
  pci_write32(bus, dev, func, PCI_REG_COMMAND, cmd);
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

  pci_read_bars(bus, dev, func, out);

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
            break;
          continue;
        }

        if (vendor == vendor_id && device == device_id) {
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

void pci_disable_bus_master(const struct pci_device *dev) {
  if (!dev)
    return;
  uint16_t cmd = pci_read16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND);
  cmd &= (uint16_t)~PCI_COMMAND_MASTER;
  pci_write16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND, cmd);
}

// MESSAGE SIGNALLED INTERRUPTS

#define MSI_ADDR_BASE 0xfee00000u

#define MSI_CTL_ENABLE 0x0001
#define MSI_CTL_MMC(x) (((x) >> 1) & 7)
#define MSI_CTL_MME(x) (((x) & 7) << 4)
#define MSI_CTL_MME_MASK 0x0070
#define MSI_CTL_64BIT 0x0080
#define MSI_CTL_PERVEC 0x0100

#define MSIX_CTL_TABLE_SIZE(x) (((x) & 0x7ff) + 1)
#define MSIX_CTL_FUNC_MASK 0x4000
#define MSIX_CTL_ENABLE 0x8000
#define MSIX_TABLE_BIR(x) ((x) & 7)
#define MSIX_TABLE_OFFSET(x) ((x) & ~7u)
#define MSIX_VCTL_MASK 0x00000001

int pci_find_capability(const struct pci_device *dev, uint8_t id) {
  uint16_t status;
  uint8_t off;
  int guard;

  if (!dev)
    return -1;
  status = pci_read16(dev->bus, dev->device, dev->function, PCI_REG_STATUS);
  if ((status & PCI_STATUS_CAP_LIST) == 0)
    return -1;

  off = pci_read8(dev->bus, dev->device, dev->function, PCI_REG_CAP_PTR) & 0xfc;
  for (guard = 0; off >= 0x40 && guard < 48; guard++) {
    uint8_t cap = pci_read8(dev->bus, dev->device, dev->function, off);
    if (cap == id)
      return off;
    off = pci_read8(dev->bus, dev->device, dev->function, off + 1) & 0xfc;
    if (off == 0)
      break;
  }
  return -1;
}

int pci_msi_count(const struct pci_device *dev) {
  int cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
  uint16_t ctl;

  if (cap < 0)
    return 0;
  ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
  return 1 << MSI_CTL_MMC(ctl);
}

int pci_msix_count(const struct pci_device *dev) {
  int cap = pci_find_capability(dev, PCI_CAP_ID_MSIX);
  uint16_t ctl;

  if (cap < 0)
    return 0;
  ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
  return MSIX_CTL_TABLE_SIZE(ctl);
}

static uint32_t msi_address(void) {
  return MSI_ADDR_BASE | ((cpus[0].apicid & 0xff) << 12);
}

static int msix_setup(const struct pci_device *dev, int cap, int vector) {
  volatile uint32_t *entry;
  uint32_t table;
  uint16_t ctl;
  int bir;

  table = pci_read32(dev->bus, dev->device, dev->function, cap + 4);
  bir = MSIX_TABLE_BIR(table);
  if (bir > 5 || dev->bar_is_io[bir] || dev->bar[bir] == 0)
    return -1;

  entry = ioremap(dev->bar[bir] + MSIX_TABLE_OFFSET(table), 16);
  if (entry == 0)
    return -1;

  ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
  pci_write16(dev->bus, dev->device, dev->function, cap + 2,
              ctl | MSIX_CTL_ENABLE | MSIX_CTL_FUNC_MASK);

  entry[0] = msi_address();
  entry[1] = 0;
  entry[2] = (uint32_t)vector;
  entry[3] = 0;

  ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
  ctl &= ~MSIX_CTL_FUNC_MASK;
  pci_write16(dev->bus, dev->device, dev->function, cap + 2,
              ctl | MSIX_CTL_ENABLE);
  return 0;
}

static int msi_setup(const struct pci_device *dev, int cap, int vector) {
  uint16_t ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
  int wide = (ctl & MSI_CTL_64BIT) != 0;
  int data_off = wide ? cap + 12 : cap + 8;
  int mask_off = wide ? cap + 16 : cap + 12;

  pci_write32(dev->bus, dev->device, dev->function, cap + 4, msi_address());
  if (wide)
    pci_write32(dev->bus, dev->device, dev->function, cap + 8, 0);
  pci_write16(dev->bus, dev->device, dev->function, data_off,
              (uint16_t)vector);
  if (ctl & MSI_CTL_PERVEC)
    pci_write32(dev->bus, dev->device, dev->function, mask_off, 0);

  ctl &= ~MSI_CTL_MME_MASK;
  ctl |= MSI_CTL_ENABLE;
  pci_write16(dev->bus, dev->device, dev->function, cap + 2, ctl);
  return 0;
}

// the firmware left MSI on. shut up until a driver says so
void pci_msi_quiesce(const struct pci_device *dev) {
  uint16_t ctl;
  int cap;

  if ((cap = pci_find_capability(dev, PCI_CAP_ID_MSI)) >= 0 &&
      ((ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2)) & MSI_CTL_ENABLE)) {
    pci_write16(dev->bus, dev->device, dev->function, cap + 2, ctl & ~MSI_CTL_ENABLE);
    printf("pci %d:%d.%d: the firmware left MSI on, turned off\n", dev->bus, dev->device,
           dev->function);
  }
  if ((cap = pci_find_capability(dev, PCI_CAP_ID_MSIX)) >= 0 &&
      ((ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2)) & MSIX_CTL_ENABLE)) {
    pci_write16(dev->bus, dev->device, dev->function, cap + 2, ctl & ~MSIX_CTL_ENABLE);
    printf("pci %d:%d.%d: the firmware left MSI-X on, turned off\n", dev->bus, dev->device,
           dev->function);
  }
}

static void intx_disable(const struct pci_device *dev, int off) {
  uint16_t cmd = pci_read16(dev->bus, dev->device, dev->function,
                            PCI_REG_COMMAND);
  if (off)
    cmd |= PCI_COMMAND_INTX_DISABLE;
  else
    cmd &= ~PCI_COMMAND_INTX_DISABLE;
  pci_write16(dev->bus, dev->device, dev->function, PCI_REG_COMMAND, cmd);
}

// INTx held off at the function, for a sleeping device
void pci_intx_mask(const struct pci_device *dev, int masked) {
  intx_disable(dev, masked);
}

int pci_intr_alloc(const struct pci_device *dev, struct pci_intr *out,
                   int best) {
  int cap, vector;

  if (!dev || !out)
    return -1;
  out->kind = PCI_INTR_NONE;
  out->vector = -1;
  out->irq = -1;

  cap = best >= PCI_INTR_MSIX ? pci_find_capability(dev, PCI_CAP_ID_MSIX) : -1;
  if (cap >= 0) {
    vector = intr_vector_alloc();
    if (vector >= 0) {
      if (msix_setup(dev, cap, vector) == 0) {
        intx_disable(dev, 1);
        out->kind = PCI_INTR_MSIX;
        out->vector = vector;
        return 0;
      }
      intr_vector_free(vector);
    }
  }

  cap = best >= PCI_INTR_MSI ? pci_find_capability(dev, PCI_CAP_ID_MSI) : -1;
  if (cap >= 0) {
    vector = intr_vector_alloc();
    if (vector >= 0) {
      if (msi_setup(dev, cap, vector) == 0) {
        intx_disable(dev, 1);
        out->kind = PCI_INTR_MSI;
        out->vector = vector;
        return 0;
      }
      intr_vector_free(vector);
    }
  }

  {
    int pin = pci_read8(dev->bus, dev->device, dev->function, PCI_REG_INTERRUPT_PIN);
    int intin, level = 1, low = 1;

    if (pin >= 1 && pin <= 4 &&
        acpi_pci_route(dev->bus, dev->device, pin - 1, &intin, &level, &low) == 0)
      out->irq = intin;
    else if (pin >= 1 && pin <= 4 &&
             mp_pci_route(dev->bus, dev->device, pin - 1, &intin, &level, &low) == 0)
      out->irq = intin;
    else if (dev->irq != 0 && dev->irq < 16)
      out->irq = dev->irq;
    else
      return -1;
    if ((out->vector = ioapic_gsi_vector(out->irq, 0, level, low)) < 0)
      return -1;
    intx_disable(dev, 0);
    out->kind = PCI_INTR_INTX;
    return 0;
  }
}

void pci_intr_release(const struct pci_device *dev, struct pci_intr *in) {
  uint16_t ctl;
  int cap;

  switch (in->kind) {
  case PCI_INTR_MSIX:
    if ((cap = pci_find_capability(dev, PCI_CAP_ID_MSIX)) >= 0) {
      ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
      pci_write16(dev->bus, dev->device, dev->function, cap + 2,
                  ctl & ~MSIX_CTL_ENABLE);
    }
    intr_vector_free(in->vector);
    break;
  case PCI_INTR_MSI:
    if ((cap = pci_find_capability(dev, PCI_CAP_ID_MSI)) >= 0) {
      ctl = pci_read16(dev->bus, dev->device, dev->function, cap + 2);
      pci_write16(dev->bus, dev->device, dev->function, cap + 2,
                  ctl & ~MSI_CTL_ENABLE);
    }
    intr_vector_free(in->vector);
    break;
  default:
    break;
  }
  intx_disable(dev, 1);
  in->kind = PCI_INTR_NONE;
  in->vector = -1;
  in->irq = -1;
}

const char *pci_intr_name(const struct pci_intr *in) {
  switch (in ? in->kind : PCI_INTR_NONE) {
  case PCI_INTR_MSIX:
    return "msi-x";
  case PCI_INTR_MSI:
    return "msi";
  case PCI_INTR_INTX:
    return "intx";
  default:
    return "none";
  }
}

// SLEEP

#define PCI_CAP_ID_EXP 0x10

struct pcisave {
  uint8_t bus, dev, fn, hdr;
  uint32_t cfg[16];
  int msi, msix, exp;
  uint32_t msireg[6];
  uint16_t msixctl;
  uint16_t expreg[8];
  uint32_t ide[8];
};

static struct pcisave *saves;
static int nsaves;

static int cap_of(uint bus, uint dev, uint fn, uint8_t id) {
  struct pci_device d;

  memset(&d, 0, sizeof(d));
  d.bus = (uint8_t)bus;
  d.device = (uint8_t)dev;
  d.function = (uint8_t)fn;
  return pci_find_capability(&d, id);
}

static const uint8_t expctl[8] = {0x08, 0x10, 0x18, 0x1c, 0x28, 0x30, 0x38, 0};

static int is_ide(const struct pcisave *s) {
  return ((s->cfg[2] >> 24) & 0xff) == PCI_CLASS_STORAGE &&
         ((s->cfg[2] >> 16) & 0xff) == PCI_SUBCLASS_IDE;
}

static int pci_walk(struct pcisave *into, int max) {
  uint b, d, f;
  int i, n = 0;

  for (b = 0; b < 256; b++)
    for (d = 0; d < 32; d++)
      for (f = 0; f < 8; f++) {
        struct pcisave *s;
        uint32_t hdr;

        if (pci_read16(b, d, f, 0) == 0xffff) {
          if (f == 0)
            break;
          continue;
        }
        hdr = pci_read32(b, d, f, 0x0c);
        if (into != 0 && n == max)
          return n;
        if (into != 0) {
          s = &into[n];
          s->bus = (uint8_t)b;
          s->dev = (uint8_t)d;
          s->fn = (uint8_t)f;
          for (i = 0; i < 16; i++)
            s->cfg[i] = pci_read32(b, d, f, (uint)i * 4);
          s->hdr = (uint8_t)((s->cfg[3] >> 16) & 0x7f);
          if ((s->msi = cap_of(b, d, f, PCI_CAP_ID_MSI)) > 0)
            for (i = 0; i < 6; i++)
              s->msireg[i] = pci_read32(b, d, f, (uint)(s->msi + i * 4));
          if ((s->msix = cap_of(b, d, f, PCI_CAP_ID_MSIX)) > 0)
            s->msixctl = pci_read16(b, d, f, (uint)s->msix + 2);
          if (is_ide(s))
            for (i = 0; i < 8; i++)
              s->ide[i] = pci_read32(b, d, f, 0x40 + (uint)i * 4);
          if ((s->exp = cap_of(b, d, f, PCI_CAP_ID_EXP)) > 0)
            for (i = 0; expctl[i] != 0; i++)
              s->expreg[i] = pci_read16(b, d, f, (uint)(s->exp + expctl[i]));
        }
        n++;
        if (f == 0 && !(hdr & (PCI_HEADER_TYPE_MULTIFUNC << 16)))
          break;
      }
  return n;
}

void pci_suspend(void) {
  int n = pci_walk(0, 0);

  if (saves != 0)
    free(saves, M_DEVBUF);
  saves = malloc((size_t)n * sizeof(*saves), M_DEVBUF, M_WAITOK | M_ZERO);
  nsaves = pci_walk(saves, n);
}

void pci_resume(void) {
  int n, i, pm;
  uint16_t pmcsr;

  for (n = 0; n < nsaves; n++) {
    struct pcisave *s = &saves[n];
    uint b = s->bus, d = s->dev, f = s->fn;

    if (pci_read16(b, d, f, 0) == 0xffff)
      continue;
    // D0 before the BARs, which D3 loses
    if ((pm = cap_of(b, d, f, PCI_CAP_ID_PM)) > 0 &&
        ((pmcsr = pci_read16(b, d, f, (uint)pm + 4)) & 3) != 0) {
      pci_write16(b, d, f, (uint)pm + 4, (uint16_t)(pmcsr & ~3u));
      delay(10000); // D3hot to D0, PCI PM 1.2 5.6.1
    }
    pci_write16(b, d, f, PCI_REG_COMMAND, 0);
    pci_write32(b, d, f, 0x0c, s->cfg[3]);
    if (s->hdr == 1) {
      for (i = 4; i < 15; i++)
        pci_write32(b, d, f, (uint)i * 4, s->cfg[i]);
      pci_write16(b, d, f, 0x3e, (uint16_t)(s->cfg[15] >> 16));
    } else {
      for (i = 4; i < 10; i++)
        pci_write32(b, d, f, (uint)i * 4, s->cfg[i]);
      pci_write32(b, d, f, 0x30, s->cfg[12]);
    }
    pci_write8(b, d, f, PCI_REG_INTERRUPT_LINE, (uint8_t)s->cfg[15]);
    if (is_ide(s))
      for (i = 0; i < 8; i++)
        pci_write32(b, d, f, 0x40 + (uint)i * 4, s->ide[i]);
    if (s->exp > 0)
      for (i = 0; expctl[i] != 0; i++)
        pci_write16(b, d, f, (uint)(s->exp + expctl[i]), s->expreg[i]);
    if (s->msi > 0) {
      for (i = 1; i < 6; i++)
        pci_write32(b, d, f, (uint)(s->msi + i * 4), s->msireg[i]);
      pci_write16(b, d, f, (uint)s->msi + 2, (uint16_t)(s->msireg[0] >> 16));
    }
    // msix обратно после таблицы, не раньше. порядок, блядь
    if (s->msix > 0)
      pci_write16(b, d, f, (uint)s->msix + 2, s->msixctl & ~MSIX_CTL_ENABLE);
    pci_write16(b, d, f, PCI_REG_COMMAND, (uint16_t)s->cfg[1]);
  }
}

void pci_intr_restore(const struct pci_device *dev, const struct pci_intr *in) {
  int cap;

  switch (in->kind) {
  case PCI_INTR_MSIX:
    if ((cap = pci_find_capability(dev, PCI_CAP_ID_MSIX)) >= 0)
      msix_setup(dev, cap, in->vector);
    break;
  case PCI_INTR_MSI:
    if ((cap = pci_find_capability(dev, PCI_CAP_ID_MSI)) >= 0)
      msi_setup(dev, cap, in->vector);
    break;
  case PCI_INTR_INTX:
    intx_disable(dev, 0);
    break;
  default:
    break;
  }
}
