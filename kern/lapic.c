// The local APIC manages internal (non-I/O) interrupts.
// See Chapter 8 and 10 of Intel SDM vol.3

#include "defs.h"
#include "param.h"
#include "trap.h"
#include "x86.h"

// Local APIC registers, divided by 4 for use as uint[] indices.
#define ID (0x0020 / 4)               // ID
#define VER (0x0030 / 4)              // Version
#define TPR (0x0080 / 4)              // Task Priority
#define EOI (0x00B0 / 4)              // EOI
#define SVR (0x00F0 / 4)              // Spurious Interrupt Vector
#define SVR_ENABLE 0x00000100         // Unit Enable
#define ESR (0x0280 / 4)              // Error Status
#define ICRLO (0x0300 / 4)            // Interrupt Command
#define ICR_DELIVM_INIT 0x00000500    // Delivery Mode: INIT/RESET
#define ICR_DELIVM_STARTUP 0x00000600 // Delivery Mode: Startup IPI
#define ICR_DELIVS 0x00001000         // Delivery status
#define ICR_LEVEL_ASSERT 0x00004000   // Level: Assert interrupt
#define ICR_TRIGGER_LEVEL 0x00008000  // Trigger Mode: Level
#define ICR_DEST_OTHERS 0x000c0000 // Destination: every APIC but this one
#define ICRHI (0x0310 / 4)  // Interrupt Command [63:32]
#define TIMER (0x0320 / 4)  // Local Vector Table 0 (TIMER)
#define TIMER_PERIODIC 0x00020000 // Periodic
#define PCINT (0x0340 / 4)        // Performance Counter LVT
#define LINT0 (0x0350 / 4)        // Local Vector Table 1 (LINT0)
#define LINT1 (0x0360 / 4)        // Local Vector Table 2 (LINT1)
#define ERROR (0x0370 / 4)        // Local Vector Table 3 (ERROR)
#define LVT_MASKED 0x00010000     // Interrupt masked
#define TICR (0x0380 / 4)         // Timer Initial Count
#define TCCR (0x0390 / 4)         // Timer Current Count
#define TDCR (0x03E0 / 4)         // Timer Divide Configuration
#define TDCR_X1 0x0000000B        // divide counts by 1

volatile uint32_t *lapic; // Initialized in mp.c

static void lapicw(int index, uint32_t value) {
  lapic[index] = value;
  lapic[ID]; // wait for write to finish, by reading
}

// Clear error status register (requires back-to-back writes).
// See Intel SDM vol.3 10.5.3 Error Handling
static void reset_esr(void) {
  // First write is to clear register.
  lapicw(ESR, 0);
  // According to the manual, local APIC might update the register
  // based on an error detected since the last write to the ESR.
  // It means one error might exist at most, so the second write
  // is required and enough to reset ESR?
  lapicw(ESR, 0);
}

// Ack any outstanding interrupts.
void lapiceoi(void) {
  if (lapic) {
    lapicw(EOI, 0);
  }
}

// counts per tick. the apic timer runs at the bus clock and nothing states
// what that is: 1 GHz on qemu, 24 or 100 MHz on kaby lake.
static uint32_t lapic_tval = 10000000;
static uint64_t lapic_per_second;
static int calibrated;

// after NetBSD lapic_calibrate_timer, against the already calibrated tsc
static void lapic_calibrate(void) {
  uint32_t first, last;
  uint64_t hz;

  lapicw(TIMER, LVT_MASKED);
  lapicw(TDCR, TDCR_X1);
  lapicw(TICR, 0x80000000);
  first = lapic[TCCR];
  delay(50000);
  last = lapic[TCCR];
  lapicw(TICR, 0);
  if (first <= last)
    return;

  hz = (uint64_t)(first - last) * 20;
  hz = hz / 1000 * 1000;
  if (hz < 1000000 || hz > 4000000000ULL)
    return;
  lapic_per_second = hz;
  lapic_tval = (uint32_t)(hz / HZ);
}

