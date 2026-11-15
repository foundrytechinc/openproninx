// SIGNALS

#include "inc/signal.h"
#include "proc.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "mutex.h"
#include "trap.h"
#include "x86.h"

extern char sigcode[], sigcoderet[], esigcode[];

#define OFFSET(t, m) __offsetof(t, m)

_Static_assert(sizeof(struct trapframe) == OFFSET(struct fnu_sigcontext, mask),
               "sigcontext must begin with a trapframe");
_Static_assert(OFFSET(struct fnu_sigcontext, rdi) == OFFSET(struct trapframe, rdi),
               "sigcontext register order");
_Static_assert(OFFSET(struct fnu_sigcontext, rip) == OFFSET(struct trapframe, rip),
               "sigcontext register order");
_Static_assert(OFFSET(struct fnu_sigcontext, rsp) == OFFSET(struct trapframe, rsp),
               "sigcontext register order");
_Static_assert(OFFSET(struct fnu_sigframe, sc) == 24, "sigtramp.S SIGF_SC");

#define RFL_TF 0x00000100
#define RFL_DF 0x00000400
#define RFL_USER 0x00254dd5

// default actions
#define SP_KILL 0x01
#define SP_IGNORE 0x02
#define SP_STOP 0x04
#define SP_CONT 0x08

static const char *const signame[FNU_SIG_COUNT] = {
    [FNU_SIG_HANGUP] = "hangup",     [FNU_SIG_INTERRUPT] = "interrupt",
    [FNU_SIG_QUIT] = "quit",         [FNU_SIG_TERMINATE] = "terminate",
    [FNU_SIG_KILL] = "kill",         [FNU_SIG_STOP] = "stop",
    [FNU_SIG_TTYSTOP] = "ttystop",   [FNU_SIG_CONTINUE] = "continue",
    [FNU_SIG_USER1] = "user1",       [FNU_SIG_USER2] = "user2",
    [FNU_SIG_ILLEGAL] = "illegal",   [FNU_SIG_ARITH] = "arith",
    [FNU_SIG_MEMORY] = "memory",     [FNU_SIG_BUS] = "bus",
    [FNU_SIG_BREAK] = "break",       [FNU_SIG_BADCALL] = "badcall",
    [FNU_SIG_ABORT] = "abort",       [FNU_SIG_CPULIMIT] = "cpulimit",
    [FNU_SIG_FILELIMIT] = "filelimit", [FNU_SIG_CHILD] = "child",
    [FNU_SIG_BROKENPIPE] = "brokenpipe", [FNU_SIG_ALARM] = "alarm",
    [FNU_SIG_VTALARM] = "vtalarm",   [FNU_SIG_PROFILE] = "profile",
    [FNU_SIG_TTYIN] = "ttyin",       [FNU_SIG_TTYOUT] = "ttyout",
    [FNU_SIG_WINDOW] = "window",     [FNU_SIG_INFO] = "info",
    [FNU_SIG_IO] = "io",             [FNU_SIG_URGENT] = "urgent",
};

static const uchar sigprop[FNU_SIG_COUNT] = {
    [FNU_SIG_HANGUP] = SP_KILL,      [FNU_SIG_INTERRUPT] = SP_KILL,
    [FNU_SIG_QUIT] = SP_KILL,        [FNU_SIG_TERMINATE] = SP_KILL,
    [FNU_SIG_KILL] = SP_KILL,        [FNU_SIG_STOP] = SP_STOP,
    [FNU_SIG_TTYSTOP] = SP_STOP,     [FNU_SIG_CONTINUE] = SP_IGNORE | SP_CONT,
    [FNU_SIG_USER1] = SP_KILL,       [FNU_SIG_USER2] = SP_KILL,
    [FNU_SIG_ILLEGAL] = SP_KILL,     [FNU_SIG_ARITH] = SP_KILL,
    [FNU_SIG_MEMORY] = SP_KILL,      [FNU_SIG_BUS] = SP_KILL,
    [FNU_SIG_BREAK] = SP_KILL,       [FNU_SIG_BADCALL] = SP_KILL,
    [FNU_SIG_ABORT] = SP_KILL,       [FNU_SIG_CPULIMIT] = SP_KILL,
    [FNU_SIG_FILELIMIT] = SP_KILL,   [FNU_SIG_CHILD] = SP_IGNORE,
    [FNU_SIG_BROKENPIPE] = SP_KILL,  [FNU_SIG_ALARM] = SP_KILL,
    [FNU_SIG_VTALARM] = SP_KILL,     [FNU_SIG_PROFILE] = SP_KILL,
    [FNU_SIG_TTYIN] = SP_STOP,       [FNU_SIG_TTYOUT] = SP_STOP,
    [FNU_SIG_WINDOW] = SP_IGNORE,    [FNU_SIG_INFO] = SP_IGNORE,
    [FNU_SIG_IO] = SP_IGNORE,        [FNU_SIG_URGENT] = SP_IGNORE,
};

