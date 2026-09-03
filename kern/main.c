#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "inc/product.h"
#include "net/adapter.h"
#include "x86.h"

static void mpmain(void) __attribute__((noreturn));
extern char end[]; // first address after kernel loaded from ELF file

// Bootstrap processor starts running C code here.
// Allocate a real stack and switch to it, first
// doing some setup required for memory allocator to work.
int main(void) {
  kinit1(end);   // phys page allocator
  kvmalloc();    // kernel page table
  mpinit();      // detect other processors
  lapicinit();   // interrupt controller
  seginit();     // segment descriptors
  picinit();     // disable pic
  ioapicinit();  // another interrupt controller
  framebuffer_init(); // VBE framebuffer, if the BIOS supplied one
  consoleinit(); // console hardware
  uartinit();    // serial port
  if (framebuffer_available())
    cprintf("VIDEO: VBE framebuffer %dx%d at 0x%x\n", framebuffer_width(),
            framebuffer_height(), framebuffer_phys());
  else
    cprintf("VIDEO: VBE framebuffer unavailable (reason %d, tag 0x%x, LFB "
            "0x%x, %dx%dx%d pitch %d); using VGA text mode\n",
            framebuffer_initialization_error(), framebuffer_boot_tag(),
            framebuffer_boot_physical_base(), framebuffer_boot_width(),
            framebuffer_boot_height(), framebuffer_boot_bits_per_pixel(),
            framebuffer_boot_pitch());
  fnustateinit(); // volatile local supervisor state
  proninx_net_init(); // adapter boundary for the vendored network stack
  cprintf("%s %s\n%s\n%s\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION,
          FNU_PRODUCT_VENDOR, FNU_PRODUCT_EXPANSION);
  pinit();         // process table
  tvinit();   // trap vectors
  binit();    // buffer cache
  fileinit(); // file table
  ufs2_init(); // UFS2 allocation metadata lock
  ideinit();  // disk
  storageinit(); // discover optional read-only FNU Data volume
  // startothers();   // start other processors
  kinit2();   // must come after startothers()
  virtio_net_init(); // QEMU reference network device
  proninx_lwip_init(); // DHCP, ARP, IPv4, ICMP and UDP on vtnet0
  userinit(); // first user process
  mpmain();   // finish this processor's setup
}

// Common CPU setup code.
static void mpmain(void) {
  cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
  idtinit();                    // load idt register
  xchg(&(mycpu()->started), 1); // tell startothers() we're up
  scheduler();                  // start running processes
}
