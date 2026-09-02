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
  consoleinit(); // console hardware
  uartinit();    // serial port
  fnustateinit(); // volatile local supervisor state
  proninx_net_init(); // adapter boundary for the vendored network stack
  cprintf("\n%s %s\n%s\n%s\n\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION,
          FNU_PRODUCT_VENDOR, FNU_PRODUCT_EXPANSION);
  pinit();         // process table
  tvinit();   // trap vectors
  binit();    // buffer cache
  fileinit(); // file table
  ideinit();  // disk
  storageinit(); // discover optional read-only FNU Data volume
  // startothers();   // start other processors
  kinit2();   // must come after startothers()
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
