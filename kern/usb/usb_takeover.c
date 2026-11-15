// USB HOST CONTROLLERS, TAKEN FROM THE FIRMWARE
// halted and quiet before any driver. firmware, ты заебал
#include "defs.h"
#include "pci.h"
#include "x86.h"
#include "ehcireg.h"
#include "xhcireg.h"

#define UHCI_LEGSUP 0xc0
#define UHCI_LEGSUP_RWC 0x8f00
#define UHCI_USBCMD 0x00
#define UHCI_USBINTR 0x04
#define UHCI_CMD_HCRESET 0x0002

#define OHCI_CONTROL 0x04
#define OHCI_CMDSTATUS 0x08
#define OHCI_INTRENABLE 0x10
#define OHCI_INTRDISABLE 0x14
#define OHCI_FMINTERVAL 0x34
#define OHCI_HCR 0x00000001
#define OHCI_OCR 0x00000008
#define OHCI_CTRL_IR 0x00000100
#define OHCI_CTRL_RWC 0x00000200
#define OHCI_INTR_OC 0x40000000

#define XHCI_LEG_BIOS_OWNED 0x00010000
#define XHCI_LEG_OS_OWNED 0x01000000
#define XHCI_LEG_RESERVED ((0x7u << 1) | (0xffu << 5) | (0x7u << 17))
#define XHCI_LEG_SMI_EVENTS (0x7u << 29)
#define XHCI_CMD_EWE 0x00000400

static uint32_t rd4(volatile uint8_t *r, uint off) { return *(volatile uint32_t *)(r + off); }
static void wr4(volatile uint8_t *r, uint off, uint32_t v) { *(volatile uint32_t *)(r + off) = v; }

static int until(volatile uint8_t *r, uint off, uint32_t mask, uint32_t want, uint us) {
  uint t;

  for (t = 0; (rd4(r, off) & mask) != want; t += 10) {
    if (t >= us)
      return -1;
    delay(10);
  }
  return 0;
}

static void say(const struct pci_device *p, const char *what, uint32_t v) {
  printf("usb: %s at pci %d:%d.%d (0x%x)\n", what, p->bus, p->device, p->function, v);
}

static void uhci(const struct pci_device *p) {
  ushort io;

  pci_write16(p->bus, p->device, p->function, UHCI_LEGSUP, UHCI_LEGSUP_RWC);
  if (!p->bar_is_io[4] || p->bar[4] == 0)
    return;
  io = (ushort)p->bar[4];
  outw(io + UHCI_USBCMD, UHCI_CMD_HCRESET);
  delay(5);
  if (inw(io + UHCI_USBCMD) & UHCI_CMD_HCRESET)
    say(p, "uhci reset still running", inw(io + UHCI_USBCMD));
  outw(io + UHCI_USBINTR, 0);
  outw(io + UHCI_USBCMD, 0);
}

static void ohci(const struct pci_device *p, volatile uint8_t *r) {
  uint32_t control = rd4(r, OHCI_CONTROL), fm;

  if (control & OHCI_CTRL_IR) {
    wr4(r, OHCI_INTRENABLE, OHCI_INTR_OC);
    wr4(r, OHCI_CMDSTATUS, OHCI_OCR);
    if (until(r, OHCI_CONTROL, OHCI_CTRL_IR, 0, 500000) < 0)
      say(p, "ohci handoff failed", rd4(r, OHCI_CONTROL));
  }
  wr4(r, OHCI_INTRDISABLE, 0xffffffff);
  wr4(r, OHCI_CONTROL, control & OHCI_CTRL_RWC);
  (void)rd4(r, OHCI_CONTROL);
  fm = rd4(r, OHCI_FMINTERVAL);
  wr4(r, OHCI_CMDSTATUS, OHCI_HCR);
  until(r, OHCI_CMDSTATUS, OHCI_HCR, 0, 30);
  wr4(r, OHCI_FMINTERVAL, fm);
}

