// SLEEP

#include "acpi.h"
#include "aml.h"
#include "blockdev.h"
#include "defs.h"
#include "driver.h"
#include "hibernate.h"
#include "vfs.h"
#include "inc/abi.h"
#include "kclock.h"
#include "malloc.h"
#include "memlayout.h"
#include "mmu.h"
#include "pci.h"
#include "proc.h"
#include "task.h"
#include "usbdi.h"
#include "wake.h"
#include "kbdlight.h"
#include "callout.h"
#include "x86.h"

extern uchar _binary_wakecode_start[];
extern uchar _binary_wakecode_size[];

struct cpuctx {
  uint8_t b[CTX_SIZE];
} acpi_cpuctx __aligned(16);

int acpi_savecpu(void *ctx);
void acpi_resume_entry(void);
void cpu_restart_others(void);
void clock_suspend(void);
void clock_resume(void);
void ioapic_save(void);
void ioapic_restore(void);
void uart_resume(void);
void atkbd_resume(void);
void psm_resume(void);
void display_suspend(void);
void display_resume(int state, int posted);
int display_resume_bios(void);
int realmode_available(void);

#define MSR_EFER 0xc0000080
#define EFER_LMA (1u << 10)

static struct waker {
  struct aml_node *dev;
  struct waker *link;
} *wakers;
static int sleeping;

static char power_state[8] = "S5";
static char sleep_state[8] = "S3";
static char lid_state[8] = "NONE";
static char suspend_state[8] = "auto"; // a sleep to RAM: auto, idle, S1 or S3
static int idling;                       // asleep in S0: buttons, lid, keys wake it
static int idle_wake(int why);
// platform: real S4, _PTS first
// shutdown: firmware hears nothing till the image is down
// _PTS kills the disk's power on some boards. fuck coreboot
static char hib_mode[12] = "shutdown";
static char supported[32];
static int request;
static int reset_video;      // hw.acpi.reset_video
static int pm_trace;         // debug.pm_trace
// presses that came in with the wake
static int buttons_asleep;
static uint64_t awake_at;
#define REPLAY_MS 1000

// pm_trace: докуда дошёл resume, в rtc. остальное Lenovo стирает
static const char *const pmt_name[] = {
    "the last step into sleep", "the wake code", "the video bios's POST",
    "the kernel", "this processor", "the clock", "the video bios's state",
    "the console", "the firmware", "the other processors",
    "the keyboard and mouse", "pci", "the irq routing", "the drivers",
    "usb", "done", "going down: the video bios's state", "going down: usb",
    "going down: the drivers", "going down: pci",
    "going down: parking the other processors", "going down: the firmware's _PTS",
    "going down: the wake devices", "going down: the clock",
    "going down: stopping the processes", "going down: the disks' last writes",
    "the video bios's POST in x86emu"};
static int pmt_driver = -1; // the driver the last resume or suspend stopped in

// when each step of the way back began
static struct {
  uint8_t step, sub;
  uint64_t tsc;
} stamps[64];
static int nstamps, timing;

int acpi_pm_trace_on(void) { return pm_trace; }

// step + driver, one number, one clock
#define PMT_STEPS 32
_Static_assert(nitems(pmt_name) <= PMT_STEPS, "pm_trace steps");
_Static_assert(RTC_IMAGE_MAX == WAKE_RTC_SLOT, "wake clock image");

// hung this soon after the way back = it got all the way. probably
#define UNTRACE_SECONDS 60
static struct callout untrace;

static void untrace_now(void *arg) {
  (void)arg;
  rtc_untrace();
}

void pm_trace_step(int step, int sub) {
  if (step == PMT_KERNEL) {
    timing = 1;
    nstamps = 0;
  }
  if (timing && nstamps < (int)nitems(stamps)) {
    stamps[nstamps].step = (uint8_t)step;
    stamps[nstamps].sub = (uint8_t)sub;
    stamps[nstamps++].tsc = rdtsc();
  }
  if (!pm_trace)
    return;
  rtc_trace((uint)step + PMT_STEPS * ((uint)sub & 0xff));
}

