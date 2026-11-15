// PTRACE

#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"
#include "uvm.h"
#include "mutex.h"
#include "x86.h"
#include "inc/abi.h"
#include "inc/signal.h"

#define RFL_USER 0x00254dd5


#define PT_IO_MAX (64 * 1024)

static void regs_of(const struct trapframe *tf, struct fnu_regs *out) {
  out->rax = tf->rax, out->rbx = tf->rbx, out->rcx = tf->rcx;
  out->rdx = tf->rdx, out->rbp = tf->rbp, out->rsi = tf->rsi;
  out->rdi = tf->rdi, out->r8 = tf->r8, out->r9 = tf->r9;
  out->r10 = tf->r10, out->r11 = tf->r11, out->r12 = tf->r12;
  out->r13 = tf->r13, out->r14 = tf->r14, out->r15 = tf->r15;
  out->rip = tf->rip, out->rflags = tf->rflags, out->rsp = tf->rsp;
  out->cs = tf->cs, out->ss = tf->ss;
}

static int regs_to(struct trapframe *tf, const struct fnu_regs *in) {
  if (in->rip >= USERTOP)
    return -1;
  tf->rax = in->rax, tf->rbx = in->rbx, tf->rcx = in->rcx;
  tf->rdx = in->rdx, tf->rbp = in->rbp, tf->rsi = in->rsi;
  tf->rdi = in->rdi, tf->r8 = in->r8, tf->r9 = in->r9;
  tf->r10 = in->r10, tf->r11 = in->r11, tf->r12 = in->r12;
  tf->r13 = in->r13, tf->r14 = in->r14, tf->r15 = in->r15;
  tf->rip = in->rip;
  tf->rsp = in->rsp;
  tf->rflags = (tf->rflags & ~(uint64_t)RFL_USER) |
               (in->rflags & (uint64_t)RFL_USER) | FL_IF;
  return 0;
}

// who may trace what
static int may_trace(struct proc *tracer, struct proc *t) {
  if (t->p_p->pid == 1 || t == tracer)
    return 0;
  if (tracer->p_p->uid != 0 && (tracer->p_p->uid != t->p_p->uid || tracer->p_p->uid != t->p_p->ruid))
    return 0;
  return 1;
}

static int is_stopped(struct proc *tracer, struct proc *t) {
  return t->p_p->traced && t->p_p->parent == tracer->p_p && t->state == STOPPED &&
         t->p_p->trapped;
}

