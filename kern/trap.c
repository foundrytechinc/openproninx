#include "trap.h"
#include "defs.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "x86.h"

// Interrupt descriptor table (shared by all CPUs).

struct gatedesc idt[256];
extern uintptr_t vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE << 3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE << 3, vectors[T_SYSCALL], DPL_USER);

  // these four cannot trust the stack they interrupted
  idt[T_DBLFLT].ist = IST_DBLFLT;
  idt[T_NMI].ist = IST_NMI;
  idt[T_MCHK].ist = IST_MCHK;
  idt[T_DEBUG].ist = IST_DEBUG;

  initlock(&tickslock, "time");
}

static uint strays;

void idtinit(void) {
  lidt(idt, sizeof(idt));
  mcheck_init(); // only once a handler exists to catch it
}

void trap(struct trapframe *tf) {
  if (tf->trapno == T_SYSCALL) {
    if (myproc()->killed) {
      exit();
    }
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed) {
      exit();
    }
    return;
  }

  switch (tf->trapno) {
  case T_IRQ0 + IRQ_TIMER:
    if (cpuid() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
      // Poll registered drivers on timer ticks as fallback
      driver_poll_all();
      proninx_lwip_timers();
      console_flush_if_dirty();
    }
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_IDE_PRIMARY:
    ideintr(0);
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE_SECONDARY:
    ideintr(1);
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_HALT:
    // the system is going down. on the boot processor that means finishing
    // the job the other core handed over, everywhere else it means stopping
    lapiceoi();
    cli();
    system_halt_handoff();
    for (;;)
      hlt();

  case T_MCHK:
    cprintf("cpu%d: machine check at rip 0x%x\n", cpuid(), tf->rip);
    if (mcheck_report("now") == 0)
      cprintf("MCE: no bank holds a record\n");
    panic("machine check");

  case T_DBLFLT:
    cprintf("cpu%d: double fault rip 0x%x rsp 0x%x cr2 0x%x\n", cpuid(),
            tf->rip, tf->rsp, rcr2());
    panic("double fault");

  default:
    // Dispatch interrupt through driver framework
    if (tf->trapno >= T_IRQ0 && tf->trapno < T_IRQ0 + IRQ_SPURIOUS &&
        driver_dispatch_irq(tf->trapno - T_IRQ0)) {
      lapiceoi();
      break;
    }
    if (tf->trapno >= T_IRQ0 && tf->trapno < T_IRQ0 + 16 &&
        pic_spurious((int)(tf->trapno - T_IRQ0)))
      break;
    // a wire nobody claims is not a reason to die
    if (tf->trapno == 15 || tf->trapno >= 20) {
      if (++strays <= 8)
        cprintf("cpu%d: stray interrupt, vector %d\n", cpuid(),
                (int)tf->trapno);
      if (tf->trapno >= T_IRQ0)
        lapiceoi();
      break;
    }
    if (myproc() == NULL || (tf->cs & 3) == 0) {
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d err 0x%x from cpu %d rip 0x%x rsp 0x%x "
              "(cr2=0x%x)\n",
              tf->trapno, tf->err, cpuid(), tf->rip, tf->rsp, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d ", myproc()->pid,
            myproc()->name, tf->trapno, tf->err, cpuid());
    cprintf("rip 0x%x addr 0x%x kill proc\n", tf->rip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER) {
    exit();
  }

  // Force process to give up CPU on clock tick.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == T_IRQ0 + IRQ_TIMER) {
    yield();
  }

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER) {
    exit();
  }
}
