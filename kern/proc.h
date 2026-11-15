#ifndef PRONINX_X86_64_PROC_H
#define PRONINX_X86_64_PROC_H

#include "inc/abi.h"
#include "inc/signal.h"
#include "callout.h"
#include "fpu.h"
#include "mmu.h"
#include "uvm.h"
#include "param.h"
#include "mutex.h"
#include "sleeplock.h"
#include "x86.h"

enum procstate {
  UNUSED = FNU_PS_UNUSED,
  EMBRYO = FNU_PS_EMBRYO,
  SLEEPING = FNU_PS_SLEEPING,
  RUNNABLE = FNU_PS_RUNNABLE,
  RUNNING = FNU_PS_RUNNING,
  ZOMBIE = FNU_PS_ZOMBIE,
  STOPPED = FNU_PS_STOPPED,
  DEAD = FNU_PS_DEAD
};

struct cpu_info {
  struct cpu_info *self;
  uint32_t apicid;
  int ci_acpi_proc_id;
  struct context *scheduler;
  struct taskstate ts;
  struct segdesc gdt[NSEGS];
  volatile uint started;
  int spinlock_count;
  int dbloaded;
  int spinlock_intr;
  struct proc *proc;
  int upri; // NULLPRI for none
  volatile int resched;
  volatile int slicedown;
  int notifying;
  uint64_t notify[MAXCPU / 64]; // sent when the last spin lock goes
  volatile int idle;
  uintptr_t onfault;
  uint64_t ticks_busy, ticks_idle;
  uint64_t pcputicks;
  char *apstack;
  char *ist;
  volatile int park;
  volatile int parked;
  volatile int halted;
};

extern struct cpu_info cpus[MAXCPU];
extern int ncpus;

struct context {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t rdi;
  uint64_t rsi;
  uint64_t rbx;
  uint64_t rbp;
  uint64_t rip;
};

#define PX_WAITED 0x1
#define PX_CONTINUED 0x2

#define PS_STOPPED 0x1
#define PS_ZOMBIE 0x2
#define PS_EXEC 0x4 // exec'd since fork
#define PS_FROZEN 0x8 // held for a system suspend
#define PS_HELD (PS_STOPPED | PS_FROZEN)

#define PIO_NOREUSE 0x1
#define PIO_SYNC 0x2

#define PID_MAX 99999
#define THREAD_PID_OFFSET 100000 // tids are not pids
#define NFSTX 8

struct process;

struct session {
  struct session *s_hash;
  int s_count;
  struct process *s_leader;
  int s_tty; // -1 for none
  pid_t s_sid;
};

struct pgrp {
  struct pgrp *pg_hash;
  struct process *pg_members;
  struct session *pg_session;
  pid_t pg_id;
  int pg_jobc;
};

struct uidinfo {
  struct uidinfo *ui_hash;
  uint ui_uid;
  int ui_ref;
  int ui_proccnt;
};

// THE PROCESS
struct process {
  struct process *ps_next;
  struct process **ps_prev;
  struct process *ps_hash;
  struct process *ps_children;
  struct process *ps_sibling;
  struct process **ps_psibling;
  struct process *ps_orphans;
  struct process *ps_orphan;
  struct process **ps_porphan;
  struct pgrp *ps_pgrp;
  struct process *ps_pgnext;
  struct process **ps_ppgnext;
  struct uidinfo *ps_ruidinfo;
  struct callout ps_realit;
  uint64_t ps_realat; // 0 when off
  struct proc *ps_threads;
  int ps_nthreads;
  int ps_flags;
  struct proc *ps_single;
  uint64_t ps_siglist;
  pid_t ps_sender[FNU_SIG_COUNT];
  struct sleeplock vmlock;
  size_t sz;
  pte_t *pgdir;
  struct vm_entry *vm;
  uint64_t vmsize;
  int swaplock;
  int swapfrozen;
  pid_t pid;
  pid_t pgid;
  pid_t sid;
  char login[MAXLOGNAME];
  struct process *parent;
  struct fnu_limit rlim[FNU_LIMIT_COUNT];
  struct fnu_usage cru;
  size_t maxrss; // KB
  uint64_t rss;
  int64_t uticks;
  int64_t sticks;
  int64_t nsignals;
  int64_t nvcsw;
  int64_t nivcsw;
  int nice;
  ushort umask;
  int64_t itimer_value[FNU_TIMER_COUNT];
  int64_t itimer_interval[FNU_TIMER_COUNT];
  int32_t xexit;
  int32_t xsig;
  int xflags;
  struct fnu_sigact sig_act[FNU_SIG_COUNT];
  uint64_t sigonstack;
  uintptr_t sigcode;
  uintptr_t sigcoderet;
  uint64_t sigcookie;
  uint uid; // effective
  uint gid;
  uint ruid;
  uint rgid;
  uint svuid;
  uint svgid;
  uint groups[NGROUPS];
  int ngroups;
  int sugid;
  int pledged;
  uint64_t pledge;
  uint64_t execpledge;
  struct knote *klist;
  struct knote *sigklist;
  struct unveil *uv;
  int uvcount;
  int uvnames;
  int uvdone;
  struct sleeplock uvlock;
  struct mutex fdlock;
  struct file **ofile;
  uchar *ofileflags;
  int nofile;
  struct file *ofile0[NDFILE];
  uchar ofileflags0[NDFILE];
  struct inode *cwd;
  int traced;
  int trapped;
  struct process *oparent;
  uint32_t pt_events;
  int pt_pinned;
  int pt_wake;
  struct proc *pt_thread;
  int32_t pt_report;
  pid_t pt_other;
  int pt_syscall;
  int pt_emu;
  char name[16];
  int64_t start_ns;
  char args[FNU_PROCARGS_MAX];
};

// THE THREAD
struct proc {
  struct process *p_p;
  uintptr_t pcb_onfault;
  struct proc *p_thrnext;
  struct proc *p_hash;
  struct proc *slpnext;
  struct proc **slpprev;
  struct callout sleeptimer;
  pid_t tid;
  char *kstack;
  struct mutex lock; // held across the switch out
  enum procstate state;
  trapframe_t *tf;
  struct context *context;
  struct fxsave fpu;
  int nofault;
  int ioflag;
  uchar fstx[NFSTX];
  void *chan;
  const char *wmesg;
  int timedout;
  int slpcatch;
  int waiting;
  int woken;
  int sigcheck;
  uint64_t tcb;

  // scheduler
  uint8_t priority;
  uint8_t usrpri;
  uint8_t slppri;
  uint8_t runpri;
  uint8_t lentpri;
  int nsleeplocks;
  void *lockwait;
  uint32_t estcpu;
  uint32_t cpticks;
  uint32_t slptime;
  uint32_t pctcpu;
  uint64_t cpbase;
  struct proc *rqnext;
  struct proc *rqprev;
  int onrunq;
  int qcpu;
  int wakefrom;
  int rrcount;
  int uload;
  uint8_t inload;
  uint8_t forked;
  int killed;
  uint64_t sig_pending;
  uint64_t sig_mask;
  uint64_t sig_oldmask;
  int sig_suspended;
  int sig_restart;
  uint64_t sig_callno;
  pid_t sig_sender[FNU_SIG_COUNT];
  int sig_trapsig;
  int32_t sig_trapwhy;
  uint64_t sig_trapaddr;
  struct fnu_altstack sigstk;
  uint64_t dbregs[8];
  int dbactive;
  void (*kfn)(void *);
  void *kfnarg;
  struct cpu_info *pinned;
  struct proc *selnext; // on the select queue
  int selscan;
  uint selgen;
};

#endif 
