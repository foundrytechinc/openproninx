#include "trap.h"
#include "defs.h"
#include "memlayout.h"
#include "uvm.h"
#include "mmu.h"
#include "proc.h"
#include "sched.h"
#include "mutex.h"
#include "x86.h"
#include "inc/abi.h"


struct gatedesc idt[256];
extern uintptr_t vectors[];
volatile uint64_t ticks;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE << 3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 0, SEG_KCODE << 3, vectors[T_SYSCALL], DPL_USER);
  SETGATE(idt[T_BRKPT], 0, SEG_KCODE << 3, vectors[T_BRKPT], DPL_USER);

  idt[T_DBLFLT].ist = IST_DBLFLT;
  idt[T_NMI].ist = IST_NMI;
  idt[T_MCHK].ist = IST_MCHK;
  idt[T_DEBUG].ist = IST_DEBUG;
}

static uint strays;

#define NMI_PARITY (1 << 7)
#define NMI_IOCHAN (1 << 6)

static int panic_on_nmi = 0xff;

void trap_sysctl(void) {
  int n;

  if (getenv_int("machdep.panic_on_nmi", &n))
    panic_on_nmi = n;
  sysctl_add_int("machdep.panic_on_nmi", &panic_on_nmi,
                 FNU_CTLF_STDIO | FNU_CTLF_WRITE, 0, 0xff);
}

static void nmi_call_kdb(void) {
  int port = inb(0x61), claimed = 0;

  printf("NMI ISA %x\n", port);
  if (port & NMI_PARITY) {
    printf("RAM parity error, likely hardware failure.\n");
    claimed = 1;
  }
  if (port & NMI_IOCHAN) {
    printf("I/O channel check, likely hardware failure.\n");
    claimed = 1;
  }
  if (claimed && (panic_on_nmi & 1) != 0)
    panic("NMI indicates hardware failure");
  if (!claimed && panic_on_nmi != 0)
    panic("NMI");
}

static volatile int nmi_kdb_lock;

static void nmi_intr(void) {
  if (__sync_bool_compare_and_swap(&nmi_kdb_lock, 0, 1)) {
    nmi_call_kdb();
  } else {
    curcpu()->halted = 1;
    while (!__sync_bool_compare_and_swap(&nmi_kdb_lock, 0, 1))
      __asm __volatile("pause");
    curcpu()->halted = 0;
  }
  __sync_lock_release(&nmi_kdb_lock);
}

void idtinit(void) {
  lidt(idt, sizeof(idt));
  mcheck_init();
}