// the whole way back; steps over 20 ms get named
static void resume_times(int state) {
  uint64_t hz = delay_tsc_hz(), ms;
  const char *name;
  char line[320];
  size_t n;
  int i;

  timing = 0;
  if (!pm_trace || hz == 0 || nstamps < 2)
    return;
  n = (size_t)snprintf(line, sizeof(line), "acpi: back from S%d in %u ms", state,
                       (uint)((stamps[nstamps - 1].tsc - stamps[0].tsc) * 1000 / hz));
  for (i = 0; i + 1 < nstamps && n < sizeof(line); i++) {
    ms = (stamps[i + 1].tsc - stamps[i].tsc) * 1000 / hz;
    if (ms < 20)
      continue;
    name = stamps[i].step == PMT_DRIVERS ? driver_device_name(stamps[i].sub) : 0;
    if (name == 0)
      name = stamps[i].step < nitems(pmt_name) ? pmt_name[stamps[i].step] : "?";
    n += (size_t)snprintf(line + n, sizeof(line) - n, "%s %s %u ms", i ? "," : ":", name,
                          (uint)ms);
  }
  printf("%s\n", line);
}

// keyboard and mouse come back while the rest goes on
static void input_resume(void *arg __unused) {
  uint64_t hz = delay_tsc_hz(), t = rdtsc(), k;

  atkbd_resume();
  k = rdtsc();
  psm_resume();
  if (pm_trace && hz != 0)
    printf("acpi: keyboard back in %u ms, the mouse %u ms after it\n",
           (uint)((k - t) * 1000 / hz), (uint)((rdtsc() - k) * 1000 / hz));
}

static struct task input_task;

static void pm_trace_report(void) {
  int v = rtc_traced();
  uint step, sub;

  if (v < 0)
    return;
  step = (uint)v % PMT_STEPS;
  sub = (uint)v / PMT_STEPS;
  rtc_untrace();
  printf("pm_trace: the clock is to be set again\n");
  if (step == PMT_DONE) {
    printf("pm_trace: the last resume went all the way\n");
    return;
  }
  printf("pm_trace: the last %s stopped in %s (step %u",
         step == 0 || step >= PMT_S_VIDEO ? "suspend" : "resume",
         step < nitems(pmt_name) ? pmt_name[step] : "?", step);
  if (step == PMT_DRIVERS || step == PMT_S_DRIVERS) {
    printf(", driver %u", sub);
    pmt_driver = (int)sub;
  }
  printf(")\n");
}

// once the drivers are there to be named
void pm_trace_name_driver(void) {
  const char *name;

  if (pmt_driver >= 0 && (name = driver_device_name(pmt_driver)) != 0)
    printf("pm_trace: driver %d is %s\n", pmt_driver, name);
  pmt_driver = -1;
}

void acpi_wake_device(struct aml_node *dev) {
  struct waker *w = malloc(sizeof(*w), M_ACPI, M_WAITOK | M_ZERO);

  w->dev = dev;
  w->link = wakers;
  wakers = w;
}

// the GPE that wakes from state, or -1
static int wake_gpe(struct aml_node *dev, int state) {
  struct aml_value res, *g;
  int gpe = -1, lowest = 0;

  if (aml_eval_name(dev, "_PRW", 0, 0, &res) != 0)
    return -1;
  if (res.type == AML_PACKAGE && res.length >= 2) {
    g = res.pkg[0];
    gpe = (int)(g->type == AML_PACKAGE && g->length >= 2 ? aml_val2int(g->pkg[1])
                                                         : aml_val2int(g));
    lowest = (int)aml_val2int(res.pkg[1]);
  }
  aml_freevalue(&res);
  return gpe < 0 || state > lowest ? -1 : gpe;
}

// tell the device before it sleeps
static void wake_prep(struct aml_node *dev, int state, int on) {
  struct aml_value arg[3];

  if (wake_gpe(dev, state) < 0)
    return;
  memset(arg, 0, sizeof(arg));
  arg[0].type = arg[1].type = arg[2].type = AML_INTEGER;
  arg[0].integer = on;
  arg[1].integer = state;
  if (aml_eval_name(dev, "_DSW", 3, arg, 0) != 0)
    aml_eval_name(dev, "_PSW", 1, arg, 0);
}

static void set_int(const char *name, int v) {
  struct aml_value arg;

  memset(&arg, 0, sizeof(arg));
  arg.type = AML_INTEGER;
  arg.integer = v;
  aml_eval_name(aml_root(), name, 1, &arg, 0);
}

static uint64_t wake_pa; // from lowmem, once

// room for the wake code. before anything stops
static int wake_room(void) {
  if (wake_pa == 0 && (wake_pa = lowmem_alloc(WAKE_PAGES)) == 0)
    return -1;
  if ((uintptr_t)_binary_wakecode_size > WAKE_STACK - 0x100)
    panic("the wake code runs into its stack");
  return lowmem_tables() != 0 ? 0 : -1;
}

