// USB FROM THE BIOS
// bios usb, иди нахуй: halted before the kernel lands
#include "boot.h"

#define PCI_ADDR 0xcf8
#define PCI_DATA 0xcfc

static inline void outb(uint16 p, uint8 v) { __asm __volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(uint16 p, uint16 v) { __asm __volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl(uint16 p, uint32 v) { __asm __volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint16 inw(uint16 p) {
  uint16 v;

  __asm __volatile("inw %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}
static inline uint32 inl(uint16 p) {
  uint32 v;

  __asm __volatile("inl %1, %0" : "=a"(v) : "Nd"(p));
  return v;
}

// a port 80h write takes about 1 us
static void udelay(uint32 us) {
  while (us-- > 0)
    outb(0x80, 0);
}

static uint32 cfg_rd(uint32 b, uint32 d, uint32 f, uint32 off) {
  outl(PCI_ADDR, 0x80000000u | b << 16 | d << 11 | f << 8 | (off & 0xfc));
  return inl(PCI_DATA);
}

static void cfg_wr(uint32 b, uint32 d, uint32 f, uint32 off, uint32 v) {
  outl(PCI_ADDR, 0x80000000u | b << 16 | d << 11 | f << 8 | (off & 0xfc));
  outl(PCI_DATA, v);
}

static void cfg_wr8(uint32 b, uint32 d, uint32 f, uint32 off, uint8 v) {
  outl(PCI_ADDR, 0x80000000u | b << 16 | d << 11 | f << 8 | (off & 0xfc));
  outb((uint16)(PCI_DATA + (off & 3)), v);
}

static int until(uint32 reg, uint32 mask, uint32 want, uint32 us) {
  uint32 t;

  for (t = 0; (flat_rd32(reg) & mask) != want; t += 10) {
    if (t >= us)
      return -1;
    udelay(10);
  }
  return 0;
}

// BAR 0 as a linear address under 4 GB, or 0
static uint32 bar0(uint32 b, uint32 d, uint32 f) {
  uint32 lo = cfg_rd(b, d, f, 0x10);

  if (lo & 1)
    return 0;
  if ((lo & 6) == 4 && cfg_rd(b, d, f, 0x14) != 0)
    return 0;
  return lo & ~0xfu;
}

static void uhci(uint32 b, uint32 d, uint32 f) {
  uint32 bar4 = cfg_rd(b, d, f, 0x20);
  uint16 io;

  outl(PCI_ADDR, 0x80000000u | b << 16 | d << 11 | f << 8 | 0xc0);
  outw(PCI_DATA, 0x8f00); // LEGSUP: no SMI, the status bits cleared
  if (!(bar4 & 1))
    return;
  io = (uint16)(bar4 & ~3u);
  outw(io, 0x0002); // HCRESET
  udelay(5000);
  outw((uint16)(io + 4), 0);
  outw(io, 0);
  (void)inw(io);
}

static void ohci(uint32 r) {
  uint32 control = flat_rd32(r + 0x04), fm;

  if (control & 0x100) { // interrupt routing: the SMM owns it
    flat_wr32(r + 0x10, 0x40000000u);
    flat_wr32(r + 0x08, 0x8); // ownership change request
    until(r + 0x04, 0x100, 0, 500000);
  }
  flat_wr32(r + 0x14, 0xffffffffu);
  flat_wr32(r + 0x04, control & 0x200);
  fm = flat_rd32(r + 0x34);
  flat_wr32(r + 0x08, 0x1); // reset
  until(r + 0x08, 0x1, 0, 30);
  flat_wr32(r + 0x34, fm);
}

static void ehci(uint32 b, uint32 d, uint32 f, uint32 r) {
  uint32 op = r + (flat_rd32(r) & 0xff), eecp = (flat_rd32(r + 0x08) >> 8) & 0xff, cap;
  int n, i, owned = 0;

  for (n = 0; eecp >= 0x40 && n < 64; n++, eecp = (cap >> 8) & 0xff) {
    cap = cfg_rd(b, d, f, eecp);
    if ((cap & 0xff) != 1)
      continue;
    if (cap & 0x10000) {
      owned = 1;
      cfg_wr8(b, d, f, eecp + 3, 1);
      for (i = 0; i < 100 && (cap & 0x10000); i++) {
        udelay(10000);
        cap = cfg_rd(b, d, f, eecp);
      }
      if (cap & 0x10000)
        cfg_wr8(b, d, f, eecp + 2, 0);
    }
    cfg_wr(b, d, f, eecp + 4, 0); // USBLEGCTLSTS: no SMI
    if (owned)
      flat_wr32(op + 0x40, 0); // CONFIGFLAG
  }
  if (!(flat_rd32(op + 0x04) & 0x1000)) {
    flat_wr32(op, flat_rd32(op) & ~1u);
    until(op + 0x04, 0x1000, 0x1000, 2000);
  }
  flat_wr32(op + 0x08, 0);
  flat_wr32(op + 0x04, 0x3f);
}

static void xhci(uint32 r) {
  uint32 xecp = ((flat_rd32(r + 0x10) >> 16) & 0xffff) << 2, eec, v, op;
  int n;

  for (n = 0; xecp != 0 && n < 256; n++) {
    eec = flat_rd32(r + xecp);
    if (eec == 0xffffffffu)
      return;
    if ((eec & 0xff) == 1) {
      if (eec & 0x10000) {
        flat_wr32(r + xecp, eec | 0x1000000);
        if (until(r + xecp, 0x10000, 0, 1000000) < 0)
          flat_wr32(r + xecp, flat_rd32(r + xecp) & ~0x10000u);
      }
      v = flat_rd32(r + xecp + 4);
      flat_wr32(r + xecp + 4, (v & 0x000e1fee) | 0xe0000000u);
      break;
    }
    if (((eec >> 8) & 0xff) == 0)
      break;
    xecp += ((eec >> 8) & 0xff) << 2;
  }
  op = r + (flat_rd32(r) & 0xff);
  until(op + 0x04, 0x800, 0, 1000000); // CNR
  flat_wr32(op, flat_rd32(op) & ~(1u | 4u | 8u | 0x400u));
  until(op + 0x04, 1, 1, 32000); // HCH
}

static void take(uint32 b, uint32 d, uint32 f, uint32 class) {
  uint32 cmd = cfg_rd(b, d, f, 0x04), r;
  uint8 pi = (uint8)(class >> 8);

  if (pi == 0x00) {
    uhci(b, d, f);
    return;
  }
  if (!(cmd & 2) || (r = bar0(b, d, f)) == 0)
    return;
  if (pi == 0x10)
    ohci(r);
  else if (pi == 0x20)
    ehci(b, d, f, r);
  else if (pi == 0x30)
    xhci(r);
}

void usb_handoff(void) {
  uint32 b, d, f, id, class, hdr;

  for (b = 0; b < 256; b++)
    for (d = 0; d < 32; d++)
      for (f = 0; f < 8; f++) {
        if ((id = cfg_rd(b, d, f, 0)) == 0xffffffffu || (id & 0xffff) == 0xffff) {
          if (f == 0)
            break;
          continue;
        }
        class = cfg_rd(b, d, f, 0x08);
        if ((class >> 16) == 0x0c03)
          take(b, d, f, class);
        hdr = cfg_rd(b, d, f, 0x0c) >> 16;
        if (f == 0 && !(hdr & 0x80))
          break;
      }
}
