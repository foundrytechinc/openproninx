#include "acpi.h"
#include "blockdev.h"
#include "defs.h"
#include "malloc.h"
#include "inc/bootinfo.h"
#include "inc/product.h"
#include "memlayout.h"
#include "param.h"
#include "sched.h"
#include "task.h"
#include "mmu.h"
#include "net/if.h"
#include "realmode.h"
#include "ufs2.h"
#include "proc.h"
#include "x86.h"

void mp_startup_common(void) __dead;

extern char end[];

void kmain(struct fnu_bootinfo *bi) {
  com_init();
  i8259_init();
  bootinfo_init((uint64_t)(uintptr_t)bi);
  page_init(end);
  nxinit();
  hat_kern_setup();
  page_boot_tables_free();
  kernel_text_init();
  lowmem_init();
  msgbuf_attach(msgbuf_reserved());
  contig_setup();
  wsmouseinit();
  uaudioinit();
  vfs_init();
  devfs_init();
  realmode_init();
  framebuffer_init();
  consoleinit();
  delay_init();
  uptime_init();
  efirt_init();
  acpi_init();
  clockinit();
  mpinit();
  lapicinit();
  percpu_attach();
  gdt_init();
  tvinit();
  idtinit();
  lapic_esr_check("the local APIC set up");
  lockup_start();
  contig_lock_enable();
  pci_lock_enable();
  kmeminit();
  kheap_init();
  sysctl_init();
  init_param2();
  sched_init();
  param_sysctl();
  shutdown_sysctl();
  trap_sysctl();
  idle_sysctl();
  kernel_text_sysctl();
  callout_startup();
  socketinit();
  smbios_init();
  smbios_sysctl();
  intr_init();
  ioapicinit();
  lapic_esr_check("the I/O APICs");
  console_enable_input();
  msgbuf_init();
  uartintr_enable();
  uart_announce();
  bootinfo_print();
  acpi_print_summary();
  mtrr_check_ram();
  if (!lapic_timer_hz())
    printf("CLOCK: apic timer would not calibrate, tick is a guess\n");
  efirt_report();
  mcheck_report_boot();
  if (framebuffer_available()) {
    printf("VIDEO: framebuffer %dx%d at 0x%lx, size from %s, modeset %s, back from sleep %s\n",
            framebuffer_width(), framebuffer_height(), framebuffer_phys(),
            framebuffer_mode_source(),
            display_can_modeset() ? "available" : "fixed by firmware", display_resume_how());
    printf("VIDEO: console %d x %d cells, %s\n", framebuffer_columns(),
            framebuffer_rows(), framebuffer_font_name());
  } else
    printf("VIDEO: VBE framebuffer unavailable (reason %d, LFB "
            "0x%lx, %dx%dx%d pitch %d); using VGA text mode\n",
            framebuffer_initialization_error(),
            framebuffer_boot_physical_base(), framebuffer_boot_width(),
            framebuffer_boot_height(), framebuffer_boot_bits_per_pixel(),
            framebuffer_boot_pitch());
  randominit();
  randomdevinit();
  uvminit();
  futex_init();
  memdevinit();
  hotpluginit();
  sysmouseinit();
  partition_init();
  net_init();
  printf("%s %s\n%s\n%s\n", FNU_PRODUCT_NAME, FNU_PRODUCT_VERSION,
          FNU_PRODUCT_VENDOR, FNU_PRODUCT_EXPANSION);
  pinit();
  fileinit();
  pipeinit();
  eventinit();
  fifoinit();
  ufs2_init();
  ramdisk_init();
  startothers();
  lapic_esr_check("starting the other cpus");
  page_init_rest(); // after startothers
  binit();
  blockdev_stage_init();
  framebuffer_shadow_init();
  acpi_start();
  lapic_esr_check("acpi");
  psminit();
  driver_framework_init();
  e1000_driver_init();
  virtio_net_driver_init();
  re_driver_init();
  pciide_driver_init();
  ahci_driver_init();
  nvme_driver_init();
  virtio_gpu_driver_init();
  usbd_init();
  uhub_init();
  hid_init();
  ukbd_init();
  ums_init();
  umass_init();
  uaudio_init();
  xhci_driver_init();
  ehci_driver_init();
  dmar_init();
  driver_attach_pci_devices();
  pm_trace_name_driver();
  create_init();
  softclock_start();
  taskq_start();
  netisr_start();
  acpi_thread_start();
  kthread_create(console_refresh_thread, 0, 0, "consrefresh");
  kthread_create(usbd_explore_thread, 0, 0, "usbexplore");
  kthread_create(bio_syncer, 0, 0, "syncer");
  kthread_create(ufs2_logger, 0, 0, "ufs2log");
  kthread_create(pagedaemon, 0, 0, "pagedaemon");
  kthread_create(kheap_reclaimer, 0, 0, "kheap");
  lapic_esr_check("the drivers");
  lockup_stop();
  lapic_esr_check("the lockup watch");
  mp_startup_common();
}

// every cpu comes through here
void mp_startup_common(void) {
  nxinit();
  idtinit();
  fpuinit();
  curcpu()->pcputicks = ticks;
  xchg(&(curcpu()->started), 1);
  scheduler();
}