uint64_t lapic_timer_hz(void) { return lapic_per_second; }

uint32_t lapic_timer_reload(void) { return lapic_tval; }

void lapicinit(void) {
  if (!lapic) {
    return;
  }

  // Enable local APIC; set spurious interrupt vector.
  //
  // Set SVR_ENABLE of SVR is one way of enabling local APIC according to Intel
  // SVM 10.4.3. I'm not sure what spurious interrupt is, but it is something
  // like unexpected interrupt?
  lapicw(SVR, SVR_ENABLE | (T_IRQ0 + IRQ_SPURIOUS));

  // The timer repeatedly counts down at bus frequency from lapic[TICR] and
  // then issues an interrupt. See Intel SDM Vol3 10.5.4 APIC Timer
  if (!calibrated) {
    calibrated = 1;
    lapic_calibrate();
  }
  lapicw(TDCR, TDCR_X1);
  lapicw(TIMER, TIMER_PERIODIC | (T_IRQ0 + IRQ_TIMER));
  lapicw(TICR, lapic_tval);

  // Disable logical interrupt lines.
  lapicw(LINT0, LVT_MASKED);
  lapicw(LINT1, LVT_MASKED);

  // Disable performance counter overflow interrupts
  // on machines that provide that interrupt entry.
  //
  // According to Intel SDM vol.3 10.4.8 Local APIC Version Register,
  // the value returned is 4 for the P6 family processors (which have 5 LVT
  // entries).
  if (((lapic[VER] >> 16) & 0xFF) >= 4) {
    lapicw(PCINT, LVT_MASKED);
  }

  // Map error interrupt to IRQ_ERROR.
  lapicw(ERROR, T_IRQ0 + IRQ_ERROR);

  // Clear error status register (requires back-to-back writes).
  reset_esr();

  // Ack any outstanding interrupts.
  lapiceoi();

  // Enable interrupts on the APIC (but not on the processor).
  lapicw(TPR, 0);
}

int lapicid(void) {
  if (!lapic)
    return 0;
  return lapic[ID] >> 24;
}

// park every other core, as OpenBSD does before boot()
void lapic_halt_others(void) {
  if (!lapic)
    return;

  lapicw(ICRHI, 0);
  lapicw(ICRLO, ICR_DEST_OTHERS | ICR_LEVEL_ASSERT | (T_IRQ0 + IRQ_HALT));
  for (int i = 0; i < 1000 && (lapic[ICRLO] & ICR_DELIVS); i++)
    delay(100);
  delay(20000);
}

// firmware reset paths expect the boot processor. so does FreeBSD cpu_reset.
void lapic_halt_to(uchar apicid) {
  if (!lapic)
    return;

  lapicw(ICRHI, apicid << 24);
  lapicw(ICRLO, ICR_LEVEL_ASSERT | (T_IRQ0 + IRQ_HALT));
  for (int i = 0; i < 1000 && (lapic[ICRLO] & ICR_DELIVS); i++)
    delay(100);
}

void lapicstartap(uchar apicid, uint32_t addr) {
  int i;

  // "Universal startup algorithm."
  // Send INIT (level-triggered) interrupt to reset other CPU.
  lapicw(ICRHI, apicid << 24);
  lapicw(ICRLO, ICR_DELIVM_INIT | ICR_TRIGGER_LEVEL | ICR_LEVEL_ASSERT);
  delay(200);
  lapicw(ICRLO, ICR_DELIVM_INIT | ICR_TRIGGER_LEVEL);
  delay(100);

  // Send startup IPI (twice!) to enter code.
  for (i = 0; i < 2; i++) {
    lapicw(ICRHI, apicid << 24);
    lapicw(ICRLO, ICR_DELIVM_STARTUP | (addr >> 12));
    delay(200);
  }
}
