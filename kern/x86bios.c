// X86BIOS: the video bios in x86emu
// low memory and roms are boot copies, the vga window is real. bios, мать твою
#include "defs.h"
#include "inc/bootinfo.h"
#include "memlayout.h"
#include "pci.h"
#include "realmode.h"
#include "x86.h"
#include <dev/x86emu/x86emu.h>
#include <dev/x86emu/x86emu_regs.h>

#define MEM_SIZE 0x100000
#define PAGE 0x1000
#define VGA_BASE 0xa0000
#define VGA_SIZE 0x20000
#define ROM_BASE 0xc0000
#define VIDEO_SEG_LO 0xc000
#define VIDEO_SEG_HI 0xe000
#define STACK_SEG 0x1000
#define STACK_TOP 0xfffe
#define IRET_LIN 0xfff53 // F000:FF53, where every bios keeps an iret
#define FETCH_CHECK 0xffff

static uint8_t low[VGA_BASE] __aligned(PAGE);
static uint8_t rom[MEM_SIZE - ROM_BASE] __aligned(PAGE);
static uint8_t *vga;
static uint8_t *pages[MEM_SIZE / PAGE];
static struct x86emu emu;
static struct mutex lock;
static int ready;
static uint64_t deadline;
static uint32_t fetches;
static int stopped; // 1 out of time, 2 outside the megabyte
static int rom_bdf = -1;

// int 10h is the video bios's: kept from boot, or set by our POST
int x86bios_ready(void) {
  uint32_t seg = ((uint32_t *)low)[0x10] >> 16;

  return ready && seg >= VIDEO_SEG_LO && seg < VIDEO_SEG_HI;
}

void *x86bios_buffer(void) { return &low[X86BIOS_BUF]; }

// what the bios sees at a linear address, or 0
void *x86bios_ptr(uint32_t lin) {
  lin &= MEM_SIZE - 1;
  return pages[lin / PAGE] != 0 ? pages[lin / PAGE] + lin % PAGE : 0;
}

static uint8_t *at(struct x86emu *e, uint32_t addr) {
  uint8_t *p = x86bios_ptr(addr);

  if (p == 0 || (addr >= MEM_SIZE && addr - MEM_SIZE >= 0x10000)) {
    stopped = 2;
    x86emu_halt_sys(e);
  }
  return p;
}

// bios data area tick count, 18.2 Hz, from the TSC
static uint32_t bios_ticks(void) {
  uint64_t hz = delay_tsc_hz();

  return hz != 0 ? (uint32_t)(rdtsc() / (hz * 10 / 182)) % 0x1800b0 : 0;
}

static uint8_t rdb(struct x86emu *e, uint32_t addr) {
  uint32_t t;

  if ((++fetches & FETCH_CHECK) == 0 && deadline != 0 && rdtsc() > deadline) {
    stopped = 1;
    x86emu_halt_sys(e);
  }
  if (addr >= 0x46c && addr < 0x470) {
    t = bios_ticks();
    memmove(&low[0x46c], &t, sizeof(t));
  }
  return *at(e, addr);
}

static uint16_t rdw(struct x86emu *e, uint32_t addr) {
  return (uint16_t)(rdb(e, addr) | rdb(e, addr + 1) << 8);
}

static uint32_t rdl(struct x86emu *e, uint32_t addr) {
  return (uint32_t)rdw(e, addr) | (uint32_t)rdw(e, addr + 2) << 16;
}

static void wrb(struct x86emu *e, uint32_t addr, uint8_t v) { *at(e, addr) = v; }

static void wrw(struct x86emu *e, uint32_t addr, uint16_t v) {
  wrb(e, addr, (uint8_t)v);
  wrb(e, addr + 1, (uint8_t)(v >> 8));
}

static void wrl(struct x86emu *e, uint32_t addr, uint32_t v) {
  wrw(e, addr, (uint16_t)v);
  wrw(e, addr + 2, (uint16_t)(v >> 16));
}

// port 0xb2 raises an SMI. no
static int kept_back(uint16_t port) { return port == 0xb2 || (port >= 0x80 && port < 0x88); }