int sig_valid(int sig) {
  return sig > 0 && sig < FNU_SIG_COUNT && signame[sig] != 0;
}

int sig_default_ignores(int sig) {
  return sig > 0 && sig < FNU_SIG_COUNT && (sigprop[sig] & SP_IGNORE) != 0;
}

int sig_stops(int sig) {
  return sig > 0 && sig < FNU_SIG_COUNT && (sigprop[sig] & SP_STOP) != 0;
}

int sig_continues(int sig) {
  return sig > 0 && sig < FNU_SIG_COUNT && (sigprop[sig] & SP_CONT) != 0;
}

void sigexec(struct proc *p) {
  int sig;

  for (sig = 1; sig < FNU_SIG_COUNT; sig++) {
    if (p->p_p->sig_act[sig].handler == (uint64_t)FNU_SIGACT_DEFAULT ||
        p->p_p->sig_act[sig].handler == (uint64_t)FNU_SIGACT_IGNORE)
      continue;
    p->p_p->sig_act[sig].handler = (uint64_t)FNU_SIGACT_DEFAULT;
    p->p_p->sig_act[sig].mask = 0;
    p->p_p->sig_act[sig].flags = 0;
    if (sigprop[sig] & SP_IGNORE)
      sig_forget(p, sig);
  }
  if (p->p_p->sig_act[FNU_SIG_CHILD].handler == (uint64_t)FNU_SIGACT_IGNORE)
    p->p_p->sig_act[FNU_SIG_CHILD].handler = (uint64_t)FNU_SIGACT_DEFAULT;
  p->sigstk.base = 0;
  p->sigstk.size = 0;
  p->sigstk.flags = FNU_SIGSTACK_OFF;
  p->p_p->sigonstack = 0;
  p->sig_trapsig = 0;
  random_bytes(&p->p_p->sigcookie, sizeof p->p_p->sigcookie);
}

int sigcode_map(pte_t *pgdir, uintptr_t va) {
  return map_sigcode(pgdir, va, sigcode, (uint)(esigcode - sigcode));
}

void sigcode_attach(struct proc *p, uintptr_t va) {
  p->p_p->sigcode = va;
  p->p_p->sigcoderet = va + (uintptr_t)(sigcoderet - sigcode);
}

// build the frame
static int sendsig(struct proc *p, struct trapframe *tf,
                   const struct fnu_sigact *act, int sig, const struct fnu_siginfo *si,
                   uint64_t returnmask) {
  struct fnu_sigframe sf;
  uintptr_t sp;

  memset(&sf, 0, sizeof sf);
  sf.si = *si;
  memmove(&sf.sc.rax, tf, sizeof(struct trapframe));
  sf.sc.mask = returnmask;
  sf.sc.sig = sig;

  if ((p->p_p->sigonstack & FNU_SIGBIT(sig)) != 0 &&
      (p->sigstk.flags & (FNU_SIGSTACK_OFF | FNU_SIGSTACK_INUSE)) == 0 &&
      p->sigstk.size >= sizeof sf + FNU_FPSTATE_SIZE + 32) {
    sp = p->sigstk.base + p->sigstk.size;
    p->sigstk.flags |= FNU_SIGSTACK_INUSE;
  } else {
    sp = tf->rsp - 128;
  }
  sp = (sp - FNU_FPSTATE_SIZE) & ~(uintptr_t)15;
  sf.sc.fpstate = sp;
  sp = (sp - sizeof sf) & ~(uintptr_t)15;
  if (uva_range_ok(p->p_p->pgdir, sp, sf.sc.fpstate + FNU_FPSTATE_SIZE - sp) < 0)
    return -1;
  sf.sc.cookie = (sp + OFFSET(struct fnu_sigframe, sc)) ^ p->p_p->sigcookie;
  if (copyout(p->p_p->pgdir, sp, &sf, sizeof sf) < 0)
    return -1;
  fpusave(&p->fpu);
  if (copyout(p->p_p->pgdir, sf.sc.fpstate, p->fpu.data, sizeof p->fpu.data) < 0)
    return -1;
  fpureset(&p->fpu);

  tf->rsp = sp;
  tf->rax = act->handler;
  tf->rdi = (uint64_t)sig;
  tf->rsi = sp + OFFSET(struct fnu_sigframe, si);
  tf->rdx = sp + OFFSET(struct fnu_sigframe, sc);
  tf->rip = p->p_p->sigcode;
  tf->rflags &= ~(uint64_t)(RFL_TF | RFL_DF);
  return 0;
}