static void wake_prepare(int flags) {
  struct acpi_facs *facs = acpi_facs();
  uint8_t *w = P2V(wake_pa);

  memmove(w, _binary_wakecode_start, (uintptr_t)_binary_wakecode_size);
  *(uint32_t *)(w + WAKE_HDR_TABLES) = (uint32_t)lowmem_tables();
  *(uint64_t *)(w + WAKE_HDR_ENTRY) = (uint64_t)(uintptr_t)acpi_resume_entry;
  *(uint64_t *)(w + WAKE_HDR_EFER) = rdmsr(MSR_EFER) & ~(uint64_t)EFER_LMA;
  *(uint8_t *)(w + WAKE_HDR_FLAGS) = (uint8_t)flags;
  if (flags & WAKE_TRACE) {
    rtc_raw_regs(w + WAKE_HDR_CLOCK_REGS);
    rtc_trace_image(PMT_WAKECODE, w + WAKE_HDR_RTC);
    rtc_trace_image(PMT_VIDEO, w + WAKE_HDR_RTC + WAKE_RTC_SLOT);
  }
  facs->wakeup_vector = (uint32_t)wake_pa;
  if (facs->length >= 32 && facs->version >= 1)
    facs->x_wakeup_vector = 0;
}

// this cpu as it was
static uint32_t asleep_esr; // the error lapicinit is about to clear

static void cpu_resume(void) {
  int64_t moved;
  int i;

  asleep_esr = lapic_esr();
  lapic_x2apic_enable();
  mtrr_load();
  percpu_attach();
  gdt_init();
  // the stamps already taken go onto the TSC as it is now
  if ((moved = tsc_adjust_check()) != 0)
    for (i = 0; i < nstamps; i++)
      stamps[i].tsc += (uint64_t)moved;
  idtinit();
  fpuinit();
  i8259_init();
  lapicinit();
  ioapic_restore();
}

static int parse_state(const char *s) {
  if (s[0] == 'S' && s[1] >= '0' && s[1] <= '5' && s[2] == 0)
    return s[1] - '0';
  return -1;
}

static void act(const char *what) {
  int state = parse_state(what);

  if (state == ACPI_STATE_S5)
    acpi_powerbutton();
  else if (state == 1 || state == 3 || state == 4)
    acpi_sleep_request(state);
}

// the press that woke us. the firmware says it twice, fuckers
static int wake_press(void) {
  return buttons_asleep ||
         (awake_at != 0 && (ticks - awake_at) * 1000 / HZ < REPLAY_MS);
}

void acpi_power_pressed(void) {
  if (idle_wake(0))
    return;
  if (!wake_press())
    act(power_state);
}

void acpi_sleep_pressed(void) {
  if (idle_wake(1))
    return;
  if (!wake_press())
    act(sleep_state);
}

// last in the queue, behind everything the wake latched
static void buttons_wake(void *arg0 __unused, int turns) {
  if (acpi_tasks_pending() && turns < 64) {
    acpi_task_add(buttons_wake, 0, turns + 1);
    return;
  }
  awake_at = ticks != 0 ? ticks : 1;
  buttons_asleep = 0;
}

void acpi_lid_closed(void) { act(lid_state); }

static void sleep_task(void *arg0 __unused, int state) {
  int err = acpi_sleep(state);

  if (err != 0)
    printf("acpi: no sleep to S%d (%d)\n", state, err);
}

void acpi_sleep_request(int state) {
  if (sleeping)
    return;
  acpi_task_add(sleep_task, 0, state);
}

static int can_sleep(int state) {
  if (state == 1)
    return acpi_sleep_supported(1);
  if (state == 3)
    return acpi_sleep_supported(3) && acpi_facs() != 0;
  return state == 4 && acpi_sleep_supported(ACPI_STATE_S5);
}

static int hib_platform(void) {
  return strncmp(hib_mode, "platform", sizeof(hib_mode)) == 0 &&
         acpi_sleep_supported(4);
}

// the firmware hears about it before the devices sleep
static int firmware_told(int state) { return state == 1 || state == 3 || hib_platform(); }

// 0 is idle. S3 unless the screen stays dark
static int suspend_target(void) {
  if (strcmp(suspend_state, "idle") == 0)
    return 0;
  if (strcmp(suspend_state, "S1") == 0 && can_sleep(1))
    return 1;
  if (strcmp(suspend_state, "S3") == 0 && can_sleep(3))
    return 3;
  if (can_sleep(3) && display_back_after_s3())
    return 3;
  return 0;
}

