// THE PCI INTERRUPT ROUTING TABLE

#include "acpi.h"
#include "aml.h"
#include "defs.h"
#include "malloc.h"
#include "pci.h"

#define PCI_BRIDGE 1
#define PCI_REG_SECONDARY 0x19
#define PCI_REG_PRIMARY 0x18

struct host {
  struct host *link;
  struct aml_node *node;
  int bus;
  struct aml_value prt;
};

static struct host *hosts;

static int busrange(int idx __unused, union acpi_resource *crs, void *arg) {
  int *bus = arg;

  if (AML_CRSTYPE(crs) == LR_WORD && crs->lr_word.type == LR_TYPE_BUS &&
      *bus < 0)
    *bus = crs->lr_word._min;
  return 0;
}

static int isroot(struct aml_node *node) {
  struct aml_value res;
  int root = 0;

  if (aml_eval_hid(node, &res) == 0) {
    root = res.type == AML_STRING && (strcmp(res.str, ACPI_DEV_PCIB) == 0 ||
                                      strcmp(res.str, ACPI_DEV_PCIEB) == 0);
    aml_freevalue(&res);
  }
  return root;
}

static int busnum(struct aml_node *node) {
  struct aml_value res;
  int64_t v;
  int bus = -1, pbus;

  if (node == 0 || node->value == 0 || node->value->type != AML_DEVICE)
    return -1;
  if (isroot(node)) {
    if (aml_eval_int(node, "_SEG", 0, 0, &v) == 0 && v != 0)
      return -1;
    if (aml_eval_int(node, "_BBN", 0, 0, &v) == 0)
      return (int)(v & 0xff);
    if (aml_eval_name(node, "_CRS", 0, 0, &res) == 0) {
      aml_parse_resource(&res, busrange, &bus);
      aml_freevalue(&res);
    }
    return bus < 0 ? 0 : bus;
  }
  if (aml_eval_int(node, "_ADR", 0, 0, &v) != 0 ||
      (pbus = busnum(node->parent)) < 0)
    return -1;
  if (pci_read16((uint)pbus, (uint)(v >> 16) & 31, (uint)v & 7, 0) == 0xffff ||
      (pci_read8((uint)pbus, (uint)(v >> 16) & 31, (uint)v & 7, PCI_REG_HEADER_TYPE) & 0x7f) != PCI_BRIDGE)
    return -1;
  return pci_read8((uint)pbus, (uint)(v >> 16) & 31, (uint)v & 7,
                   PCI_REG_SECONDARY);
}

static int foundprt(struct aml_node *prt, void *arg __unused) {
  struct host *h;
  int bus;

  if (prt->parent == 0 || (bus = busnum(prt->parent)) < 0)
    return 0;
  h = malloc(sizeof(*h), M_ACPI, M_WAITOK | M_ZERO);
  h->node = prt->parent;
  h->bus = bus;
  if (aml_eval(prt, 0, 0, &h->prt) != 0 || h->prt.type != AML_PACKAGE) {
    aml_freevalue(&h->prt);
    free(h, M_ACPI);
    return 0;
  }
  h->link = hosts;
  hosts = h;
  return 0;
}

void acpi_prt_init(void) { aml_find(aml_root(), "_PRT", foundprt, 0); }

// A LINK
struct linkirq {
  int gsi, level, low;
};

struct linkset {
  struct linkset *next;
  struct aml_node *dev;
  struct aml_value crs;
};

static struct linkset *linksets;

static int linkcb(int idx __unused, union acpi_resource *crs, void *arg) {
  struct linkirq *l = arg;
  int mask, bit;

  if (l->gsi >= 0)
    return 0;
  switch (AML_CRSTYPE(crs)) {
  case SR_IRQ:
    mask = crs->sr_irq.irq_mask;
    for (bit = 0; bit < 16 && !(mask & (1 << bit)); bit++)
      ;
    if (bit == 16)
      return 0;
    l->gsi = bit;
    if (AML_CRSLEN(crs) == 3) {
      l->level = 0;
      l->low = 0;
    } else {
      l->level = !(crs->sr_irq.irq_flags & SR_IRQ_MODE);
      l->low = (crs->sr_irq.irq_flags & SR_IRQ_POLARITY) != 0;
    }
    break;
  case LR_EXTIRQ:
    if (crs->lr_extirq.irq_count < 1)
      return 0;
    l->gsi = (int)crs->lr_extirq.irq[0];
    l->level = !(crs->lr_extirq.flags & LR_EXTIRQ_MODE);
    l->low = (crs->lr_extirq.flags & LR_EXTIRQ_POLARITY) != 0;
    break;
  }
  return 0;
}

