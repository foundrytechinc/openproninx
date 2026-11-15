// GAS

#include "acpi.h"
#include "aml.h"
#include "aml_priv.h"
#include "defs.h"
#include "mmu.h"
#include "mutex.h"
#include "pci.h"
#include "x86.h"

// SYSTEM I/O

int acpi_region_sysio(void *cookie __unused, int dir, uint64_t addr, int size,
                      uint64_t *val) {
  uint16_t port;

  if (addr > 0xffff || addr + (uint64_t)size > 0x10000)
    return -1;
  port = (uint16_t)addr;
  if (dir == ACPI_IOREAD) {
    switch (size) {
    case 1:
      *val = inb(port);
      break;
    case 2:
      *val = inw(port);
      break;
    case 4:
      *val = inl(port);
      break;
    case 8:
      *val = inl(port) | ((uint64_t)inl((uint16_t)(port + 4)) << 32);
      break;
    default:
      return -1;
    }
  } else {
    switch (size) {
    case 1:
      outb(port, (uint8_t)*val);
      break;
    case 2:
      outw(port, (uint16_t)*val);
      break;
    case 4:
      outl(port, (uint32_t)*val);
      break;
    case 8:
      outl(port, (uint32_t)*val);
      outl((uint16_t)(port + 4), (uint32_t)(*val >> 32));
      break;
    default:
      return -1;
    }
  }
  return 0;
}

// SYSTEM MEMORY

#define NWINDOW 8

static struct {
  struct mutex lock;
  struct {
    uint64_t pa; // of the page
    uintptr_t va;
    int used;
  } w[NWINDOW];
  int next;
} mem = {MUTEX_INITIALIZER("acpi window"), {{0, 0, 0}}, 0};

static uintptr_t window(uint64_t pa) {
  uint64_t page = pa & ~(uint64_t)(PGSIZE - 1);
  int i, k;

  for (i = 0; i < NWINDOW; i++)
    if (mem.w[i].used && mem.w[i].pa == page)
      return mem.w[i].va;
  k = mem.next;
  mem.next = (mem.next + 1) % NWINDOW;
  if (mem.w[k].used) {
    kunmap_page(mem.w[k].va);
    kmap_update();
    mem.w[k].used = 0;
  }
  if (mem.w[k].va == 0 && (mem.w[k].va = kmap_alloc(PGSIZE)) == 0)
    return 0;
  kmap_page(mem.w[k].va, page, 1, 0, KMAP_UNCACHED);
  mem.w[k].pa = page;
  mem.w[k].used = 1;
  return mem.w[k].va;
}

int acpi_region_sysmem(void *cookie __unused, int dir, uint64_t addr, int size,
                       uint64_t *val) {
  uintptr_t va;
  int err = 0;

  if ((addr & (PGSIZE - 1)) + (uint64_t)size > PGSIZE)
    return -1;
  mtx_enter(&mem.lock);
  if ((va = window(addr)) == 0) {
    mtx_leave(&mem.lock);
    return -1;
  }
  va += addr & (PGSIZE - 1);
  switch (size) {
  case 1:
    if (dir == ACPI_IOREAD)
      *val = *(volatile uint8_t *)va;
    else
      *(volatile uint8_t *)va = (uint8_t)*val;
    break;
  case 2:
    if (dir == ACPI_IOREAD)
      *val = *(volatile uint16_t *)va;
    else
      *(volatile uint16_t *)va = (uint16_t)*val;
    break;
  case 4:
    if (dir == ACPI_IOREAD)
      *val = *(volatile uint32_t *)va;
    else
      *(volatile uint32_t *)va = (uint32_t)*val;
    break;
  case 8:
    if (dir == ACPI_IOREAD)
      *val = *(volatile uint64_t *)va;
    else
      *(volatile uint64_t *)va = *val;
    break;
  default:
    err = -1;
  }
  mtx_leave(&mem.lock);
  return err;
}

// PCI CONFIG