static uint8_t in_b(struct x86emu *e __unused, uint16_t port) { return kept_back(port) ? 0 : inb(port); }

static uint16_t in_w(struct x86emu *e __unused, uint16_t port) { return kept_back(port) ? 0 : inw(port); }

static uint32_t in_l(struct x86emu *e __unused, uint16_t port) { return kept_back(port) ? 0 : inl(port); }

static void out_b(struct x86emu *e __unused, uint16_t port, uint8_t v) {
  if (!kept_back(port))
    outb(port, v);
}

static void out_w(struct x86emu *e __unused, uint16_t port, uint16_t v) {
  if (!kept_back(port))
    outw(port, v);
}

static void out_l(struct x86emu *e __unused, uint16_t port, uint32_t v) {
  if (!kept_back(port))
    outl(port, v);
}

// THE SYSTEM BIOS'S SERVICES

static void fail(struct x86emu *e, uint8_t ah) {
  e->x86.R_AH = ah;
  e->x86.R_FLG |= F_CF;
}

static void ok(struct x86emu *e) { e->x86.R_FLG &= ~(uint32_t)F_CF; }

// int 1Ah, PCI BIOS: bus, device, function in BX
static void pcibios(struct x86emu *e, int intno __unused) {
  uint bus = e->x86.R_BH, dev = (uint)e->x86.R_BL >> 3, fn = e->x86.R_BL & 7;
  uint off = e->x86.R_DI & 0xff;
  struct pci_device p;

  switch (e->x86.R_AX) {
  case 0xb101:
    e->x86.R_EDX = 0x20494350; // "PCI "
    e->x86.R_AL = 0x01;        // configuration mechanism 1
    e->x86.R_BX = 0x0210;
    e->x86.R_CL = 0xff;
    e->x86.R_AH = 0;
    ok(e);
    return;
  case 0xb102:
    if (e->x86.R_SI == 0 && pci_find_device(e->x86.R_DX, e->x86.R_CX, &p) == 0) {
      e->x86.R_BH = p.bus;
      e->x86.R_BL = (uint8_t)(p.device << 3 | p.function);
      e->x86.R_AH = 0;
      ok(e);
    } else
      fail(e, 0x86);
    return;
  case 0xb108:
    e->x86.R_CL = pci_read8(bus, dev, fn, off);
    break;
  case 0xb109:
    e->x86.R_CX = pci_read16(bus, dev, fn, off & ~1u);
    break;
  case 0xb10a:
    e->x86.R_ECX = pci_read32(bus, dev, fn, off & ~3u);
    break;
  case 0xb10b:
    pci_write8(bus, dev, fn, off, e->x86.R_CL);
    break;
  case 0xb10c:
    pci_write16(bus, dev, fn, off & ~1u, e->x86.R_CX);
    break;
  case 0xb10d:
    pci_write32(bus, dev, fn, off & ~3u, e->x86.R_ECX);
    break;
  default:
    if (e->x86.R_AH == 0x00) { // the tick count
      uint32_t t = bios_ticks();

      e->x86.R_CX = (uint16_t)(t >> 16);
      e->x86.R_DX = (uint16_t)t;
      e->x86.R_AL = 0;
      ok(e);
    } else
      fail(e, e->x86.R_AH == 0xb1 ? 0x81 : 0x86);
    return;
  }
  e->x86.R_AH = 0;
  ok(e);
}

// int 15h: panel hooks are the firmware's problem
static void sysbios(struct x86emu *e, int intno __unused) {
  if (e->x86.R_AH == 0x86) { // wait CX:DX microseconds
    delay((uint)e->x86.R_CX << 16 | e->x86.R_DX);
    e->x86.R_AH = 0;
    ok(e);
    return;
  }
  fail(e, 0x86);
}

static void equipment(struct x86emu *e, int intno __unused) { e->x86.R_AX = (uint16_t)(low[0x410] | low[0x411] << 8); }

static void memsize(struct x86emu *e, int intno __unused) { e->x86.R_AX = (uint16_t)(low[0x413] | low[0x414] << 8); }