// IDLE: S0, firmware not told. processes frozen, panel dark
static int idle_enter(void) {
  if (sleeping)
    return FNU_ERR_BUSY;
  sleeping = 1;
  kbdlight_suspend();
  if (proc_freeze_all() < 0) {
    kbdlight_resume();
    sleeping = 0;
    return FNU_ERR_BUSY;
  }
  vfs_sync();
  blockdev_stall();
  acpi_backlight_off();
  idling = 1;
  printf("acpi: asleep in S0; the power button, the lid or a key wakes it\n");
  return 0;
}

static void idle_leave(void *arg0 __unused, int why) {
  static const char *const by[] = {"the power button", "the sleep button", "the lid", "a key"};

  if (!idling)
    return;
  idling = 0;
  acpi_backlight_on();
  blockdev_restart();
  proc_thaw_all();
  kbdlight_resume();
  printf("acpi: woke from idle by %s\n", by[why]);
  sleeping = 0;
}

static int idle_wake(int why) {
  if (!idling)
    return 0;
  acpi_task_add(idle_leave, 0, why);
  return 1;
}

int acpi_idle_key(void) { return idle_wake(3); }

void acpi_lid_opened(void) { idle_wake(2); }

// DO NOT WRITE A FILESYSTEM HERE
static void __dead hibernate_off(void) {
  struct waker *w;

  blockdev_flush_all();
  if (hib_platform()) {
    for (w = wakers; w != 0; w = w->link)
      wake_prep(w->dev, 4, 1);
    usbd_suspend();
    driver_suspend_all();
    acpi_tpm_suspend();
    pci_suspend();
    set_int("\\_PTS", 4);
    set_int("\\_SI_._SST", 4);
    cpu_park_others();
    acpi_sleep_events();
    for (w = wakers; w != 0; w = w->link) {
      int gpe = wake_gpe(w->dev, 4);

      if (gpe >= 0)
        acpi_gpe_enable(gpe);
    }
    pm_trace_step(0, 0);
    spinlock_enter();
    acpi_sleep_registers(4);
    spinlock_exit();
  } else {
    driver_shutdown_all();
    acpi_tpm_shutdown();
  }
  pm_trace_step(0, 0);
  acpi_poweroff_prepare(1);
  acpi_poweroff();
  printf("hibernate: power down manually\n");
  for (;;)
    hlt();
}

static int hib_error(int err) {
  if (err == HIB_NOSWAP)
    return FNU_ERR_NOSPACE;
  return err == HIB_NOMEM ? FNU_ERR_NOMEMORY : FNU_ERR_IO;
}