int acpi_region_pcicfg(void *cookie, int dir, uint64_t addr, int size,
                       uint64_t *val) {
  uint bus = ACPI_PCI_BUS(addr), dev = ACPI_PCI_DEV(addr);
  uint fn = ACPI_PCI_FN(addr), reg = ACPI_PCI_REG(addr);
  uint64_t ecam;

  if (ACPI_PCI_SEG(addr) != 0 || reg + (uint)size > 256) {
    if (dev > 31 || fn > 7 || reg + (uint)size > 4096 ||
        (ecam = acpi_mcfg_addr(ACPI_PCI_SEG(addr), (int)bus, (int)dev, (int)fn,
                               (int)reg)) == 0) {
      if (dir == ACPI_IOREAD)
        *val = ~(uint64_t)0 >> (64 - size * 8);
      return dir == ACPI_IOREAD ? 0 : -1;
    }
    return acpi_region_sysmem(cookie, dir, ecam, size, val);
  }
  if (fn == 0xffff && dir == ACPI_IOREAD) {
    *val = ~(uint64_t)0 >> (64 - size * 8);
    return 0;
  }
  if (dev > 31 || fn > 7)
    return -1;
  if (dir == ACPI_IOREAD) {
    switch (size) {
    case 1:
      *val = pci_read8(bus, dev, fn, reg);
      break;
    case 2:
      *val = pci_read16(bus, dev, fn, reg);
      break;
    case 4:
      *val = pci_read32(bus, dev, fn, reg);
      break;
    default:
      return -1;
    }
  } else {
    switch (size) {
    case 1:
      pci_write8(bus, dev, fn, reg, (uint8_t)*val);
      break;
    case 2:
      pci_write16(bus, dev, fn, reg, (uint16_t)*val);
      break;
    case 4:
      pci_write32(bus, dev, fn, reg, (uint32_t)*val);
      break;
    default:
      return -1;
    }
  }
  return 0;
}

int acpi_gasio(int dir, int space, uint64_t address, int access, int len,
               void *buf) {
  uint8_t *pb = buf;
  int off;

  for (off = 0; off < len; off += access) {
    uint64_t v = 0;
    int err;

    if (dir == ACPI_IOWRITE) {
      switch (access) {
      case 1:
        v = pb[off];
        break;
      case 2:
        v = *(uint16_t *)(pb + off);
        break;
      case 4:
        v = *(uint32_t *)(pb + off);
        break;
      case 8:
        v = *(uint64_t *)(pb + off);
        break;
      default:
        return -1;
      }
    }
    switch (space) {
    case GAS_SYSTEM_MEMORY:
      err = acpi_region_sysmem(0, dir, address + (uint64_t)off, access, &v);
      break;
    case GAS_SYSTEM_IOSPACE:
      err = acpi_region_sysio(0, dir, address + (uint64_t)off, access, &v);
      break;
    case GAS_PCI_CFG_SPACE:
      err = acpi_region_pcicfg(0, dir, address + (uint64_t)off, access, &v);
      break;
    default:
      return -1;
    }
    if (err)
      return err;
    if (dir == ACPI_IOREAD) {
      switch (access) {
      case 1:
        pb[off] = (uint8_t)v;
        break;
      case 2:
        *(uint16_t *)(pb + off) = (uint16_t)v;
        break;
      case 4:
        *(uint32_t *)(pb + off) = (uint32_t)v;
        break;
      case 8:
        *(uint64_t *)(pb + off) = v;
        break;
      }
    }
  }
  return 0;
}

// THE GLOBAL LOCK

int acpi_acquire_glk(uint32_t *lock) {
  uint32_t new, old;

  do {
    old = *lock;
    new = (old & ~GL_BIT_PENDING) | GL_BIT_OWNED;
    if (old & GL_BIT_OWNED)
      new |= GL_BIT_PENDING;
  } while (__sync_val_compare_and_swap(lock, old, new) != old);
  return (new & GL_BIT_PENDING) == 0;
}

int acpi_release_glk(uint32_t *lock) {
  uint32_t new, old;

  do {
    old = *lock;
    new = old & ~(GL_BIT_PENDING | GL_BIT_OWNED);
  } while (__sync_val_compare_and_swap(lock, old, new) != old);
  return (old & GL_BIT_PENDING) != 0;
}