static int link(struct aml_node *dev, int *gsi, int *level, int *low) {
  struct linkirq l = {-1, 1, 1};
  struct aml_value res;
  struct linkset *ls;

  if (!(acpi_getsta(dev) & STA_ENABLED))
    return -1;
  if (aml_eval_name(dev, "_CRS", 0, 0, &res) != 0)
    return -1;
  aml_parse_resource(&res, linkcb, &l);
  if (l.gsi < 0) {
    aml_freevalue(&res);
    return -1;
  }
  for (ls = linksets; ls != 0 && ls->dev != dev; ls = ls->next)
    ;
  if (ls == 0) {
    ls = malloc(sizeof(*ls), M_ACPI, M_WAITOK | M_ZERO);
    ls->dev = dev;
    ls->crs = res;
    ls->next = linksets;
    linksets = ls;
  } else
    aml_freevalue(&res);
  *gsi = l.gsi;
  *level = l.level;
  *low = l.low;
  return 0;
}

void acpi_prt_resume(void) {
  struct linkset *ls;

  for (ls = linksets; ls != 0; ls = ls->next)
    if (aml_eval_name(ls->dev, "_SRS", 1, &ls->crs, 0) != 0)
      printf("acpi: %s would not take its interrupt back\n", aml_nodename(ls->dev));
}

static struct host *hostof(int bus) {
  struct host *h;

  for (h = hosts; h != 0; h = h->link)
    if (h->bus == bus)
      return h;
  return 0;
}

static int lookup(struct host *h, int dev, int pin, int *gsi, int *level,
                  int *low) {
  int i;

  for (i = 0; i < h->prt.length; i++) {
    struct aml_value *e = h->prt.pkg[i], *src;

    if (e->type != AML_PACKAGE || e->length != 4)
      continue;
    if ((int)(aml_val2int(e->pkg[0]) >> 16) != dev ||
        (int)aml_val2int(e->pkg[1]) != pin)
      continue;
    src = e->pkg[2];
    if (src->type == AML_OBJREF)
      src = src->objref.ref;
    if (src != 0 && src->type == AML_DEVICE && src->node != 0)
      return link(src->node, gsi, level, low);
    *gsi = (int)aml_val2int(e->pkg[3]);
    *level = 1;
    *low = 1;
    return 0;
  }
  return -1;
}

static int parent(int sec, int *pbus, int *pdev) {
  int b, d, f;

  for (b = 0; b < sec; b++)
    for (d = 0; d < 32; d++)
      for (f = 0; f < 8; f++) {
        if (pci_read16((uint)b, (uint)d, (uint)f, 0) == 0xffff) {
          if (f == 0)
            break;
          continue;
        }
        if ((pci_read8((uint)b, (uint)d, (uint)f, PCI_REG_HEADER_TYPE) & 0x7f) !=
                PCI_BRIDGE ||
            pci_read8((uint)b, (uint)d, (uint)f, PCI_REG_SECONDARY) != sec)
          continue;
        *pbus = b;
        *pdev = d;
        return 0;
      }
  return -1;
}

int acpi_pci_route(int bus, int dev, int pin, int *gsi, int *level, int *low) {
  struct host *h;
  int hops, pbus, pdev;

  for (hops = 0; hops < 8; hops++) {
    if ((h = hostof(bus)) != 0)
      return lookup(h, dev, pin, gsi, level, low);
    if (bus == 0 || parent(bus, &pbus, &pdev) != 0)
      return -1;
    pin = (pin + dev) & 3;
    dev = pdev;
    bus = pbus;
  }
  return -1;
}