int kern_sigreturn(uintptr_t scp) {
  struct proc *p = curproc;
  struct trapframe *tf = p->tf;
  struct fnu_sigcontext sc;

  if (p->p_p->sigcoderet == 0 || tf->rip != p->p_p->sigcoderet)
    exit_signal(FNU_SIG_ILLEGAL);
  if ((scp & 7) != 0 || uva_range_ok(p->p_p->pgdir, scp, sizeof sc) < 0)
    exit_signal(FNU_SIG_ILLEGAL);
  if (copyin(p->p_p->pgdir, &sc, scp, sizeof sc) < 0)
    exit_signal(FNU_SIG_ILLEGAL);
  if (sc.cookie != ((uint64_t)scp ^ p->p_p->sigcookie))
    exit_signal(FNU_SIG_ILLEGAL);
  {
    uint64_t spent = 0;

    copyout(p->p_p->pgdir, scp + OFFSET(struct fnu_sigcontext, cookie), &spent,
            sizeof(spent));
  }

  // iretq faults in the kernel on a bad cs, ss or rip
  if (sc.cs != ((SEG_UCODE << 3) | DPL_USER) || sc.ss != ((SEG_UDATA << 3) | DPL_USER) ||
      sc.rip >= USERTOP)
    exit_signal(FNU_SIG_ILLEGAL);

  if ((sc.fpstate & 15) != 0 ||
      uva_range_ok(p->p_p->pgdir, (uintptr_t)sc.fpstate, FNU_FPSTATE_SIZE) < 0 ||
      copyin(p->p_p->pgdir, p->fpu.data, (uintptr_t)sc.fpstate,
             sizeof p->fpu.data) < 0 ||
      (*(uint32_t *)(p->fpu.data + 24) & 0xffff0000u) != 0)
    exit_signal(FNU_SIG_ILLEGAL);
  fpurestore(&p->fpu);

  memmove(tf, &sc.rax, sizeof(struct trapframe));
  tf->rflags = (sc.rflags & RFL_USER) | FL_IF | 0x2;
  tf->trapno = T_SYSCALL;
  tf->err = 0;

  p->sig_mask = sc.mask & ~FNU_SIG_UNCATCHABLE;
  p->sigstk.flags &= ~FNU_SIGSTACK_INUSE;
  return CALL_JUSTRETURN;
}

// sigaltstack(2)
int kern_sigstack(const struct fnu_altstack *in, struct fnu_altstack *out) {
  struct proc *p = curproc;

  if (out != 0)
    *out = p->sigstk;
  if (in == 0)
    return 0;
  if (p->sigstk.flags & FNU_SIGSTACK_INUSE)
    return FNU_ERR_NOTPERMITTED;
  if (in->flags & FNU_SIGSTACK_OFF) {
    p->sigstk.flags = FNU_SIGSTACK_OFF;
    p->sigstk.base = 0;
    p->sigstk.size = 0;
    return 0;
  }
  if (in->size < FNU_SIGSTACK_MIN)
    return FNU_ERR_NOMEMORY;
  if (uva_range_ok(p->p_p->pgdir, (uintptr_t)in->base, (size_t)in->size) < 0)
    return FNU_ERR_FAULT;
  p->sigstk = *in;
  p->sigstk.flags &= ~FNU_SIGSTACK_INUSE;
  return 0;
}

// sigact(2)
int kern_sigact(int sig, const struct fnu_sigact *act, struct fnu_sigact *old) {
  struct proc *p = curproc;

  if (!sig_valid(sig))
    return FNU_ERR_INVALID;
  if (old)
    *old = p->p_p->sig_act[sig];
  if (act == 0)
    return 0;
  if (FNU_SIGBIT(sig) & FNU_SIG_UNCATCHABLE)
    return FNU_ERR_INVALID;

  p->p_p->sig_act[sig] = *act;
  p->p_p->sig_act[sig].mask &= ~FNU_SIG_UNCATCHABLE;
  if (act->flags & FNU_SIGACT_ALTSTACK)
    p->p_p->sigonstack |= FNU_SIGBIT(sig);
  else
    p->p_p->sigonstack &= ~FNU_SIGBIT(sig);
  if (act->handler == (uint64_t)FNU_SIGACT_IGNORE ||
      (act->handler == (uint64_t)FNU_SIGACT_DEFAULT && sig_default_ignores(sig)))
    sig_forget(p, sig);
  return 0;
}