static void nothing(struct x86emu *e, int intno __unused) { fail(e, 0x01); }

// ???
static int rom_image(const struct fnu_pcirom *r, const uint8_t **img, uint *len) {
  const uint8_t *p = P2V((uintptr_t)r->base);
  uint off = 0, pcir, n;

  while (off + 0x1a <= r->size && p[off] == 0x55 && p[off + 1] == 0xaa) {
    pcir = off + (uint)(p[off + 0x18] | p[off + 0x19] << 8);
    if (pcir + 0x18 > r->size || memcmp(&p[pcir], "PCIR", 4) != 0)
      return -1;
    n = (uint)(p[pcir + 0x10] | p[pcir + 0x11] << 8) * 512;
    if (n == 0 || off + n > r->size)
      return -1;
    if (p[pcir + 0x14] == 0) {
      *img = &p[off];
      *len = n;
      return 0;
    }
    if (p[pcir + 0x15] & 0x80)
      return -1;
    off += n;
  }
  return -1;
}

// no system bios under UEFI: the loader's rom, an empty data area, every vector an iret
static int from_loader(void) {
  const struct fnu_bootinfo *bi = bootinfo();
  const uint8_t *img;
  uint i, len;

  for (i = 0; bi != 0 && i < bi->pciroms; i++) {
    if (bi->pcirom[i].class != 0x030000 || rom_image(&bi->pcirom[i], &img, &len) < 0)
      continue;
    if (len > (VIDEO_SEG_HI - VIDEO_SEG_LO) << 4) {
      printf("x86bios: the %d KB video bios of %d:%d.%d does not fit\n", len / 1024,
             bi->pcirom[i].bus, bi->pcirom[i].slot, bi->pcirom[i].function);
      continue;
    }
    memset(low, 0, sizeof(low));
    memset(rom, 0, sizeof(rom));
    memmove(rom, img, len);
    low[0x410] = 0x21; // a colour adapter, 80x25
    low[0x413] = (uint8_t)(VGA_BASE / 1024);
    low[0x414] = (uint8_t)(VGA_BASE / 1024 >> 8);
    rom_bdf = bi->pcirom[i].bus << 8 | bi->pcirom[i].slot << 3 | bi->pcirom[i].function;
    return 0;
  }
  return -1;
}

// copies taken while the firmware's are whole
void x86bios_init(void) {
  uint32_t *ivt = (uint32_t *)low, seg;
  uint i;

  const char *off = kgetenv("hw.x86bios");

  if (off != 0 && off[0] == '0')
    return;
  if (realmode_available()) {
    memmove(low, P2V(0), sizeof(low));
    memmove(rom, P2V(ROM_BASE), sizeof(rom));
  } else if (from_loader() < 0)
    return;
  mtx_init(&lock, "x86bios");
  vga = P2V(VGA_BASE); // uncached by the fixed MTRRs
  for (i = 0; i < VGA_BASE / PAGE; i++)
    pages[i] = &low[i * PAGE];
  for (i = 0; i < VGA_SIZE / PAGE; i++)
    pages[VGA_BASE / PAGE + i] = vga + i * PAGE;
  for (i = 0; i < sizeof(rom) / PAGE; i++)
    pages[ROM_BASE / PAGE + i] = &rom[i * PAGE];

  // no POST memory manager: its code lives above the megabyte
  for (i = 0xe0000 - ROM_BASE; i + 4 <= sizeof(rom); i += 16)
    if (memcmp(&rom[i], "$PMM", 4) == 0)
      rom[i] = 0;

  // the video bios keeps its vectors; the rest are iret
  rom[IRET_LIN - ROM_BASE] = 0xcf;
  for (i = 0; i < 256; i++) {
    seg = ivt[i] >> 16;
    if (seg < VIDEO_SEG_LO || seg >= VIDEO_SEG_HI)
      ivt[i] = (IRET_LIN & 0xf0000u) << 12 | (IRET_LIN & 0xffffu);
  }

  memset(&emu, 0, sizeof(emu));
  emu.emu_rdb = rdb;
  emu.emu_rdw = rdw;
  emu.emu_rdl = rdl;
  emu.emu_wrb = wrb;
  emu.emu_wrw = wrw;
  emu.emu_wrl = wrl;
  emu.emu_inb = in_b;
  emu.emu_inw = in_w;
  emu.emu_inl = in_l;
  emu.emu_outb = out_b;
  emu.emu_outw = out_w;
  emu.emu_outl = out_l;
  emu._x86emu_intrTab[0x11] = equipment;
  emu._x86emu_intrTab[0x12] = memsize;
  emu._x86emu_intrTab[0x13] = nothing;
  emu._x86emu_intrTab[0x15] = sysbios;
  emu._x86emu_intrTab[0x16] = nothing;
  emu._x86emu_intrTab[0x1a] = pcibios;
  ready = 1;
}

