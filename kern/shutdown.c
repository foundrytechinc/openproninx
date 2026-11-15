#include "blockdev.h"
#include "defs.h"
#include "inc/abi.h"
#include "inc/signal.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "acpi.h"
#include "x86.h"
#include "vfs.h"

extern struct cpu_info cpus[];

static void uptime(void) {
  uint64_t s = ticks / HZ;

  printf("Uptime: %dd %dh %dm %ds\n", (int)(s / 86400), (int)(s % 86400 / 3600),
          (int)(s % 3600 / 60), (int)(s % 60));
}

static void quiesce(int howto) {
  vfs_sync();
  msgbuf_dump(0);
  vfs_dismount_all();
  blockdev_flush_all();
  if ((howto & 0xff) != FNU_HALT_REBOOT)
    blockdev_powerdown_all();
  driver_shutdown_all();
  acpi_tpm_shutdown();
  acpi_poweroff_prepare((howto & 0xff) == FNU_HALT_POWEROFF);
}

static void alone(void) {
  lapic_halt_others();
  console_drop_lock();
  cli();
}

static void say(const char *what) {
  printf("%s\n", what);
}

static volatile int handoff_howto;
static volatile int handoff_pending;

static int on_boot_cpu(void) { return (uint32_t)lapicid() == cpus[0].apicid; }

static void finish(int howto) __dead;

void system_halt_handoff(void) {
  if (!handoff_pending || !on_boot_cpu())
    return;
  handoff_pending = 0;
  finish(handoff_howto);
}

static void kbd_pulse(void) {
  int i, spin;

  for (i = 0; i < 10; i++) {
    for (spin = 0; spin < 10000 && (inb(0x64) & 0x02); spin++)
      delay(10);
    delay(50);
    outb(0x64, 0xFE);
    delay(50);
  }
  delay(100000);
}

static void pch_reset(uint8_t code) {
  uint8_t cf9 = (uint8_t)(inb(0xcf9) & ~code);

  outb(0xcf9, (uint8_t)(cf9 | 0x02));
  delay(50);
  outb(0xcf9, (uint8_t)(cf9 | code));
  delay(200000);
}

static void fast_init_reset(void) {
  int v = inb(0x92);

  if (v == 0xff)
    return;
  if (v & 0x01)
    outb(0x92, (uint8_t)(v & 0xfe));
  outb(0x92, (uint8_t)(v | 0x01));
  delay(200000);
}

void machine_restart(void) {
  struct {
    uint16_t limit;
    uint64_t base;
  } __packed null_idt = {0, 0};

  cli();
  pch_reset(0x06);
  kbd_pulse();
  if (efirt_available())
    efirt_reboot();
  acpi_reset_via_fadt();
  pch_reset(0x0e);
  fast_init_reset();
  printf("reset: nothing worked, shutting the cpu down\n");
  __asm __volatile("lidt (%0); int $3" : : "r"(&null_idt));
  for (;;)
    hlt();
}

void system_halt(int howto) {
  uptime();
  quiesce(howto);
  if (!on_boot_cpu() && ncpus > 1) {
    handoff_howto = howto;
    handoff_pending = 1;
    console_drop_lock();
    lapic_halt_to(cpus[0].apicid);
    cli();
    curcpu()->halted = 1;
    for (;;)
      hlt();
  }
  finish(howto);
}

static void finish(int howto) {
  alone();
  switch (howto & 0xff) {
  case FNU_HALT_REBOOT:
    say("Rebooting...");
    machine_restart();
    break;
  case FNU_HALT_POWEROFF:
    say("Powering system off...");
    if (efirt_available())
      efirt_poweroff();
    acpi_poweroff();
    break;
  default:
    say("The operating system has halted.");
    break;
  }
  for (;;)
    hlt();
}

#define PANIC_REBOOT_WAIT_TIME 15
static int panic_reboot_wait_time = PANIC_REBOOT_WAIT_TIME;

void shutdown_sysctl(void) {
  int n;

  if (getenv_int("kern.panic_reboot_wait_time", &n) && n >= -1)
    panic_reboot_wait_time = n;
  sysctl_add_int("kern.panic_reboot_wait_time", &panic_reboot_wait_time,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, -1, 1 << 30);
}

void shutdown_panic(void) {
  int loop;

  if (panic_reboot_wait_time == 0)
    return;
  if (panic_reboot_wait_time != -1) {
    printf("Automatic reboot in %d seconds - press a key on the console to abort\n",
           panic_reboot_wait_time);
    for (loop = panic_reboot_wait_time * 10; loop > 0; --loop) {
      delay(1000 * 100);
      if (cncheckc() != -1)
        break;
    }
    if (loop == 0)
      return;
  }
  printf("--> Press a key on the console to reboot,\n"
         "--> or switch off the system now.\n");
  while (cncheckc() == -1)
    delay(1000 * 10);
}

void system_poweroff(void) { system_halt(FNU_HALT_POWEROFF); }

void system_reboot(void) { system_halt(FNU_HALT_REBOOT); }
