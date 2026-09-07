#include "defs.h"
#include "inc/bootinfo.h"
#include "inc/product.h"
#include "memlayout.h"
#include "param.h"
#include "mmu.h"
#include "net/adapter.h"
#include "realmode.h"
#include "proc.h"
#include "x86.h"

void mpmain(void) __attribute__((noreturn));

extern char end[]; // first address after kernel loaded from ELF file

// first page above the kernel and every module the loader left
static void *heap_start(void) {
  const struct fnu_bootinfo *info = bootinfo();
  uintptr_t top = (uintptr_t)V2P(end);
  uint i;

  if (info != 0)
    for (i = 0; i < info->modules; i++) {
      uintptr_t e = (uintptr_t)(info->module[i].base + info->module[i].size);
      if (e > top)
        top = e;
    }
  return P2V(PGROUNDUP(top));
}

void kmain(struct fnu_bootinfo *bi) {
  uartinit(); // serial first, so early failures are visible
  picinit();  // disable pic before the apic can latch its vectors
  delay_init(); // calibrate before anything waits on hardware
  bootinfo_init((uint64_t)(uintptr_t)bi);
  kinit1(heap_start()); // phys page allocator
  kvmalloc();    // kernel page table
  vmem_setup();  // console arena, now that the direct map exists
  efirt_init();  // uefi runtime services, for the reset it owns
  acpi_init();   // ACPI table discovery (RSDP, RSDT/XSDT, MADT, FADT)
  mpinit();      // detect other processors (ACPI MADT with MPS fallback)
  lapicinit();   // interrupt controller
  seginit();     // segment descriptors
  tvinit();             // trap vectors, before anything can fault
  idtinit();
  ioapicinit();  // another interrupt controller
  realmode_init();    // trampoline for calling the video bios later
  framebuffer_init(); // VBE framebuffer, if the BIOS supplied one
  consoleinit(); // console hardware
  uartintr_enable();
  bootinfo_print();
  acpi_print_summary(); // Print ACPI detected status and tables
  cprintf("SMP: ncpu = %d\n", ncpu);
  if (lapic_timer_hz())
    cprintf("CLOCK: apic timer %lu Hz, %lu counts per %d Hz tick\n",
            lapic_timer_hz(), (uint64_t)lapic_timer_reload(), HZ);
  else
    cprintf("CLOCK: apic timer would not calibrate, tick is a guess\n");
  efirt_report();
  mcheck_report_boot();
  if (framebuffer_available()) {
    cprintf("VIDEO: framebuffer %dx%d at 0x%x, size from %s, modeset %s\n",
            framebuffer_width(), framebuffer_height(), framebuffer_phys(),
            framebuffer_mode_source(),
            display_can_modeset() ? "available" : "fixed by firmware");
    cprintf("VIDEO: console %d x %d cells, %s\n", framebuffer_columns(),
            framebuffer_rows(), framebuffer_font_name());
  } else
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
  binit();    // buffer cache
  fileinit(); // file table
  ufs2_init(); // UFS2 allocation metadata lock
  ramdisk_init(); // embedded LiveCD rootfs
  ideinit();  // legacy IDE disk
  startothers();   // start other processors
  kinit2();   // must come after startothers()
  framebuffer_shadow_init();   // ram back buffer, needs kinit2
  driver_framework_init(); // extensible driver framework
  e1000_driver_init(); // register Intel E1000 NIC driver
  virtio_net_driver_init(); // register VirtIO-Net fallback driver
  ahci_driver_init(); // register AHCI SATA storage driver
  nvme_driver_init(); // register NVMe PCIe SSD driver
  virtio_gpu_driver_init(); // register VirtIO-GPU display driver
  driver_attach_pci_devices(); // probe & attach PCI hardware
  storageinit(); // discover optional read-only FNU Data volume
  proninx_lwip_init();
  userinit();
  mpmain();   // finish this processor's setup
}

// Common CPU setup code.
void mpmain(void) {
  cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
  idtinit();                    // load idt register
  xchg(&(mycpu()->started), 1); // tell startothers() we're up
  scheduler();                  // start running processes
}