uint64_t kern_sigmask(uint64_t block, uint64_t unblock) {
  struct proc *p = curproc;
  uint64_t old = p->sig_mask;

  p->sig_mask = ((p->sig_mask | block) & ~unblock) & ~FNU_SIG_UNCATCHABLE;
  return old;
}

static int issignal(struct proc *p);

int kern_sigsuspend(uint64_t mask) {
  struct proc *p = curproc;

  p->sig_oldmask = p->sig_mask;
  p->sig_suspended = 1;
  p->sig_mask = mask & ~FNU_SIG_UNCATCHABLE;
  while (!p->killed && issignal(p) == 0)
    tsleep(&p->sig_oldmask, PPAUSE | PCATCH, "pause", 0);
  p->sig_restart = 0;
  return FNU_ERR_INTERRUPTED;
}

static int issignal(struct proc *p) {
  uint64_t ready;
  int sig, prop;

  for (;;) {
    if ((p->p_p->ps_flags & PS_HELD) && !p->killed)
      proc_suspend();
    ready = sig_pending_of(p) & ~p->sig_mask;
    if (ready == 0)
      return 0;
    for (sig = 1; sig < FNU_SIG_COUNT; sig++)
      if (ready & FNU_SIGBIT(sig))
        break;

    // traced: the tracer chooses
    if (p->p_p->traced && sig != FNU_SIG_KILL) {
      sig_forget(p, sig);
      sig = proc_trace_stop(sig);
      if (!p->p_p->traced || !sig_valid(sig))
        continue;
      sig_post(p, sig, 0);
      if (p->sig_mask & FNU_SIGBIT(sig))
        continue;
    }
    prop = sigprop[sig];

    if (p->p_p->sig_act[sig].handler == (uint64_t)FNU_SIGACT_IGNORE) {
      sig_forget(p, sig);
      continue;
    }
    if (p->p_p->sig_act[sig].handler != (uint64_t)FNU_SIGACT_DEFAULT)
      return sig;

    // init takes no default action
    if (p->p_p->pid == 1) {
      sig_forget(p, sig);
      continue;
    }
    if (prop & SP_IGNORE) {
      sig_forget(p, sig);
      continue;
    }
    if (prop & SP_STOP) {
      sig_forget(p, sig);
      proc_stop(sig);
      continue;
    }
    return sig;
  }
}

static void postsig(struct proc *p, int sig, struct trapframe *tf) {
  struct fnu_sigact act = p->p_p->sig_act[sig];
  uint64_t returnmask = p->sig_mask;
  struct fnu_siginfo si;

  if (p->sig_suspended) {
    returnmask = p->sig_oldmask;
    p->sig_suspended = 0;
  }

  memset(&si, 0, sizeof si);
  si.sig = sig;
  si.sender = sig_forget(p, sig);
  si.from = si.sender ? FNU_SIGFROM_PROCESS : FNU_SIGFROM_SYSTEM;
  if (p->sig_trapsig == sig) {
    si.from = FNU_SIGFROM_FAULT;
    si.why = p->sig_trapwhy;
    si.addr = p->sig_trapaddr;
    p->sig_trapsig = 0;
  }

  if (act.handler == (uint64_t)FNU_SIGACT_DEFAULT) {
    if (si.from == FNU_SIGFROM_FAULT)
      printf("pid %d %s: %s at 0x%lx, pc 0x%lx\n", p->p_p->pid, p->p_p->name,
              signame[sig], si.addr, tf->rip);
    p->killed = 1;
    exit_signal(sig);
  }
  if (sendsig(p, tf, &act, sig, &si, returnmask) < 0) {
    // unwritable stack: kill it
    p->killed = 1;
    exit_signal(FNU_SIG_ILLEGAL);
  }
  if (act.flags & FNU_SIGACT_ONCE)
    p->p_p->sig_act[sig].handler = (uint64_t)FNU_SIGACT_DEFAULT;
  p->p_p->nsignals++;
  p->sig_mask |= act.mask;
  if (!(act.flags & FNU_SIGACT_REENTER))
    p->sig_mask |= FNU_SIGBIT(sig);
  p->sig_mask &= ~FNU_SIG_UNCATCHABLE;
}

// CALL_RESTART
static void sig_restart_call(struct proc *p, struct trapframe *tf, int sig) {
  if (!p->sig_restart || tf->trapno != T_SYSCALL || !(tf->rflags & FL_CF) ||
      tf->rax != FNU_ERR_INTERRUPTED)
    return;
  if (sig != 0 && !(p->p_p->sig_act[sig].flags & FNU_SIGACT_RESTART))
    return;
  tf->rip -= 2;
  tf->rax = p->sig_callno;
}