void trap(struct trapframe *tf) {
  uintptr_t fva = tf->trapno == T_PGFLT ? rcr2() : 0; // before anything faults
  int sig, why, pferr = FNU_ERR_FAULT;

  if (curproc != 0 && (tf->cs & 3) == DPL_USER)
    curproc->tf = tf;

  if (tf->trapno == T_SYSCALL) {
    struct proc *p = curproc;

    sti();
    if (p->killed) {
      exit();
    }
    // FNU_PT_SYSCALL
    if (p->p_p->traced && p->p_p->pt_syscall) {
      p->p_p->pt_report = FNU_PTRACE_SCE;
      p->p_p->pt_other = 0;
      sig_post(p, FNU_SIG_BREAK, 0);
      sig_deliver(tf);
      if (p->killed)
        exit();
      if (p->p_p->pt_emu)
        p->p_p->pt_emu = 0;
      else
        syscall();
      if (p->p_p->traced && p->p_p->pt_syscall) {
        p->p_p->pt_report = FNU_PTRACE_SCX;
        sig_post(p, FNU_SIG_BREAK, 0);
      }
    } else {
      syscall();
    }
    if (curproc->killed) {
      exit();
    }
    sched_userret(p);
    if (sched_wanted())
      yield(0);
    sig_deliver(tf);
    return;
  }

  // a debug register fired
  if (tf->trapno == T_DEBUG && curproc != 0 && (tf->cs & 3) == DPL_USER) {
    uint64_t dr6 = dbregs_status();
    if (curproc->p_p->traced && (dr6 & 0xf) != 0) {
      curproc->dbregs[6] = dr6;
      curproc->p_p->pt_report = FNU_PTRACE_HWBP;
      curproc->p_p->pt_other = 0;
    }
  }

  // a fault may sleep on the disk; interrupts as they were
  if (tf->trapno == T_PGFLT && curproc != 0 && fva < USERTOP &&
      ((tf->cs & 3) == DPL_USER || curcpu()->spinlock_count == 0) &&
      (tf->err & (PGFLT_PRESENT | PGFLT_FETCH)) !=
          (PGFLT_PRESENT | PGFLT_FETCH)) {
    if (tf->rflags & FL_IF)
      sti();
    if ((pferr = vm_fault(curproc, fva, (tf->err & PGFLT_WRITE) != 0)) == 0)
      return;
  }
  if (tf->trapno == T_PGFLT && (tf->cs & 3) == 0 && curproc != 0 &&
      curproc->pcb_onfault != 0) {
    tf->rip = curproc->pcb_onfault;
    curproc->pcb_onfault = 0;
    return;
  }

  switch (tf->trapno) {
  case T_APIC_TIMER:
    hardclock((tf->cs & 3) == DPL_USER);
    lapiceoi();
    break;

  case T_IPI_SIGNOTIFY:
    lapiceoi();
    break;

  case T_IPI_TLB:
    tlb_flush_ack();
    lapiceoi();
    break;

  case T_IPI_HALT:
    // going down
    lapiceoi();
    cli();
    system_halt_handoff();
    curcpu()->halted = 1;
    for (;;)
      hlt();

  case T_NMI:
    if (!lockup_nmi(tf))
      nmi_intr();
    break;

  case T_MCHK:
    printf("cpu%d: machine check at rip 0x%lx\n", cpuid(), tf->rip);
    if (mcheck_report("now") == 0)
      printf("MCE: no bank holds a record\n");
    panic("machine check");

  case T_DBLFLT:
    printf("cpu%d: double fault rip 0x%lx rsp 0x%lx cr2 0x%lx\n", cpuid(),
            tf->rip, tf->rsp, rcr2());
    if (curproc != 0 && rcr2() < (uintptr_t)curproc->kstack &&
        rcr2() >= (uintptr_t)curproc->kstack - PGSIZE)
      panic("kernel stack overflow, pid %d", curproc->p_p->pid);
    panic("double fault");

  default:
    if (intr_is_device_vector((int)tf->trapno)) {
      if (intr_dispatch((int)tf->trapno) == 0 && ++strays <= 8)
        printf("cpu%d: nobody claims vector %d\n", cpuid(), (int)tf->trapno);
      lapiceoi();
      break;
    }
    if (tf->trapno == T_APIC_SPURIOUS)
      break;
    if (tf->trapno == T_APIC_ERROR) {
      lapic_error();
      lapiceoi();
      break;
    }
    if (tf->trapno >= T_IRQ0 && tf->trapno < T_IRQ0 + 16 &&
        pic_spurious((int)(tf->trapno - T_IRQ0)))
      break;
    if (tf->trapno == 15 || tf->trapno >= 20) {
      if (++strays <= 8)
        printf("cpu%d: stray interrupt, vector %d\n", cpuid(),
                (int)tf->trapno);
      if (tf->trapno >= T_IRQ0)
        lapiceoi();
      break;
    }
    if ((tf->cs & 3) == 0 && tf->trapno == T_GPFLT) {
      uintptr_t onfault;

      spinlock_enter();
      if ((onfault = curcpu()->onfault) != 0)
        curcpu()->onfault = 0;
      spinlock_exit();
      if (onfault != 0) {
        tf->rip = onfault;
        break;
      }
    }
    if (curproc == NULL || (tf->cs & 3) == 0) {
      printf("unexpected trap %lu err 0x%lx from cpu %d rip 0x%lx rsp 0x%lx "
              "(cr2=0x%lx)\n",
              tf->trapno, tf->err, cpuid(), tf->rip, tf->rsp, fva);
      if (tf->rip == 0)
        printf("  called from 0x%lx\n", *(uint64_t *)tf->rsp);
      panic("trap");
    }
    sig = sig_for_trap(curproc, tf, fva, pferr, &why);
    trapsignal(curproc, sig, why, tf->trapno == T_PGFLT ? fva : tf->rip);
  }

  if (curproc && curproc->killed && (tf->cs & 3) == DPL_USER) {
    exit();
  }

  // never inside a spin lock
  if (curproc != 0 && (tf->cs & 3) == DPL_USER)
    sched_userret(curproc);
  if (curproc && curproc->state == RUNNING && sched_wanted() &&
      ((tf->cs & 3) == DPL_USER || (tf->rflags & FL_IF)))
    yield(0);

  if (curproc && curproc->killed && (tf->cs & 3) == DPL_USER) {
    exit();
  }

  sig_deliver(tf);
}
