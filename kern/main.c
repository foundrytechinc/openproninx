#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "inc/product.h"
#include "net/adapter.h"
#include "x86.h"

void mpmain(void) __attribute__((noreturn));
extern char end[]; // first address after kernel loaded from ELF file

// Bootstrap processor starts running C code here.
// Allocate a real stack and switch to it, first
// doing some setup required for memory allocator to work.
int main(void) {
  kinit1(end);   // phys page allocator
  kvmalloc();    // kernel page table
  acpi_init();   // ACPI table discovery (RSDP, RSDT/XSDT, MADT, FADT)
  mpinit();      // detect other processors (ACPI MADT with MPS fallback)
  lapicinit();   // interrupt controller
  seginit();     // segment descriptors
  picinit();     // disable pic
  ioapicinit();  // another interrupt controller
  framebuffer_init(); // VBE framebuffer, if the BIOS supplied one
  consoleinit(); // console hardware
  uartinit();    // serial port
  acpi_print_summary(); // Print ACPI detected status and tables
  cprintf("SMP: ncpu = %d\n", ncpu);
  if (framebuffer_available())
    cprintf("VIDEO: VBE framebuffer %dx%d at 0x%x (DISPI: %s)\n", framebuffer_width(),
            framebuffer_height(), framebuffer_phys(),
            framebuffer_has_dispi() ? "yes" : "no");
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
  ramdisk_init(); // embedded LiveCD rootfs
  ideinit();  // legacy IDE disk
  startothers();   // start other processors
  kinit2();   // must come after startothers()
  driver_framework_init(); // extensible driver framework
  e1000_driver_init(); // register Intel E1000 NIC driver
  virtio_net_driver_init(); // register VirtIO-Net fallback driver
  ahci_driver_init(); // register AHCI SATA storage driver
  nvme_driver_init(); // register NVMe PCIe SSD driver
  virtio_gpu_driver_init(); // register VirtIO-GPU display driver
  driver_attach_pci_devices(); // probe & attach PCI hardware
  storageinit(); // discover optional read-only FNU Data volume
  proninx_lwip_init(); // DHCP, ARP, IPv4, ICMP, UDP, TCP on network interface
  userinit(); // first user process
  mpmain();   // finish this processor's setup
}

// Common CPU setup code.
void mpmain(void) {
  cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
  idtinit();                    // load idt register
  xchg(&(mycpu()->started), 1); // tell startothers() we're up
  scheduler();                  // start running processes
}