static int sleep_once(int state, int *again) {
  struct waker *w;
  uint8_t *wake;
  int woke, err = 0, video, imaged, fw = firmware_told(state);

  if (state != 1 && state != 3 && state != 4)
    return FNU_ERR_UNSUPPORTED;
  if (!can_sleep(state))
    return FNU_ERR_WRONGDEVICE;
  if (wake_room() < 0) {
    printf("acpi: no low memory for the wake code\n");
    return FNU_ERR_WRONGDEVICE;
  }
  if (sleeping)
    return FNU_ERR_BUSY;
  sleeping = 1;
  buttons_asleep = 1;
  kbdlight_suspend();
  if (state == 4 && hibernate_prepare() < 0) {
    buttons_asleep = 0;
    sleeping = 0;
    return FNU_ERR_NOSPACE;
  }
  if (fw)
    set_int("\\_TTS", state);
  // a traced wake's record still in the clock
  if (callout_stop_async(&untrace) && !pm_trace)
    rtc_untrace();
  // stop the processes and the filesystems
  pm_trace_step(PMT_S_FREEZE, 0);
  if (proc_freeze_all() < 0) {
    err = FNU_ERR_BUSY;
    goto unfrozen;
  }
  if (state == 4 && hibernate_reserve() < 0) {
    printf("hibernate: no memory for the image\n");
    proc_thaw_all();
    err = FNU_ERR_NOMEMORY;
    goto unfrozen;
  }
  pm_trace_step(PMT_S_IO, 0);
  vfs_sync();
  blockdev_stall();
  pm_trace_step(PMT_S_VIDEO, 0);
  display_suspend();
  pm_trace_step(PMT_S_WAKERS, 0);
  for (w = wakers; fw && w != 0; w = w->link)
    wake_prep(w->dev, state, 1);
  pm_trace_step(PMT_S_USB, 0);
  usbd_suspend();
  if (driver_suspend_all() != 0) {
    usbd_resume(0);
    for (w = wakers; fw && w != 0; w = w->link)
      wake_prep(w->dev, state, 0);
    blockdev_restart();
    proc_thaw_all();
    err = FNU_ERR_BUSY;
    goto unfrozen;
  }
  acpi_tpm_suspend();
  pm_trace_step(PMT_S_PCI, 0);
  pci_suspend();
  pm_trace_step(PMT_S_PTS, 0);
  if (fw) {
    set_int("\\_PTS", state);
    set_int("\\_SI_._SST", state == 4 ? 4 : 3);
  }
  pm_trace_step(PMT_S_PARK, 0);
  cpu_park_others();
  pm_trace_step(PMT_S_CLOCK, 0);
  acpi_sleep_events();
  for (w = wakers; fw && w != 0; w = w->link) {
    int gpe = wake_gpe(w->dev, state);

    if (gpe >= 0)
      acpi_gpe_enable(gpe);
  }

  // real-mode POST only when we came from a bios
  video = state == 3 && realmode_available() && reset_video;
  spinlock_enter();
  lapic_suspend();
  clock_suspend();
  ioapic_save();
  wake_prepare((pm_trace ? WAKE_TRACE : 0) | (video ? WAKE_VIDEO : 0));
  pm_trace_step(0, 0);
  woke = 0;
  imaged = 0;
  // S1 keeps the cpu: it goes on right after the write
  if (state == 1)
    woke = acpi_sleep_registers(1) == 0;
  else if (acpi_savecpu(&acpi_cpuctx) == 0) {
    if (state == 4)
      imaged = (err = hibernate_suspend()) == 0;
    else
      acpi_sleep_registers(state);
  } else
    woke = 1;
  wake = P2V(wake_pa);
  if (woke)
    acpi_wake_capture();
  if (woke && pm_trace)
    clock_woke(state == 3 && wake[WAKE_HDR_CLOCK_TAKEN] ? wake + WAKE_HDR_CLOCK : 0);
  pm_trace_step(PMT_KERNEL, 0);
  cpu_resume();
  pm_trace_step(PMT_CPU, 0);
  if (woke && state == 4)
    hibernate_resumed();
  clock_resume();
  pm_trace_step(PMT_CLOCK, 0);
  spinlock_exit();
  cpu_restart_others();
  pm_trace_step(PMT_CPUS, 0);
  // config space first, the display's included
  pci_resume();
  pm_trace_step(PMT_PCI, 0);
  acpi_tpm_resume();
  if (err != 0)
    hibernate_explain(err);

  uart_resume();
  // the display only reads memory: nothing to restore for a write
  if (woke)
    display_resume(state, video);
  pm_trace_step(PMT_CONSOLE, 0);
  if (asleep_esr != 0)
    printf("cpu0: local APIC error 0x%x while it slept\n", asleep_esr);
  if (imaged)
    hibernate_made();
  else if (woke && (state == 1 || state == 3)) {
    char why[16];

    printf("acpi: woke from S%d by %s\n", state, acpi_wake_reason(why, sizeof(why)));
  } else
    printf("acpi: %s S%d\n", woke ? "woke from" : "did not go to", state);
  acpi_wake_registers();
  if (fw) {
    for (w = wakers; w != 0; w = w->link)
      wake_prep(w->dev, state, 0);
    set_int("\\_WAK", state);
    set_int("\\_TTS", 0);
    set_int("\\_SI_._SST", 1);
  }
  pm_trace_step(PMT_WAK, 0);
  task_set(&input_task, input_resume, 0);
  task_add(systq, &input_task);
  pm_trace_step(PMT_INPUT, 0);
  acpi_prt_resume();
  pm_trace_step(PMT_PRT, 0);
  driver_resume_all();
  pm_trace_step(PMT_USB, 0);
  usbd_resume(woke && state == 4);
  if (imaged) {
    if (hibernate_write() == 0)
      hibernate_off();
    printf("hibernate: the image could not be written\n");
    err = HIB_WRITE;
  }
  blockdev_restart();
  if (err == HIB_NOMEM)
    *again = hibernate_shrink() > 0;
  proc_thaw_all();
  pm_trace_step(PMT_DONE, 0);
  if (pm_trace)
    callout_reset(&untrace, UNTRACE_SECONDS * HZ, untrace_now, 0);
  resume_times(state);
  acpi_poll_now();
  acpi_sci_poll();
  acpi_ec_resume();
  kbdlight_resume();
  acpi_task_add(buttons_wake, 0, 0);
  if (state == 4)
    hibernate_unprepare();
  sleeping = 0;
  return err != 0 ? hib_error(err) : 0;

unfrozen:
  if (fw)
    set_int("\\_TTS", 0);
  if (state == 4)
    hibernate_unprepare();
  if (pm_trace)
    rtc_untrace();
  buttons_asleep = 0;
  sleeping = 0;
  return err;
}