static void ehci(const struct pci_device *p, volatile uint8_t *r) {
  volatile uint8_t *op = r + *(volatile uint8_t *)(r + EHCI_CAPLENGTH);
  uint32_t eecp = EHCI_HCC_EECP(rd4(r, EHCI_HCCPARAMS)), cap;
  int n, owned = 0, i;

  for (n = 0; eecp >= 0x40 && n < 64; n++, eecp = (cap >> 8) & 0xff) {
    cap = pci_read32(p->bus, p->device, p->function, eecp);
    if ((cap & 0xff) != EHCI_CAP_ID_LEGACY)
      continue;
    if (cap & EHCI_LEG_HC_BIOS_OWNED) {
      owned = 1;
      pci_write8(p->bus, p->device, p->function, eecp + 3, 1);
      for (i = 0; i < 100 && (cap & EHCI_LEG_HC_BIOS_OWNED); i++) {
        delay(10000);
        cap = pci_read32(p->bus, p->device, p->function, eecp);
      }
      if (cap & EHCI_LEG_HC_BIOS_OWNED) {
        say(p, "ehci handoff failed", cap);
        pci_write8(p->bus, p->device, p->function, eecp + 2, 0);
      }
    }
    pci_write32(p->bus, p->device, p->function, eecp + PCI_EHCI_USBLEGCTLSTS, 0);
    if (owned)
      wr4(op, EHCI_CONFIGFLAG, 0);
  }
  if (!(rd4(op, EHCI_USBSTS) & EHCI_STS_HCH)) {
    wr4(op, EHCI_USBCMD, rd4(op, EHCI_USBCMD) & ~EHCI_CMD_RS);
    for (i = 0; i < 20 && !(rd4(op, EHCI_USBSTS) & EHCI_STS_HCH); i++) {
      wr4(op, EHCI_USBSTS, 0x3f);
      delay(100);
    }
    if (!(rd4(op, EHCI_USBSTS) & EHCI_STS_HCH))
      say(p, "ehci will not halt", rd4(op, EHCI_USBSTS));
  }
  wr4(op, EHCI_USBINTR, 0);
  wr4(op, EHCI_USBSTS, 0x3f);
}

static void xhci(const struct pci_device *p, volatile uint8_t *r, uint64_t len) {
  volatile uint8_t *op;
  uint32_t xecp = XHCI_HCC_XECP(rd4(r, XHCI_HCCPARAMS)) << 2, eec = 0, v;
  int n;

  for (n = 0; xecp != 0 && xecp + 8 <= len && n < 256; n++) {
    eec = rd4(r, xecp);
    if (eec == 0xffffffff)
      return;
    if (XHCI_XECP_ID(eec) == XHCI_ID_USB_LEGACY) {
      if (eec & XHCI_LEG_BIOS_OWNED) {
        wr4(r, xecp, eec | XHCI_LEG_OS_OWNED);
        if (until(r, xecp, XHCI_LEG_BIOS_OWNED, 0, 1000000) < 0) {
          say(p, "xhci handoff failed", rd4(r, xecp));
          wr4(r, xecp, rd4(r, xecp) & ~XHCI_LEG_BIOS_OWNED);
        }
      }
      v = rd4(r, xecp + XHCI_XECP_LEGCTLSTS);
      wr4(r, xecp + XHCI_XECP_LEGCTLSTS, (v & XHCI_LEG_RESERVED) | XHCI_LEG_SMI_EVENTS);
      break;
    }
    if (XHCI_XECP_NEXT(eec) == 0)
      break;
    xecp += XHCI_XECP_NEXT(eec) << 2;
  }
  op = r + *(volatile uint8_t *)(r + XHCI_CAPLENGTH);
  if (until(op, XHCI_USBSTS, XHCI_STS_CNR, 0, 5000000) < 0)
    say(p, "xhci not ready", rd4(op, XHCI_USBSTS));
  wr4(op, XHCI_USBCMD,
      rd4(op, XHCI_USBCMD) & ~(XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE | XHCI_CMD_EWE));
  if (until(op, XHCI_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 32000) < 0)
    say(p, "xhci will not halt", rd4(op, XHCI_USBSTS));
}

// regs: BAR 0 mapped, or 0
void usb_early_takeover(const struct pci_device *p, void *regs) {
  uint16_t cmd = pci_read16(p->bus, p->device, p->function, PCI_REG_COMMAND);

  if (p->class_code != 0x0c || p->subclass != 0x03)
    return;
  if (p->prog_if == 0x00) {
    uhci(p);
    return;
  }
  // no memory decode
  if (regs == 0 || !(cmd & 0x2))
    return;
  if (p->prog_if == 0x10)
    ohci(p, regs);
  else if (p->prog_if == 0x20)
    ehci(p, regs);
  else if (p->prog_if == 0x30)
    xhci(p, regs, p->mmio_size);
}