static void load(const struct realregs *r) {
  memset(&emu.x86, 0, sizeof(emu.x86));
  emu.x86.R_AX = r->ax;
  emu.x86.R_BX = r->bx;
  emu.x86.R_CX = r->cx;
  emu.x86.R_DX = r->dx;
  emu.x86.R_SI = r->si;
  emu.x86.R_DI = r->di;
  emu.x86.R_ES = r->es;
  emu.x86.R_DS = r->es;
  emu.x86.R_SS = STACK_SEG;
  emu.x86.R_SP = STACK_TOP;
}

static void store(struct realregs *r) {
  r->ax = emu.x86.R_AX;
  r->bx = emu.x86.R_BX;
  r->cx = emu.x86.R_CX;
  r->dx = emu.x86.R_DX;
  r->si = emu.x86.R_SI;
  r->di = emu.x86.R_DI;
  r->es = emu.x86.R_ES;
}

// int vector, or far call; 0, or -1 on timeout or past the megabyte
static int run(int vector, uint16_t seg, uint16_t off, struct realregs *r, uint ms) {
  uint64_t hz = delay_tsc_hz();
  int err;

  if (!ready)
    return -1;
  mtx_enter(&lock);
  load(r);
  stopped = 0;
  deadline = hz != 0 ? rdtsc() + hz / 1000 * ms : 0;
  if (vector >= 0)
    x86emu_exec_intr(&emu, (uint8_t)vector);
  else
    x86emu_exec_call(&emu, seg, off);
  store(r);
  err = stopped;
  mtx_leave(&lock);
  if (err != 0)
    printf("x86bios: %s at %04x:%04x\n", err == 1 ? "out of time" : "outside the megabyte",
           emu.x86.R_CS, emu.x86.R_IP);
  return err != 0 ? -1 : 0;
}

int x86bios_int(int vector, struct realregs *r) { return run(vector, 0, 0, r, 2000); }

// a video bios to POST, whether or not it has run yet
int x86bios_has_rom(void) {
  return ready && rom[0] == 0x55 && rom[1] == 0xaa && (rom[3] == 0xe9 || rom[3] == 0xeb);
}

// the device the rom's PCI data names, or the first vga controller
static int rom_device(struct pci_device *p) {
  const uint8_t *pcir;
  uint off = (uint)(rom[0x18] | rom[0x19] << 8);

  if (off + 0x18 <= 0x8000) {
    pcir = &rom[off];
    if (memcmp(pcir, "PCIR", 4) == 0 &&
        pci_find_device((uint16_t)(pcir[4] | pcir[5] << 8), (uint16_t)(pcir[6] | pcir[7] << 8),
                        p) == 0)
      return 0;
  }
  return pci_find_device_by_class(0x03, 0x00, 0xff, p);
}

// rom POST: AH bus, AL devfn, DL 0x80
int x86bios_post(void) {
  struct pci_device p;
  struct realregs r;

  if (!x86bios_has_rom())
    return -1;
  memset(&r, 0, sizeof(r));
  if (rom_bdf >= 0)
    r.ax = (uint16_t)rom_bdf;
  else if (rom_device(&p) == 0)
    r.ax = (uint16_t)(p.bus << 8 | p.device << 3 | p.function);
  r.dx = 0x80;
  return run(-1, VIDEO_SEG_LO, 0x0003, &r, 5000);
}