// again, if hibernate_shrink made room
int acpi_sleep(int state) {
  int err, again, tries;

  if (state == 3 && (state = suspend_target()) == 0)
    return idle_enter();
  for (tries = 0;; tries++) {
    again = 0;
    err = sleep_once(state, &again);
    if (!again || tries == 2)
      return err;
  }
}

static int sleep_written(void *arg __unused, int old __unused) {
  int state = request;

  request = 0;
  if (!can_sleep(state))
    return FNU_ERR_UNSUPPORTED;
  acpi_sleep_request(state);
  return 0;
}

static int state_written(void *arg, int old __unused) {
  char *s = arg;

  if (strcmp(s, "NONE") != 0 && parse_state(s) != 1 && parse_state(s) != 3 &&
      parse_state(s) != 4 && parse_state(s) != ACPI_STATE_S5) {
    strlcpy(s, "NONE", 8);
    return FNU_ERR_INVALID;
  }
  return 0;
}

static int suspend_written(void *arg, int old __unused) {
  char *s = arg;

  if (strcmp(s, "auto") != 0 && strcmp(s, "idle") != 0 && strcmp(s, "S1") != 0 &&
      strcmp(s, "S3") != 0) {
    strlcpy(s, "auto", 8);
    return FNU_ERR_INVALID;
  }
  return 0;
}

static int mode_written(void *arg, int old) {
  char *s = arg;

  (void)old;
  if (strncmp(s, "platform", 12) != 0 && strncmp(s, "shutdown", 12) != 0) {
    strlcpy(s, "shutdown", 12);
    return FNU_ERR_INVALID;
  }
  return 0;
}

void acpi_sleep_sysctl(void) {
  int i;

  pm_trace_report();
  sysctl_add_int("hw.acpi.reset_video", &reset_video, FNU_CTLF_WRITE, 0, 1);
  sysctl_add_int("debug.pm_trace", &pm_trace, FNU_CTLF_WRITE, 0, 1);

  for (i = 1; i <= ACPI_STATE_S5; i++)
    if (acpi_sleep_supported(i)) {
      size_t n = strlen(supported);

      snprintf(supported + n, sizeof(supported) - n, "%sS%d", n ? " " : "", i);
    }
  sysctl_add_string("hw.acpi.supported_sleep_state", supported,
                    sizeof(supported), 0);
  if (can_sleep(3))
    strlcpy(lid_state, "S3", sizeof(lid_state));
  sysctl_add_int("hw.acpi.sleep", &request, FNU_CTLF_WRITE, 0, 5);
  sysctl_on_write("hw.acpi.sleep", sleep_written, 0);
  sysctl_add_string("hw.acpi.power_button_state", power_state,
                    sizeof(power_state), FNU_CTLF_WRITE);
  sysctl_on_write("hw.acpi.power_button_state", state_written, power_state);
  sysctl_add_string("hw.acpi.sleep_button_state", sleep_state,
                    sizeof(sleep_state), FNU_CTLF_WRITE);
  sysctl_on_write("hw.acpi.sleep_button_state", state_written, sleep_state);
  sysctl_add_string("hw.acpi.lid_switch_state", lid_state, sizeof(lid_state),
                    FNU_CTLF_WRITE);
  sysctl_on_write("hw.acpi.lid_switch_state", state_written, lid_state);
  sysctl_add_string("hw.acpi.suspend_state", suspend_state, sizeof(suspend_state),
                    FNU_CTLF_WRITE);
  sysctl_on_write("hw.acpi.suspend_state", suspend_written, suspend_state);
  sysctl_add_string("hw.acpi.hibernate_mode", hib_mode, sizeof(hib_mode),
                    FNU_CTLF_WRITE);
  sysctl_on_write("hw.acpi.hibernate_mode", mode_written, hib_mode);
}