int kern_ptrace(int request, pid_t pid, uintptr_t addr, int64_t data,
                register_t *retval) {
  struct proc *me = curproc;
  struct proc *t;
  struct fnu_regs regs;
  struct fnu_ptrace_io io;
  char iobuf[sizeof(t->fpu.data)];
  uint64_t word;
  int error = 0;
  uint n;

  if (request == FNU_PT_TRACE_ME) {
    if (me->p_p->traced || me->p_p->pid == 1)
      return FNU_ERR_BUSY;
    me->p_p->traced = 1;
    return 0;
  }

  if ((t = proc_hold(pid)) == 0)
    return FNU_ERR_NOPROCESS;

  if (request == FNU_PT_ATTACH) {
    if (t->p_p->traced || !may_trace(me, t)) {
      error = t->p_p->traced ? FNU_ERR_BUSY : FNU_ERR_NOTPERMITTED;
      proc_release();
      return error;
    }
    t->p_p->traced = 1;
    proc_trace_attach(t, me->p_p);
    sig_post_process(t->p_p, FNU_SIG_STOP, me->p_p->pid);
    proc_release();
    return 0;
  }

  if (!is_stopped(me, t)) {
    proc_release();
    return FNU_ERR_BUSY;
  }
  proc_pin(t);
  proc_release();

  switch (request) {
  case FNU_PT_READ_I:
  case FNU_PT_READ_D:
    if (copyin(t->p_p->pgdir, &word, addr, sizeof(word)) < 0)
      error = FNU_ERR_FAULT;
    else
      *retval = (register_t)word;
    break;

  case FNU_PT_WRITE_I:
  case FNU_PT_WRITE_D:
    word = (uint64_t)data;
    if (vm_write_force(t, addr, &word, sizeof(word)) < 0)
      error = FNU_ERR_FAULT;
    break;

  case FNU_PT_IO:
    if (copyin(me->p_p->pgdir, &io, addr, sizeof(io)) < 0) {
      error = FNU_ERR_FAULT;
      break;
    }
    if (io.len > PT_IO_MAX)
      io.len = PT_IO_MAX;
    for (n = 0; n < io.len; n += (uint)sizeof(iobuf)) {
      uint part = io.len - n;

      if (part > sizeof(iobuf))
        part = sizeof(iobuf);
      if (io.op == FNU_PT_IO_READ) {
        if (copyin(t->p_p->pgdir, iobuf, (uintptr_t)(io.addr + n), part) < 0 ||
            copyout(me->p_p->pgdir, (uintptr_t)(io.buf + n), iobuf, part) < 0)
          break;
      } else if (io.op == FNU_PT_IO_WRITE) {
        if (copyin(me->p_p->pgdir, iobuf, (uintptr_t)(io.buf + n), part) < 0 ||
            vm_write_force(t, (uintptr_t)(io.addr + n), iobuf, part) < 0)
          break;
      } else {
        break;
      }
    }
    if (n == 0 && io.len != 0)
      error = io.op == FNU_PT_IO_READ || io.op == FNU_PT_IO_WRITE ? FNU_ERR_FAULT : FNU_ERR_INVALID;
    else
      *retval = n >= io.len ? (register_t)io.len : (register_t)n;
    break;

  case FNU_PT_GETREGS:
    regs_of(t->tf, &regs);
    if (copyout(me->p_p->pgdir, addr, &regs, sizeof(regs)) < 0)
      error = FNU_ERR_FAULT;
    break;

  case FNU_PT_SETREGS:
    if (copyin(me->p_p->pgdir, &regs, addr, sizeof(regs)) < 0)
      error = FNU_ERR_FAULT;
    else if (regs_to(t->tf, &regs) < 0)
      error = FNU_ERR_INVALID;
    break;

  case FNU_PT_GETFPREGS:
    if (copyout(me->p_p->pgdir, addr, t->fpu.data, sizeof(t->fpu.data)) < 0)
      error = FNU_ERR_FAULT;
    break;

  case FNU_PT_SETFPREGS:
    if (copyin(me->p_p->pgdir, iobuf, addr, sizeof(t->fpu.data)) < 0)
      error = FNU_ERR_FAULT;
    else if ((*(uint32_t *)(iobuf + 24) & 0xffff0000u) != 0)
      error = FNU_ERR_INVALID;
    else
      memmove(t->fpu.data, iobuf, sizeof(t->fpu.data));
    break;

  case FNU_PT_SET_EVENT_MASK: {
    struct fnu_ptrace_event ev;
    if (copyin(me->p_p->pgdir, &ev, addr, sizeof(ev)) < 0)
      error = FNU_ERR_FAULT;
    else if ((ev.pe_set_event & ~FNU_PTRACE_FORK) != 0)
      error = FNU_ERR_INVALID;
    else
      t->p_p->pt_events = (uint32_t)ev.pe_set_event;
    break;
  }

  case FNU_PT_GET_EVENT_MASK: {
    struct fnu_ptrace_event ev;
    ev.pe_set_event = (int32_t)t->p_p->pt_events;
    if (copyout(me->p_p->pgdir, addr, &ev, sizeof(ev)) < 0)
      error = FNU_ERR_FAULT;
    break;
  }

  case FNU_PT_GET_PROCESS_STATE: {
    struct fnu_ptrace_state st;
    st.pe_report_event = t->p_p->pt_report;
    st.pe_other_pid = t->p_p->pt_other;
    if (copyout(me->p_p->pgdir, addr, &st, sizeof(st)) < 0)
      error = FNU_ERR_FAULT;
    break;
  }

  case FNU_PT_SYSCALLEMU:
    // only at an entry stop
    if (t->p_p->pt_report != FNU_PTRACE_SCE)
      error = FNU_ERR_INVALID;
    else
      t->p_p->pt_emu = 1;
    break;

  case FNU_PT_GETDBREGS: {
    struct fnu_dbregs db;
    memmove(db.dr, t->dbregs, sizeof(db.dr));
    if (copyout(me->p_p->pgdir, addr, &db, sizeof(db)) < 0)
      error = FNU_ERR_FAULT;
    break;
  }

  case FNU_PT_SETDBREGS: {
    struct fnu_dbregs db;
    int i;
    if (copyin(me->p_p->pgdir, &db, addr, sizeof(db)) < 0) {
      error = FNU_ERR_FAULT;
      break;
    }
    for (i = 0; i < 4; i++)
      if (db.dr[i] >= USERTOP) {
        error = FNU_ERR_INVALID;
        break;
      }
    if (error != 0)
      break;
    for (i = 0; i < 4; i++)
      t->dbregs[i] = db.dr[i];
    t->dbregs[6] = 0;
    t->dbregs[7] = db.dr[7] & 0xffff0055ull;
    t->dbactive = (t->dbregs[7] & 0x55) != 0;
    break;
  }

  case FNU_PT_SYSCALL:
  case FNU_PT_STEP:
  case FNU_PT_CONTINUE:
    if (data != 0 && !sig_valid((int)data)) {
      error = FNU_ERR_INVALID;
      break;
    }
    if (addr != FNU_PT_CONTINUE_HERE) {
      if (uva_range_ok(t->p_p->pgdir, addr, 1) < 0) {
        error = FNU_ERR_FAULT;
        break;
      }
      t->tf->rip = addr;
    }
    if (request == FNU_PT_STEP)
      t->tf->rflags |= FL_TF;
    else
      t->tf->rflags &= ~FL_TF;
    t->p_p->pt_syscall = request == FNU_PT_SYSCALL;
    if (t->p_p->pt_report != FNU_PTRACE_SCE)
      t->p_p->pt_emu = 0;
    t->p_p->pt_report = 0;
    t->p_p->pt_other = 0;
    t->p_p->xsig = (int32_t)data;
    proc_lock();
    proc_resume(t);
    proc_unlock();
    break;

  case FNU_PT_DETACH:
    if (data != 0 && !sig_valid((int)data)) {
      error = FNU_ERR_INVALID;
      break;
    }
    proc_lock();
    t->p_p->traced = 0;
    t->tf->rflags &= ~FL_TF;
    t->p_p->pt_syscall = 0;
    t->p_p->pt_events = 0;
    t->dbactive = 0;
    t->p_p->xsig = (int32_t)data;
    proc_trace_detach(t);
    proc_resume(t);
    proc_unlock();
    break;

  case FNU_PT_KILL:
    proc_lock();
    t->p_p->traced = 0;
    t->p_p->xsig = FNU_SIG_KILL;
    t->killed = 1;
    proc_resume(t);
    proc_unlock();
    break;

  default:
    error = FNU_ERR_INVALID;
    break;
  }

  proc_unpin(t);
  return error;
}

int sys_ptrace(struct proc *p __unused, const register_t *uap, register_t *retval) {
  int request;
  int pid;
  uint64_t addr;
  uint64_t data;

  request = (int)uap[0];
  pid = (int)uap[1];
  addr = uap[2];
  data = uap[3];
  return kern_ptrace(request, (pid_t)pid, (uintptr_t)addr, (int64_t)data, retval);
}