void sig_deliver(struct trapframe *tf) {
  struct proc *p = curproc;
  int sig, acted = 0;

  if (p == 0 || (tf->cs & 3) != DPL_USER)
    return;
  while ((sig = issignal(p)) != 0) {
    if (!acted) {
      sig_restart_call(p, tf, sig);
      acted = 1;
    }
    postsig(p, sig, tf);
  }
  if (!acted)
    sig_restart_call(p, tf, 0);
  p->sig_restart = 0;
  if (p->sig_suspended) {
    p->sig_mask = p->sig_oldmask;
    p->sig_suspended = 0;
  }
}

void trapsignal(struct proc *p, int sig, int32_t why, uint64_t addr) {
  static uint initfaults;

  p->sig_trapsig = sig;
  p->sig_trapwhy = why;
  p->sig_trapaddr = addr;

  if ((sigprop[sig] & SP_KILL) && p->p_p->pid != 1 &&
      ((p->sig_mask & FNU_SIGBIT(sig)) != 0 ||
       p->p_p->sig_act[sig].handler == (uint64_t)FNU_SIGACT_IGNORE)) {
    printf("pid %d %s: %s at 0x%lx, pc 0x%lx\n", p->p_p->pid, p->p_p->name,
            signame[sig], addr, p->tf->rip);
    p->killed = 1;
    exit_signal(sig);
  }
  if (p->p_p->pid == 1 && ++initfaults <= 4)
    printf("init: %s at 0x%lx\n", signame[sig], addr);
  sig_post(p, sig, 0);
}

static int fpu_why(int simd) {
  static const int why[] = {FNU_FAULT_FLTINV, FNU_FAULT_FLTRES, FNU_FAULT_FLTDIV,
                            FNU_FAULT_FLTOVF, FNU_FAULT_FLTUND, FNU_FAULT_FLTRES,
                            FNU_FAULT_FLTINV};
  uint32_t status;
  uint16_t sw;
  int i;

  if (simd) {
    __asm __volatile("stmxcsr %0" : "=m"(status));
  } else {
    __asm __volatile("fnstsw %0" : "=m"(sw));
    status = sw;
  }
  for (i = 0; i < (int)(sizeof(why) / sizeof(why[0])); i++)
    if (status & (1u << i))
      return why[i];
  return FNU_FAULT_FLTINV;
}

// pferr is what vm_fault said
int sig_for_trap(struct proc *p, const struct trapframe *tf, uintptr_t fva,
                 int pferr, int *why) {
  switch (tf->trapno) {
  case T_DIVIDE: *why = FNU_FAULT_INTDIV; return FNU_SIG_ARITH;
  case T_OFLOW: *why = FNU_FAULT_INTOVF; return FNU_SIG_ARITH;
  case T_BOUND: *why = FNU_FAULT_FLTSUB; return FNU_SIG_ARITH;
  case T_FPERR: *why = fpu_why(0); return FNU_SIG_ARITH;
  case T_SIMDERR: *why = fpu_why(1); return FNU_SIG_ARITH;
  case T_DEBUG:
  case T_BRKPT: *why = FNU_FAULT_BREAK; return FNU_SIG_BREAK;
  case T_ILLOP: *why = FNU_FAULT_PRIVOP; return FNU_SIG_ILLEGAL;
  case T_DEVNOTAVAIL: *why = FNU_FAULT_COPROC; return FNU_SIG_ILLEGAL;
  case T_ALIGN: *why = FNU_FAULT_ALIGN; return FNU_SIG_BUS;
  case T_TSS: *why = FNU_FAULT_OBJECT; return FNU_SIG_BUS;
  case T_PGFLT:
    if (pferr == FNU_ERR_IO) {
      *why = FNU_FAULT_OBJECT;
      return FNU_SIG_BUS;
    }
    if (pferr == FNU_ERR_NOMEMORY) {
      printf("uvm: pid %d (%s), uid %u killed: out of swap\n", p->p_p->pid,
             p->p_p->name, p->p_p->uid);
      *why = 0;
      return FNU_SIG_KILL;
    }
    *why = pferr == FNU_ERR_DENIED || vm_holds(p, fva) ? FNU_FAULT_ACCESS
                                                       : FNU_FAULT_UNMAPPED;
    return FNU_SIG_MEMORY;
  default: *why = FNU_FAULT_UNMAPPED; return FNU_SIG_MEMORY;
  }
}
