#include "defs.h"
#include "inc/signal.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "acpi.h"
#include "x86.h"

extern uint ticks;
extern struct cpu cpus[];

static void uptime(void) {
  uint s = ticks / HZ;

  cprintf("Uptime: %dd %dh %dm %ds\n", s / 86400, (s % 86400) / 3600,
          (s % 3600) / 60, s % 60);
}

// interrupts are about to go, so the tsc is the only clock left
static uint64_t stamp;
static uint device_ms, cpu_ms;

static uint elapsed_ms(void) {
  uint64_t hz = delay_tsc_hz(), now = rdtsc();
  uint ms;

  ms = hz >= 1000 ? (uint)((now - stamp) / (hz / 1000)) : 0;
  stamp = now;
  return ms;
}

// order from OpenBSD machdep.c boot()
static void quiesce(void) {
  stamp = rdtsc();
  driver_shutdown_all();
  device_ms = elapsed_ms();
  lapic_halt_others();
  cpu_ms = elapsed_ms();
  console_drop_lock();
  cli();
}

static void say(const char *what) {
  cprintf("%s (devices %d ms, cpus %d ms)\n", what, device_ms, cpu_ms);
}

static volatile int handoff_howto;
static volatile int handoff_pending;

// firmware reset paths want the boot processor, as FreeBSD cpu_reset does.
// mycpu() refuses to run with interrupts on, so read the apic directly.
static int on_boot_cpu(void) { return lapicid() == cpus[0].apicid; }

void system_halt_handoff(void) {
  if (!handoff_pending || !on_boot_cpu())
    return;
  handoff_pending = 0;
  system_halt(handoff_howto);
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

// Linux keeps the bits it did not come to change
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

static void machine_restart(void) {
  struct {
    uint16_t limit;
    uint64_t base;
  } __attribute__((packed)) null_idt = {0, 0};

  cli();
  // hard reset first: a modern chipset answers it and it does not cut power.
  // the firmware value can carry FULL_RST, so it waits.
  cprintf("reset: pch reset control, hard\n");
  pch_reset(0x06);
  cprintf("reset: keyboard controller\n");
  kbd_pulse();
  if (efirt_available()) {
    cprintf("reset: efi runtime services\n");
    efirt_reboot();
  }
  cprintf("reset: acpi reset register\n");
  acpi_reset_via_fadt();
  cprintf("reset: pch reset control, full\n");
  pch_reset(0x0e);
  cprintf("reset: fast init port\n");
  fast_init_reset();
  cprintf("reset: nothing worked, shutting the cpu down\n");
  __asm__ volatile("lidt (%0); int $3" : : "r"(&null_idt));
  for (;;)
    hlt();
}

void system_halt(int howto) {
  if (!on_boot_cpu() && ncpu > 1) {
    cprintf("Handing the shutdown to the boot processor\n");
    handoff_howto = howto;
    handoff_pending = 1;
    console_drop_lock();
    lapic_halt_to((uchar)cpus[0].apicid);
    cli();
    for (;;)
      hlt();
  }

  uptime();
  quiesce();
  switch (howto & 0xff) {
  case FNU_RB_REBOOT:
    say("Rebooting...");
    machine_restart();
    break;
  case FNU_RB_POWEROFF:
    say("Powering system off...");
    if (efirt_available()) {
      cprintf("poweroff: efi runtime services\n");
      efirt_poweroff();
    }
    cprintf("poweroff: acpi soft-off\n");
    acpi_poweroff();
    break;
  default:
    say("The operating system has halted.");
    break;
  }
  for (;;)
    hlt();
}

void system_poweroff(void) { system_halt(FNU_RB_POWEROFF); }

void system_reboot(void) { system_halt(FNU_RB_REBOOT); }
