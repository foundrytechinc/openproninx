#include "inc/signal.h"
#include "proc.h"
#include "sched.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"
#include "callout.h"
#include "mutex.h"
#include "sleeplock.h"
#include "x86.h"
#include "inc/abi.h"
#include "malloc.h"
#include "unveil.h"

struct mutex proctree_lock;

_Static_assert(HZ == FNU_HZ, "HZ and FNU_HZ disagree");

#define PROCHASH 256
#define PROCH(id) ((uint)(id) & (PROCHASH - 1))
#define UIHASH 64

static struct process *pidhash[PROCHASH];
static struct pgrp *pgrphash[PROCHASH];
static struct session *sesshash[PROCHASH];
static struct proc *tidhash[PROCHASH];
static struct uidinfo *uihash[UIHASH];

static int nprocs, nthreads;

static int thread_room(uint euid) {
  return nthreads < maxthread - 100 || (euid == 0 && nthreads < maxthread);
}

// THE SLEEP QUEUES
#define SLPQUE 128
#define SLPQ(chan) (((uintptr_t)(chan) >> 8) & (SLPQUE - 1))

static struct {
  struct mutex lock;
  struct proc *head;
  struct proc **tail;
} slpque[SLPQUE];

// the chain's lock held
static void slpq_add(struct proc *p) {
  int b = (int)SLPQ(p->chan);

  if (slpque[b].tail == 0)
    slpque[b].tail = &slpque[b].head;
  p->slpnext = 0;
  p->slpprev = slpque[b].tail;
  *slpque[b].tail = p;
  slpque[b].tail = &p->slpnext;
}

static void slpq_del(struct proc *p) {
  int b = (int)SLPQ(p->chan);

  if (p->slpprev == 0)
    return;
  if (p->slpnext != 0)
    p->slpnext->slpprev = p->slpprev;
  else
    slpque[b].tail = p->slpprev;
  *p->slpprev = p->slpnext;
  p->slpnext = 0;
  p->slpprev = 0;
}

// THE PROCESSES
static struct process *allprocess;
static pid_t nexttid;

#define FOREACH_PROCESS(pr) for ((pr) = allprocess; (pr) != 0; (pr) = (pr)->ps_next)
#define FOREACH_THREAD(pr, t) for ((t) = (pr)->ps_threads; (t) != 0; (t) = (t)->p_thrnext)
#define FOREACH_CHILD(pr, c)                                                   \
  for ((c) = (pr)->ps_children; (c) != 0; (c) = (c)->ps_sibling)
#define FOREACH_MEMBER(pg, pr)                                                 \
  for ((pr) = (pg)->pg_members; (pr) != 0; (pr) = (pr)->ps_pgnext)

struct proc *initprocess;
static int parked_chan;

int nextpid = 1;
extern void fork_trampoline(void);
static void thread_start(struct proc *p);
extern void trapret(void);

void pinit(void) {
  int i;

  mtx_init(&proctree_lock, "proctree");
  for (i = 0; i < SLPQUE; i++)
    mtx_init(&slpque[i].lock, "slpque");
}

static void schedcpu(int second) {
  struct process *pr;
  struct proc *p;

  mtx_enter(&proctree_lock);
  FOREACH_PROCESS(pr)
  FOREACH_THREAD(pr, p) {
    mtx_enter(&p->lock);
    sched_decay(p);
    if (second)
      sched_second(p);
    mtx_leave(&p->lock);
  }
  mtx_leave(&proctree_lock);
}

static void setrunnable(struct proc *p);
static void post(struct process *pr, struct proc *t, int sig, pid_t sender);

// sleep_core waits for it
static void endtsleep(void *arg) {
  struct proc *p = arg;

  mtx_enter(&p->lock);
  if (p->state == SLEEPING) {
    p->timedout = 1;
    setrunnable(p);
  } else if (p->waiting)
    p->timedout = 1;
  mtx_leave(&p->lock);
}

static void realitexpire(void *arg);

static void realit_arm(struct process *pr) {
  uint64_t left = pr->ps_realat > ticks ? pr->ps_realat - ticks : 1;

  callout_reset(&pr->ps_realit, left > 0x7fffffff ? 0x7fffffff : (int)left,
                realitexpire, pr);
}

// SIGALRM, and the interval
static void realitexpire(void *arg) {
  struct process *pr = arg;

  mtx_enter(&proctree_lock);
  if (pr->ps_realat != 0 && !(pr->ps_flags & PS_ZOMBIE)) {
    if (ticks < pr->ps_realat) {
      realit_arm(pr);
    } else {
      post(pr, 0, FNU_SIG_ALARM, 0);
      if (pr->itimer_interval[FNU_TIMER_REAL] != 0) {
        pr->ps_realat = ticks + (uint64_t)pr->itimer_interval[FNU_TIMER_REAL];
        realit_arm(pr);
      } else {
        pr->ps_realat = 0;
      }
    }
  }
  mtx_leave(&proctree_lock);
}

// the cpu that moved ticks over owns that work
static void sched_tick(uint64_t from, uint64_t to) {
  uint64_t t;

  for (t = from + 1; t <= to; t++) {
    if (t % ESTCPUSTEP == 0)
      schedcpu(t % HZ == 0);
    if (t % (5 * HZ) == 0)
      sched_loadavg_update();
  }
}

// at its sleep's priority; p->lock held
static void setrunnable(struct proc *p) {
  uint8_t prio = p->state == SLEEPING
                     ? (p->slppri < p->lentpri ? p->slppri : p->lentpri)
                     : sched_priority(p);

  if (p->state == STOPPED && p->p_p->pt_pinned) {
    p->p_p->pt_wake = 1;
    return;
  }
  sched_wake(p);
  sched_setrunqueue(p, prio);
}

static void proc_notify(struct proc *p);

// interrupts off
int cpuid() { return curcpu() - cpus; }

static struct cpu_info *cpu_by_apicid(void) {
  uint32_t apicid = (uint32_t)lapicid();
  int i;

  for (i = 0; i < ncpus; i++) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// per-cpu state at %gs
static int percpu_via_gs;

void percpu_attach(void) {
  struct cpu_info *c = cpu_by_apicid();

  c->self = c;
  wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
  wrmsr(MSR_KERNEL_GS_BASE, 0);
  percpu_via_gs = 1;
}

// interrupts off
struct cpu_info *curcpu(void) {
  struct cpu_info *c;

  if (readeflags() & FL_IF)
    panic("curcpu called with interrupts enabled");

  if (percpu_via_gs) {
    __asm __volatile("movq %%gs:0, %0" : "=r"(c));
    return c;
  }
  return cpu_by_apicid();
}

struct proc *proc_self(void) {
  struct cpu_info *c;
  struct proc *p;
  spinlock_enter();
  c = curcpu();
  p = c->proc;
  spinlock_exit();
  return p;
}

// PRIORITY LENT THROUGH A LOCK
static int lends;

// one owner locked at a time
void lend_priority(struct proc *owner, struct proc *waiter) {
  struct proc *next;
  uint8_t pri = sched_priority(waiter);
  int depth;

  for (depth = 0; owner != 0 && depth < 8; depth++, owner = next) {
    next = 0;
    mtx_enter(&owner->lock);
    if (pri < sched_priority(owner)) {
      __atomic_fetch_add(&lends, 1, __ATOMIC_RELAXED);
      owner->lentpri = pri;
      if (owner->state == RUNNABLE)
        sched_lend(owner, pri);
      else if (owner->state == SLEEPING) {
        if (owner->slppri > pri)
          owner->slppri = pri;
        if (owner->lockwait != 0)
          next = ((struct sleeplock *)owner->lockwait)->owner;
      }
    }
    mtx_leave(&owner->lock);
  }
}

void unlend_priority(struct proc *owner) {
  if (owner->lentpri == NOLEND)
    return;
  mtx_enter(&owner->lock);
  if (owner->nsleeplocks == 0)
    owner->lentpri = NOLEND;
  mtx_leave(&owner->lock);
}

int sleeplock_lends(void) { return lends; }


// proctree_lock held
static struct process *process_find(pid_t pid);

// proctree_lock held
int process_with(pid_t pid, int (*fn)(struct process *, void *), void *arg) {
  struct process *pr;
  int r = -1;

  mtx_enter(&proctree_lock);
  if ((pr = process_find(pid)) != 0)
    r = fn(pr, arg);
  mtx_leave(&proctree_lock);
  return r;
}

static struct process *process_find(pid_t pid) {
  struct process *pr;

  for (pr = pidhash[PROCH(pid)]; pr != 0; pr = pr->ps_hash)
    if (pr->pid == pid)
      return (pr->ps_flags & PS_ZOMBIE) ? 0 : pr;
  return 0;
}

static struct process *zombie_find(pid_t pid) {
  struct process *pr;

  for (pr = pidhash[PROCH(pid)]; pr != 0; pr = pr->ps_hash)
    if (pr->pid == pid)
      return (pr->ps_flags & PS_ZOMBIE) ? pr : 0;
  return 0;
}

static int pid_held(pid_t pid) {
  struct process *pr;

  for (pr = pidhash[PROCH(pid)]; pr != 0; pr = pr->ps_hash)
    if (pr->pid == pid)
      return 1;
  return 0;
}

static struct pgrp *pgfind(pid_t pgid) {
  struct pgrp *pg;

  for (pg = pgrphash[PROCH(pgid)]; pg != 0; pg = pg->pg_hash)
    if (pg->pg_id == pgid)
      return pg;
  return 0;
}

static struct session *sessfind(pid_t sid) {
  struct session *s;

  for (s = sesshash[PROCH(sid)]; s != 0; s = s->s_hash)
    if (s->s_sid == sid)
      return s;
  return 0;
}

static struct proc *tidfind(pid_t tid) {
  struct proc *p;

  for (p = tidhash[PROCH(tid)]; p != 0; p = p->p_hash)
    if (p->tid == tid)
      return p;
  return 0;
}

static pid_t allocpid(void) {
  pid_t pid;
  int n;

  for (n = 0; n < PID_MAX; n++) {
    pid = nextpid;
    nextpid = nextpid >= PID_MAX ? 2 : nextpid + 1;
    if (!pid_held(pid) && pgfind(pid) == 0 && sessfind(pid) == 0)
      return pid;
  }
  return 0;
}

// proctree_lock held
static pid_t alloctid(void) {
  pid_t tid;
  int n;

  for (n = 0; n < PID_MAX; n++) {
    tid = THREAD_PID_OFFSET + 1 + nexttid;
    nexttid = (nexttid + 1) % PID_MAX;
    if (tidfind(tid) == 0)
      return tid;
  }
  return 0;
}

// THE USERS
static struct uidinfo *uifind(uint uid) {
  struct uidinfo *ui, *fresh = 0;

  for (;;) {
    mtx_enter(&proctree_lock);
    for (ui = uihash[uid % UIHASH]; ui != 0; ui = ui->ui_hash)
      if (ui->ui_uid == uid)
        break;
    if (ui == 0 && fresh != 0) {
      ui = fresh;
      fresh = 0;
      ui->ui_uid = uid;
      ui->ui_hash = uihash[uid % UIHASH];
      uihash[uid % UIHASH] = ui;
    }
    if (ui != 0) {
      ui->ui_ref++;
      mtx_leave(&proctree_lock);
      if (fresh != 0)
        free(fresh, M_PROC);
      return ui;
    }
    mtx_leave(&proctree_lock);
    fresh = malloc(sizeof(*fresh), M_PROC, M_WAITOK | M_ZERO);
  }
}

static void uifree(struct uidinfo *ui) {
  struct uidinfo **pp;

  if (--ui->ui_ref > 0)
    return;
  for (pp = &uihash[ui->ui_uid % UIHASH]; *pp != 0; pp = &(*pp)->ui_hash)
    if (*pp == ui) {
      *pp = ui->ui_hash;
      break;
    }
  free(ui, M_PROC);
}

static int chgproccnt(struct uidinfo *ui, int diff, int max) {
  if (diff > 0 && max != 0 && ui->ui_proccnt + diff > max)
    return 0;
  ui->ui_proccnt += diff;
  return 1;
}

// THE LISTS
static void thread_link(struct process *pr, struct proc *p) {
  p->p_p = pr;
  p->p_thrnext = pr->ps_threads;
  pr->ps_threads = p;
  pr->ps_nthreads++;
  p->p_hash = tidhash[PROCH(p->tid)];
  tidhash[PROCH(p->tid)] = p;
  nthreads++;
}

static void thread_unlink(struct proc *p) {
  struct process *pr = p->p_p;
  struct proc **pp;

  for (pp = &pr->ps_threads; *pp != 0; pp = &(*pp)->p_thrnext)
    if (*pp == p) {
      *pp = p->p_thrnext;
      break;
    }
  pr->ps_nthreads--;
  p->p_thrnext = 0;
  for (pp = &tidhash[PROCH(p->tid)]; *pp != 0; pp = &(*pp)->p_hash)
    if (*pp == p) {
      *pp = p->p_hash;
      break;
    }
  nthreads--;
}

static void process_link(struct process *pr) {
  if ((pr->ps_next = allprocess) != 0)
    allprocess->ps_prev = &pr->ps_next;
  pr->ps_prev = &allprocess;
  allprocess = pr;
  pr->ps_hash = pidhash[PROCH(pr->pid)];
  pidhash[PROCH(pr->pid)] = pr;
  nprocs++;
}

static void process_unlink(struct process *pr) {
  struct process **pp;

  if (pr->ps_next != 0)
    pr->ps_next->ps_prev = pr->ps_prev;
  *pr->ps_prev = pr->ps_next;
  for (pp = &pidhash[PROCH(pr->pid)]; *pp != 0; pp = &(*pp)->ps_hash)
    if (*pp == pr) {
      *pp = pr->ps_hash;
      break;
    }
  nprocs--;
}

static void child_link(struct process *parent, struct process *pr) {
  pr->parent = parent;
  if ((pr->ps_sibling = parent->ps_children) != 0)
    parent->ps_children->ps_psibling = &pr->ps_sibling;
  pr->ps_psibling = &parent->ps_children;
  parent->ps_children = pr;
}

static void child_unlink(struct process *pr) {
  if (pr->ps_psibling == 0)
    return;
  if (pr->ps_sibling != 0)
    pr->ps_sibling->ps_psibling = pr->ps_psibling;
  *pr->ps_psibling = pr->ps_sibling;
  pr->ps_sibling = 0;
  pr->ps_psibling = 0;
}

// a traced process's own parent
static void orphan_link(struct process *oparent, struct process *pr) {
  pr->oparent = oparent;
  if ((pr->ps_orphan = oparent->ps_orphans) != 0)
    oparent->ps_orphans->ps_porphan = &pr->ps_orphan;
  pr->ps_porphan = &oparent->ps_orphans;
  oparent->ps_orphans = pr;
}

static void orphan_unlink(struct process *pr) {
  if (pr->ps_porphan != 0) {
    if (pr->ps_orphan != 0)
      pr->ps_orphan->ps_porphan = pr->ps_porphan;
    *pr->ps_porphan = pr->ps_orphan;
  }
  pr->ps_orphan = 0;
  pr->ps_porphan = 0;
  pr->oparent = 0;
}

// PROCESS GROUPS AND SESSIONS

static void orphanpg(struct pgrp *pg) {
  struct process *pr;

  FOREACH_MEMBER(pg, pr) {
    if (pr->ps_flags & PS_STOPPED) {
      FOREACH_MEMBER(pg, pr) {
        post(pr, 0, FNU_SIG_HANGUP, 0);
        post(pr, 0, FNU_SIG_CONTINUE, 0);
      }
      return;
    }
  }
}

static int qualifies(struct process *parent, struct pgrp *pg) {
  return parent != 0 && parent->ps_pgrp != pg &&
         parent->ps_pgrp->pg_session == pg->pg_session;
}

static void jobc_adjust(struct pgrp *pg, int entering) {
  if (entering)
    pg->pg_jobc++;
  else if (--pg->pg_jobc == 0)
    orphanpg(pg);
}

static void fixjobc(struct process *pr, struct pgrp *pg, int entering) {
  struct process *c;

  if (qualifies(pr->parent, pg))
    jobc_adjust(pg, entering);
  FOREACH_CHILD(pr, c) {
    if (c->ps_pgrp != pg && c->ps_pgrp->pg_session == pg->pg_session &&
        !(c->ps_flags & PS_ZOMBIE))
      jobc_adjust(c->ps_pgrp, entering);
  }
}

static void reparent(struct process *pr, struct process *to) {
  if (pr->parent == to)
    return;
  if (!(pr->ps_flags & PS_ZOMBIE) && pr->ps_pgrp != 0) {
    if (qualifies(pr->parent, pr->ps_pgrp))
      jobc_adjust(pr->ps_pgrp, 0);
    if (qualifies(to, pr->ps_pgrp))
      jobc_adjust(pr->ps_pgrp, 1);
  }
  child_unlink(pr);
  child_link(to, pr);
}

static void member_link(struct pgrp *pg, struct process *pr) {
  pr->ps_pgrp = pg;
  pr->pgid = pg->pg_id;
  pr->sid = pg->pg_session->s_sid;
  if ((pr->ps_pgnext = pg->pg_members) != 0)
    pg->pg_members->ps_ppgnext = &pr->ps_pgnext;
  pr->ps_ppgnext = &pg->pg_members;
  pg->pg_members = pr;
}

static void pgdelete(struct pgrp *pg) {
  struct session *s = pg->pg_session;
  struct pgrp **pp;
  struct session **sp;

  for (pp = &pgrphash[PROCH(pg->pg_id)]; *pp != 0; pp = &(*pp)->pg_hash)
    if (*pp == pg) {
      *pp = pg->pg_hash;
      break;
    }
  if (--s->s_count == 0) {
    for (sp = &sesshash[PROCH(s->s_sid)]; *sp != 0; sp = &(*sp)->s_hash)
      if (*sp == s) {
        *sp = s->s_hash;
        break;
      }
    free(s, M_PROC);
  }
  free(pg, M_PROC);
}

static void member_unlink(struct process *pr) {
  struct pgrp *pg = pr->ps_pgrp;

  if (pg == 0)
    return;
  if (pr->ps_pgnext != 0)
    pr->ps_pgnext->ps_ppgnext = pr->ps_ppgnext;
  *pr->ps_ppgnext = pr->ps_pgnext;
  pr->ps_pgnext = 0;
  pr->ps_ppgnext = 0;
  pr->ps_pgrp = 0;
  if (pg->pg_members == 0)
    pgdelete(pg);
}

struct pgalloc {
  struct pgrp *pg;
  struct session *sess;
};

static void pgalloc_get(struct pgalloc *a, int mksess) {
  a->pg = malloc(sizeof(*a->pg), M_PROC, M_WAITOK | M_ZERO);
  a->sess = mksess ? malloc(sizeof(*a->sess), M_PROC, M_WAITOK | M_ZERO) : 0;
}

static void pgalloc_put(struct pgalloc *a) {
  if (a->pg != 0)
    free(a->pg, M_PROC);
  if (a->sess != 0)
    free(a->sess, M_PROC);
}

// proctree_lock held
static void enterpgrp(struct process *pr, pid_t pgid, int mksess,
                      struct pgalloc *a) {
  struct pgrp *pg = pgfind(pgid);

  if (pg == 0) {
    pg = a->pg;
    a->pg = 0;
    pg->pg_id = pgid;
    if (mksess) {
      pg->pg_session = a->sess;
      a->sess = 0;
      pg->pg_session->s_sid = pgid;
      pg->pg_session->s_leader = pr;
      pg->pg_session->s_tty = -1;
      pg->pg_session->s_hash = sesshash[PROCH(pgid)];
      sesshash[PROCH(pgid)] = pg->pg_session;
    } else {
      pg->pg_session = pr->ps_pgrp->pg_session;
    }
    pg->pg_session->s_count++;
    pg->pg_hash = pgrphash[PROCH(pgid)];
    pgrphash[PROCH(pgid)] = pg;
  } else if (pg == pr->ps_pgrp) {
    return;
  }
  if (pr->ps_pgrp == 0) {
    member_link(pg, pr);
    fixjobc(pr, pg, 1);
    return;
  }
  fixjobc(pr, pg, 1);
  fixjobc(pr, pr->ps_pgrp, 0);
  member_unlink(pr);
  member_link(pg, pr);
}

static void pgsignal(struct pgrp *pg, int sig, pid_t sender) {
  struct process *pr;

  if (pg != 0)
    FOREACH_MEMBER(pg, pr) {
      post(pr, 0, sig, sender);
    }
}

// THE THREAD AND THE PROCESS

static int thread_stack(struct proc *p) {
  char *sp;

  if ((p->kstack = kheap_alloc(KSTACKSIZE, PGSIZE, M_WAITOK)) == 0)
    return -1;
  sp = p->kstack + KSTACKSIZE;
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;
  sp -= sizeof(uintptr_t);
  *((uintptr_t *)sp) = (uintptr_t)trapret;
  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (uint64_t)fork_trampoline;
  return 0;
}

static struct proc *thread_alloc(void) {
  struct proc *p = malloc(sizeof(*p), M_PROC, M_WAITOK | M_ZERO);

  if (p == 0)
    return 0;
  if (thread_stack(p) < 0) {
    free(p, M_PROC);
    return 0;
  }
  p->sigstk.flags = FNU_SIGSTACK_OFF;
  p->lentpri = NOLEND;
  p->usrpri = PUSER;
  p->wakefrom = -1;
  p->cpbase = ticks;
  mtx_init(&p->lock, "proc");
  fpuclean(&p->fpu);
  p->state = EMBRYO;
  return p;
}

static void thread_free(struct proc *p) {
  kheap_free(p->kstack);
  free(p, M_PROC);
}

static int room_for(struct uidinfo *ui, uint ruid, int limit) {
  if ((nprocs >= maxproc - 10 && ruid != 0) || nprocs >= maxproc ||
      !thread_room(ruid))
    return 0;
  return chgproccnt(ui, 1, ruid != 0 ? limit : 0);
}

static struct proc *allocproc(struct process *parent, uint ruid, int limit) {
  struct process *pr = malloc(sizeof(*pr), M_PROC, M_WAITOK | M_ZERO);
  struct proc *p = thread_alloc();
  struct uidinfo *ui = uifind(ruid);
  struct pgalloc a = {0, 0};
  int i;

  if (p == 0) {
    free(pr, M_PROC);
    mtx_enter(&proctree_lock);
    uifree(ui);
    mtx_leave(&proctree_lock);
    return 0;
  }
  initsleeplock(&pr->vmlock, "vmspace");
  initsleeplock(&pr->uvlock, "unveil");
  for (i = 0; i < FNU_LIMIT_COUNT; i++) {
    pr->rlim[i].cur = FNU_LIMIT_NONE;
    pr->rlim[i].max = FNU_LIMIT_NONE;
  }
  pr->rlim[FNU_LIMIT_OPENFILES].cur = (uint64_t)maxfilesperproc;
  pr->rlim[FNU_LIMIT_OPENFILES].max = (uint64_t)maxfilesperproc;
  pr->rlim[FNU_LIMIT_STACK].cur = DFLSSIZ;
  pr->rlim[FNU_LIMIT_STACK].max = MAXSSIZ;
  pr->umask = 022;
  fd_init(pr);
  for (i = 0; i < FNU_SIG_COUNT; i++)
    pr->sig_act[i].handler = (uint64_t)FNU_SIGACT_DEFAULT;
  pr->ruid = ruid;
  pr->ps_ruidinfo = ui;
  pr->start_ns = uptime_ns();
  if (parent != 0)
    memmove(pr->args, parent->args, sizeof(pr->args));
  if (parent == 0)
    pgalloc_get(&a, 1);

  mtx_enter(&proctree_lock);
  if (!room_for(ui, ruid, limit)) {
    uifree(ui);
    mtx_leave(&proctree_lock);
    goto fail;
  }
  if ((pr->pid = allocpid()) == 0 || (p->tid = alloctid()) == 0) {
    chgproccnt(ui, -1, 0);
    uifree(ui);
    mtx_leave(&proctree_lock);
    goto fail;
  }
  process_link(pr);
  thread_link(pr, p);
  if (parent != 0) {
    child_link(parent, pr);
    member_link(parent->ps_pgrp, pr);
  } else {
    if (initprocess != 0)
      child_link(initprocess->p_p, pr);
    enterpgrp(pr, pr->pid, 1, &a);
  }
  mtx_leave(&proctree_lock);
  pgalloc_put(&a);
  return p;

fail:
  pgalloc_put(&a);
  thread_free(p);
  free(pr, M_PROC);
  return 0;
}

// off every list; proctree_lock held
static void process_free(struct process *pr) {
  struct proc *t;

  while ((t = pr->ps_threads) != 0) {
    thread_unlink(t);
    mtx_enter(&t->lock);
    mtx_leave(&t->lock);
    thread_free(t);
  }
  orphan_unlink(pr);
  child_unlink(pr);
  member_unlink(pr);
  process_unlink(pr);
  chgproccnt(pr->ps_ruidinfo, -1, 0);
  uifree(pr->ps_ruidinfo);
  free(pr, M_PROC);
}

static void process_undo(struct proc *p) {
  struct process *pr = p->p_p;

  if (pr->pgdir != 0) {
    vm_release(p);
    hat_free(pr->pgdir);
  }
  fd_reset(pr);
  mtx_enter(&proctree_lock);
  if (pr->parent != 0 && qualifies(pr->parent, pr->ps_pgrp))
    jobc_adjust(pr->ps_pgrp, 0);
  process_free(pr);
  mtx_leave(&proctree_lock);
}

int64_t get_nprocs(void) { return nprocs; }

void proc_setruid(struct process *pr, uint ruid) {
  struct uidinfo *ui = uifind(ruid), *old;

  mtx_enter(&proctree_lock);
  old = pr->ps_ruidinfo;
  chgproccnt(old, -1, 0);
  chgproccnt(ui, 1, 0);
  pr->ps_ruidinfo = ui;
  pr->ruid = ruid;
  uifree(old);
  mtx_leave(&proctree_lock);
}

int64_t get_nthreads(void) { return nthreads; }

static void start_init(void) {
  static char *argv[] = {"/usr/bin/init", 0, 0}, *envp[] = {0};

  thread_start(curproc);
  iinit();
  storage_mountroot(rootdev);
  if (namei("/", &initprocess->p_p->cwd) != 0)
    panic("no root");
  vfs_attach_boot();
  msgbuf_restore();
  if (getenv_flag("boot_single"))
    argv[1] = "-s";
  if (exec(argv[0], argv, envp) != 0)
    panic("no %s", argv[0]);
}

void create_init(void) {
  struct proc *p;
  struct process *pr;

  if ((p = allocproc(0, 0, 0)) == 0)
    panic("create_init: no process");
  pr = p->p_p;
  initprocess = p;
  if ((pr->pgdir = hat_alloc()) == 0)
    panic("create_init: out of memory?");
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ss = (SEG_UDATA << 3) | DPL_USER;
  p->tf->rflags = FL_IF;
  p->context->rip = (uint64_t)start_init;

  strlcpy(pr->name, "init", sizeof(pr->name));
  pr->cwd = 0;

  mtx_enter(&p->lock);
  setrunnable(p);
  mtx_leave(&p->lock);
}

pid_t fork(void) {
  int pid;
  struct proc *np;
  struct process *pr = curproc->p_p, *npr;
  int limit = maxprocperuid;

  if (pr->rlim[FNU_LIMIT_PROCS].cur < (uint64_t)limit)
    limit = (int)pr->rlim[FNU_LIMIT_PROCS].cur;
  if ((np = allocproc(pr, pr->ruid, limit)) == 0)
    return -FNU_ERR_WOULDBLOCK;
  npr = np->p_p;

  mtx_enter(&proctree_lock);
  pr->swapfrozen++;
  npr->swapfrozen++;
  mtx_leave(&proctree_lock);
  if ((npr->pgdir = hat_alloc()) == 0 ||
      vm_fork(curproc, np) < 0 ||
      (pr->sigcode != 0 && sigcode_map(npr->pgdir, pr->sigcode) < 0) ||
      fd_copy(pr, npr) < 0) {
    mtx_enter(&proctree_lock);
    pr->swapfrozen--;
    mtx_leave(&proctree_lock);
    process_undo(np);
    return -FNU_ERR_NOMEMORY;
  }
  mtx_enter(&proctree_lock);
  pr->swapfrozen--;
  npr->swapfrozen--;
  mtx_leave(&proctree_lock);
  memmove(npr->login, pr->login, sizeof(npr->login));
  memmove(npr->rlim, pr->rlim, sizeof(npr->rlim));
  npr->nice = pr->nice;
  sched_fork(curproc, np);
  npr->umask = pr->umask;
  npr->pledged = pr->pledged;
  npr->pledge = pr->pledge;
  npr->execpledge = pr->execpledge;
  unveil_copy(pr, npr);
  npr->uid = pr->uid;
  npr->gid = pr->gid;
  npr->ruid = pr->ruid;
  npr->rgid = pr->rgid;
  npr->svuid = pr->svuid;
  npr->svgid = pr->svgid;
  npr->ngroups = pr->ngroups;
  npr->sugid = pr->sugid;
  memmove(npr->groups, pr->groups, sizeof(npr->groups));
  np->sig_mask = curproc->sig_mask;
  np->sigstk = curproc->sigstk;
  np->tcb = curproc->tcb;
  memmove(npr->sig_act, pr->sig_act, sizeof(npr->sig_act));
  npr->sigonstack = pr->sigonstack;
  npr->sigcode = pr->sigcode;
  npr->sigcoderet = pr->sigcoderet;
  npr->sigcookie = pr->sigcookie;
  *np->tf = *curproc->tf;
  fpusave(&curproc->fpu);
  np->fpu = curproc->fpu;

  np->tf->rax = 0;
  np->tf->rflags &= ~FL_CF;

  npr->cwd = fd_cwd(pr);

  strlcpy(npr->name, pr->name, sizeof(pr->name));

  pid = npr->pid;

  mtx_enter(&proctree_lock);

  if (pr->traced && (pr->pt_events & FNU_PTRACE_FORK) != 0 && pr->parent != 0) {
    npr->traced = 1;
    orphan_link(pr, npr);
    reparent(npr, pr->parent);
    npr->pt_events = pr->pt_events;
    npr->pt_report = FNU_PTRACE_FORK;
    npr->pt_other = pr->pid;
    sig_post_locked(np, FNU_SIG_BREAK, 0);
    pr->pt_report = FNU_PTRACE_FORK;
    pr->pt_other = pid;
    sig_post_locked(curproc, FNU_SIG_BREAK, 0);
  }

  knote_proc(pr, FNU_NOTE_FORK, 0);
  mtx_leave(&proctree_lock);
  mtx_enter(&np->lock);
  setrunnable(np);
  mtx_leave(&np->lock);
  return pid;
}

static void thread_undo(struct proc *p) {
  struct process *pr = p->p_p;

  mtx_enter(&proctree_lock);
  thread_unlink(p);
  mtx_leave(&proctree_lock);
  wakeup(&pr->ps_single);
  thread_free(p);
}

int thread_fork(const struct fnu_thread_start *st, pid_t *tidout) {
  struct proc *p;
  struct process *pr = curproc->p_p;
  uintptr_t sp = (st->stack & ~(uintptr_t)15) - sizeof(uint64_t);
  uint64_t zero = 0;
  pid_t tid;

  if (st->stack == 0 || st->entry == 0 || st->entry >= USERTOP ||
      st->stack > USERTOP || copyout(pr->pgdir, sp, &zero, sizeof(zero)) < 0)
    return FNU_ERR_FAULT;
  if ((p = thread_alloc()) == 0)
    return FNU_ERR_NOMEMORY;
  mtx_enter(&proctree_lock);
  if (pr->ps_single != 0 || pr->ps_nthreads >= max_threads_per_proc ||
      !thread_room(pr->uid) || (p->tid = alloctid()) == 0) {
    mtx_leave(&proctree_lock);
    thread_free(p);
    return FNU_ERR_WOULDBLOCK;
  }
  thread_link(pr, p);
  tid = p->tid;
  mtx_leave(&proctree_lock);
  if (st->tid != 0 && copyout(pr->pgdir, st->tid, &tid, sizeof(tid)) < 0) {
    thread_undo(p);
    return FNU_ERR_FAULT;
  }
  *p->tf = *curproc->tf;
  p->tf->rip = st->entry;
  p->tf->rdi = st->arg;
  p->tf->rsp = sp;
  p->tf->rax = 0;
  p->tf->rflags = FL_IF;
  p->tcb = st->tcb;
  p->sig_mask = curproc->sig_mask;

  sched_fork(curproc, p);
  mtx_enter(&p->lock);
  setrunnable(p);
  mtx_leave(&p->lock);
  *tidout = tid;
  return 0;
}

int single_thread(struct proc *p) {
  struct process *pr = p->p_p;
  struct proc *t;

  mtx_enter(&proctree_lock);
  if (pr->ps_single != 0 && pr->ps_single != p) {
    mtx_leave(&proctree_lock);
    return -1;
  }
  pr->ps_single = p;
  FOREACH_THREAD(pr, t) {
    if (t == p)
      continue;
    t->killed = 1;
    mtx_enter(&t->lock);
    if ((t->state == SLEEPING && t->slpcatch) || t->state == STOPPED)
      setrunnable(t);
    else {
      t->sigcheck = 1;
      if (t->state == RUNNING)
        proc_notify(t);
    }
    mtx_leave(&t->lock);
  }
  while (pr->ps_nthreads > 1)
    msleep(&pr->ps_single, &proctree_lock, PWAIT, "single", 0);
  mtx_leave(&proctree_lock);
  return 0;
}

void single_thread_clear(struct proc *p) {
  mtx_enter(&proctree_lock);
  if (p->p_p->ps_single == p)
    p->p_p->ps_single = 0;
  mtx_leave(&proctree_lock);
}

void thread_exit(void) {
  struct proc *p = curproc;
  struct process *pr = p->p_p;

  mtx_enter(&proctree_lock);
  if (pr->ps_nthreads == 1) {
    mtx_leave(&proctree_lock);
    exit();
  }
  thread_unlink(p);
  wakeup(&pr->ps_single);
  mtx_enter(&p->lock);
  p->state = DEAD;
  mtx_leave(&proctree_lock);
  sched();
  panic("dead thread ran");
}

void exit(void) {
  struct proc *t;
  struct process *pr = curproc->p_p, *q, *next;
  pid_t fg = 0;

  if (pr == initprocess->p_p)
    panic("init exiting");
  if (single_thread(curproc) < 0)
    thread_exit();
  // ITIMER_REAL
  mtx_enter(&proctree_lock);
  pr->ps_realat = 0;
  mtx_leave(&proctree_lock);
  callout_stop(&pr->ps_realit);

  fd_closeall(pr, 0);
  fd_reset(pr);

  iput(pr->cwd);
  pr->cwd = 0;
  unveil_destroy(pr);
  vm_release(curproc);

  if (pr->sid == pr->pid)
    fg = console_leave_session(pr->sid);

  mtx_enter(&proctree_lock);

  if (pr->ps_pgrp->pg_session->s_leader == pr) {
    struct pgrp *pg = fg != 0 ? pgfind(fg) : 0;

    if (pg != 0 && pg->pg_session == pr->ps_pgrp->pg_session)
      pgsignal(pg, FNU_SIG_HANGUP, 0);
    pr->ps_pgrp->pg_session->s_leader = 0;
    pr->ps_pgrp->pg_session->s_tty = -1;
  }
  if (qualifies(pr->parent, pr->ps_pgrp))
    jobc_adjust(pr->ps_pgrp, 0);
  pr->ps_realat = 0;

  // a dead tracer lets go
  for (q = pr->ps_children; q != 0; q = next) {
    struct process *op = q->oparent;

    next = q->ps_sibling;
    if (!q->traced)
      continue;
    q->traced = 0;
    q->trapped = 0;
    FOREACH_THREAD(q, t) {
      if (t->tf != 0)
        t->tf->rflags &= ~FL_TF;
    }
    if (op != 0) {
      orphan_unlink(q);
      reparent(q, op);
    }
    process_continue(q, 0);
  }

  while ((q = pr->ps_orphans) != 0) {
    orphan_unlink(q);
    orphan_link(initprocess->p_p, q);
  }
  // a zombie handed on is news for the new parent
  while ((q = pr->ps_children) != 0) {
    reparent(q, initprocess->p_p);
    if (q->ps_flags & PS_ZOMBIE) {
      wakeup(initprocess->p_p);
      sig_post_process(initprocess->p_p, FNU_SIG_CHILD, q->pid);
    }
  }

  knote_proc(pr, FNU_NOTE_EXIT,
             (int64_t)(pr->xsig != 0 ? FNU_EXIT_KILLED : FNU_EXIT_NORMAL) << 32 |
                 (uint32_t)(pr->xsig != 0 ? pr->xsig : pr->xexit));
  pr->ps_flags |= PS_ZOMBIE;
  wakeup(pr->parent);
  sig_post_process(pr->parent, FNU_SIG_CHILD, pr->pid);
  mtx_enter(&curproc->lock);
  curproc->state = ZOMBIE;
  mtx_leave(&proctree_lock);
  sched();
  panic("zombie exit");
}

static void time_add(struct fnu_timespec *a, const struct fnu_timespec *b);
static void rusage_of(struct process *pr, struct fnu_usage *out);

static void reap(struct process *q) {
  if (q->pgdir != kernel_pgdir)
    hat_free(q->pgdir);
  process_free(q);
}

// proctree_lock held
static int wait_match(struct process *q, pid_t target) {
  return target == -1 || q->pid == target || (target < -1 && q->pgid == -target);
}

pid_t waitpid(pid_t target, struct fnu_exitinfo *out, int flags) {
  struct process *q;
  int havekids;
  pid_t pid;
  struct process *pr = curproc->p_p;

  mtx_enter(&proctree_lock);
  if (target == 0)
    target = -pr->pgid;
  for (;;) {
    havekids = 0;
    for (q = pr->ps_orphans; q != 0; q = q->ps_orphan)
      if (wait_match(q, target))
        havekids = 1;
    FOREACH_CHILD(pr, q) {
      if (!wait_match(q, target))
        continue;
      havekids = 1;

      if (q->ps_flags & PS_ZOMBIE) {
        pid = q->pid;
        if (out != 0) {
          out->pid = pid;
          out->uid = q->uid;
          out->reason = q->xsig != 0 ? FNU_EXIT_KILLED : FNU_EXIT_NORMAL;
          out->status = q->xsig != 0 ? q->xsig : q->xexit;
          rusage_of(q, &out->ru);
          time_add(&out->ru.utime, &q->cru.utime);
          time_add(&out->ru.stime, &q->cru.stime);
        }
        if (q->oparent != 0 && q->oparent != pr &&
            !(q->oparent->ps_flags & PS_ZOMBIE)) {
          struct process *op = q->oparent;

          orphan_unlink(q);
          reparent(q, op);
          q->traced = 0;
          wakeup(q->parent);
          sig_post_process(q->parent, FNU_SIG_CHILD, q->pid);
          mtx_leave(&proctree_lock);
          return pid;
        }
        ruadd(pr, q);
        reap(q);
        mtx_leave(&proctree_lock);
        return pid;
      }

      if (((flags & FNU_WAIT_STOPPED) || q->traced) && (q->ps_flags & PS_STOPPED) &&
          !(q->xflags & PX_WAITED)) {
        q->xflags |= PX_WAITED;
        pid = q->pid;
        if (out != 0) {
          out->pid = pid;
          out->uid = q->uid;
          out->reason = FNU_EXIT_STOPPED;
          out->status = q->xsig;
          memset(&out->ru, 0, sizeof(out->ru));
        }
        mtx_leave(&proctree_lock);
        return pid;
      }

      if ((flags & FNU_WAIT_CONTINUED) && (q->xflags & PX_CONTINUED)) {
        q->xflags &= ~PX_CONTINUED;
        pid = q->pid;
        if (out != 0) {
          out->pid = pid;
          out->uid = q->uid;
          memset(&out->ru, 0, sizeof(out->ru));
          out->reason = FNU_EXIT_CONTINUED;
          out->status = FNU_SIG_CONTINUE;
        }
        mtx_leave(&proctree_lock);
        return pid;
      }
    }

    if (!havekids || proc_interrupted()) {
      mtx_leave(&proctree_lock);
      return havekids ? -FNU_ERR_INTERRUPTED : -FNU_ERR_NOCHILD;
    }

    if (flags & FNU_WAIT_NOHANG) {
      mtx_leave(&proctree_lock);
      return 0;
    }

    msleep(pr, &proctree_lock, PWAIT | PCATCH, "wait", 0);
  }
}

static void procinfo_of(struct process *found, struct fnu_procinfo *out) {
  struct session *s = found->ps_pgrp != 0 ? found->ps_pgrp->pg_session : 0;
  struct proc *t;
  enum procstate state = ZOMBIE;
  uint32_t pctcpu = 0;
  uint8_t prio = MAXPRI;

  memset(out, 0, sizeof(*out));
  FOREACH_THREAD(found, t) {
    if (t->state == RUNNING || (t->state == RUNNABLE && state != RUNNING) ||
        (t->state == SLEEPING && state != RUNNING && state != RUNNABLE) ||
        state == ZOMBIE)
      state = t->state;
    pctcpu += t->pctcpu;
    if ((t->state == RUNNING ? t->priority : t->usrpri) < prio)
      prio = t->state == RUNNING ? t->priority : t->usrpri;
    if (t->state == SLEEPING && out->wchan[0] == 0 && t->wmesg != 0)
      strlcpy(out->wchan, t->wmesg, sizeof(out->wchan));
    out->nthreads++;
  }
  out->pid = (pointer_t)found->pid;
  out->state = (proc_state_t)state;
  strlcpy(out->name, found->name, sizeof(out->name));
  out->memory_size = (pointer_t)vm_size(found);
  out->priority = prio;
  out->nice = found->nice;
  out->pctcpu = pctcpu;
  out->ppid = found->parent != 0 ? found->parent->pid : 0;
  out->pgid = found->pgid;
  out->sid = found->sid;
  out->uid = found->uid;
  out->ruid = found->ruid;
  out->gid = found->gid;
  out->rgid = found->rgid;
  out->start = found->start_ns;
  out->utime = found->uticks * (1000000 / HZ);
  out->stime = found->sticks * (1000000 / HZ);
  out->rss = found->rss * (PGSIZE / 1024);
  strlcpy(out->login, found->login, sizeof(out->login));
  memmove(out->args, found->args, sizeof(out->args));
  if (s != 0 && s->s_leader == found)
    out->flags |= FNU_PROC_LEADER;
  if (s != 0 && s->s_tty >= 0) {
    out->flags |= FNU_PROC_CTTY;
    tty_devname(s->s_tty, out->tty, sizeof(out->tty));
    out->tpgid = tty_foreground(s->s_tty);
    out->ttyinput = tty_inputat(s->s_tty);
    if (out->tpgid == found->pgid)
      out->flags |= FNU_PROC_FOREGROUND;
  }
  if (found->traced)
    out->flags |= FNU_PROC_TRACED;
  if (found->sugid)
    out->flags |= FNU_PROC_SUGID;
  if (found->pledged)
    out->flags |= FNU_PROC_PLEDGED;
  if (found->pgdir == kernel_pgdir)
    out->flags |= FNU_PROC_SYSTEM;
}

int procinfo_all(struct fnu_procinfo *out, int n) {
  struct process *pr;
  int i = 0;

  mtx_enter(&proctree_lock);
  FOREACH_PROCESS(pr) {
    if (i < n)
      procinfo_of(pr, &out[i]);
    i++;
  }
  mtx_leave(&proctree_lock);
  return i;
}

void scheduler(void) {
  struct proc *p;
  struct cpu_info *c = curcpu();
  c->proc = 0;

  for (;;) {
    int found = 0;

    // one instruction with interrupts open
    __asm __volatile("sti; pause; cli");
    while (!c->park && (p = sched_chooseproc(c)) != 0) {
      found = 1;
      c->proc = p;
      tss_kstack(p);
      hat_switch(p->p_p->pgdir);
      wrmsr(MSR_FS_BASE, p->tcb);
      p->state = RUNNING;
      p->priority = p->runpri;

      fpurestore(&p->fpu);
      if (p->dbactive)
        dbregs_load(p->dbregs);
      else if (c->dbloaded)
        dbregs_clear();
      c->dbloaded = p->dbactive;
      swtch(&(c->scheduler), p->context);
      // the scheduler's own state: interrupts off
      c->spinlock_intr = 0;
      fpusave(&p->fpu);
      hat_switch(kernel_pgdir);

      c->proc = 0;
      sched_release(c);
      if (p->state == ZOMBIE || p->state == DEAD)
        sched_exit(p);
      if (p->state == DEAD) {
        mtx_leave(&p->lock);
        thread_free(p);
      } else
        mtx_leave(&p->lock);
    }
    if (c->park)
      sched_park(c);
    if (c->park) {
      // stopped until restarted
      c->started = 0;
      wbinvd();
      c->parked = 1;
      wakeup(&parked_chan);
      for (;;)
        __asm __volatile("cli; hlt");
    }
    if (!found && !c->resched) {
      c->idle = 1;
      cpu_idle(&c->resched);
      c->idle = 0;
    }
  }
}

// p->lock only; the scheduler drops it
void sched(void) {
  int spinlock_intr;
  struct proc *p = curproc;

  if (!mtx_owned(&p->lock)) {
    panic("sched p->lock");
  }
  if (curcpu()->spinlock_count != 1) {
    panic("sched locks");
  }
  if (p->state == RUNNING) {
    panic("sched running");
  }
  if (readeflags() & FL_IF) {
    panic("sched interruptible");
  }
  spinlock_intr = curcpu()->spinlock_intr;
  swtch(&p->context, curcpu()->scheduler);
  curcpu()->spinlock_intr = spinlock_intr;
}

void yield(int voluntary) {
  struct proc *p = curproc;

  __atomic_fetch_add(&p->p_p->nivcsw, 1, __ATOMIC_RELAXED);
  mtx_enter(&p->lock);
  sched_requeue(p, voluntary);
  sched();
  mtx_leave(&p->lock);
}

// any locks but a thread's or a run queue's may be held
static void wakeup_n(const volatile void *chan, int n) {
  int b = (int)SLPQ(chan);
  struct proc *p, *next;

  mtx_enter(&slpque[b].lock);
  for (p = slpque[b].head; p != 0 && n != 0; p = next) {
    next = p->slpnext;
    if (p->chan != chan || !p->waiting)
      continue;
    slpq_del(p);
    mtx_enter(&p->lock);
    p->woken = 1;
    if (p->state == SLEEPING)
      setrunnable(p);
    mtx_leave(&p->lock);
    n--;
  }
  mtx_leave(&slpque[b].lock);
}

struct proc *proc_of_pgdir(pte_t *pgdir) {
  struct process *pr;
  struct proc *p = curproc;

  if (p != 0 && p->p_p->pgdir == pgdir)
    return p;
  p = 0;
  mtx_enter(&proctree_lock);
  FOREACH_PROCESS(pr) {
    if (pr->pgdir == pgdir && pr->ps_threads != 0 &&
        !(pr->ps_flags & PS_ZOMBIE)) {
      p = pr->ps_threads;
      break;
    }
  }
  mtx_leave(&proctree_lock);
  return p;
}

// ???
static int swap_holdable(struct process *pr) {
  struct proc *t;
  int ok = 1;

  if (pr->swaplock || pr->traced || pr->swapfrozen != 0)
    return 0;
  pr->swaplock = 1;
  FOREACH_THREAD(pr, t) {
    mtx_enter(&t->lock);
    if (t->state == RUNNING || t->state == EMBRYO)
      ok = 0;
    mtx_leave(&t->lock);
  }
  if (!ok)
    pr->swaplock = 0;
  return ok;
}

int proc_swap_next(pid_t *hand, struct proc **out) {
  struct process *pr, *best = 0;
  int r = -1;

  mtx_enter(&proctree_lock);
  FOREACH_PROCESS(pr) {
    if (pr->pid <= *hand || (pr->ps_flags & PS_ZOMBIE) ||
        pr->ps_threads == 0 || pr->ps_threads->kfn != 0)
      continue;
    if (best == 0 || pr->pid < best->pid)
      best = pr;
  }
  if (best != 0) {
    *hand = best->pid;
    *out = best->ps_threads;
    r = swap_holdable(best);
  }
  mtx_leave(&proctree_lock);
  return r;
}

// inside the swap: leave it
void proc_swap_freeze(struct proc *p) {
  if (p == 0)
    return;
  mtx_enter(&proctree_lock);
  p->p_p->swapfrozen++;
  mtx_leave(&proctree_lock);
}

void proc_swap_thaw(struct proc *p) {
  if (p == 0)
    return;
  mtx_enter(&proctree_lock);
  p->p_p->swapfrozen--;
  mtx_leave(&proctree_lock);
}

void proc_swap_drop(struct proc *p) {
  mtx_enter(&proctree_lock);
  p->p_p->swaplock = 0;
  mtx_leave(&proctree_lock);
}

void wakeup(const volatile void *chan) { wakeup_n(chan, -1); }

void wakeup_one(const volatile void *chan) { wakeup_n(chan, 1); }

static void proc_notify(struct proc *p) {
  int i;

  for (i = 0; i < ncpus; i++)
    if (cpus[i].proc == p) {
      lapic_notify(cpus[i].apicid);
      return;
    }
}

// proctree_lock held; a sleeper looks again
static void sig_wake(struct proc *t) {
  mtx_enter(&t->lock);
  if (t->state == SLEEPING && t->slpcatch &&
      (((t->sig_pending | t->p_p->ps_siglist) & ~t->sig_mask) != 0 ||
       t->killed || (t->p_p->ps_flags & PS_HELD)))
    setrunnable(t);
  else {
    t->sigcheck = 1;
    if (t->state == RUNNING && t != curproc)
      proc_notify(t);
  }
  mtx_leave(&t->lock);
}

void process_continue(struct process *pr, int report) {
  struct proc *t;

  if ((pr->ps_flags & PS_STOPPED) && report) {
    pr->xflags = (pr->xflags & ~PX_WAITED) | PX_CONTINUED;
    if (pr->parent != 0) {
      wakeup(pr->parent);
      sig_post_process(pr->parent, FNU_SIG_CHILD, pr->pid);
    }
  }
  pr->ps_flags &= ~PS_STOPPED;
  if (pr->ps_flags & PS_FROZEN)
    return;
  FOREACH_THREAD(pr, t) {
    mtx_enter(&t->lock);
    if (t->state == STOPPED)
      setrunnable(t);
    mtx_leave(&t->lock);
  }
}

#define FREEZE_SECONDS 20

// SUSPEND: hold everyone like SIGSTOP, quietly

static int freezable(struct process *pr) {
  return pr != curproc->p_p && !(pr->ps_flags & PS_ZOMBIE) && pr->ps_threads != 0 &&
         pr->ps_threads->kfn == 0;
}

// proctree_lock held: name of one still running, or 0
static const char *freeze_round(void) {
  struct process *pr;
  struct proc *t;
  const char *busy = 0;

  FOREACH_PROCESS(pr) {
    if (!freezable(pr))
      continue;
    if (!(pr->ps_flags & PS_FROZEN)) {
      pr->ps_flags |= PS_FROZEN;
      FOREACH_THREAD(pr, t) sig_wake(t);
    }
    FOREACH_THREAD(pr, t) {
      if (t->state == RUNNING || t->state == RUNNABLE)
        busy = pr->name;
    }
  }
  return busy;
}

int proc_freeze_all(void) {
  const char *busy;
  int i;

  for (i = 0; i < FREEZE_SECONDS * HZ; i++) {
    mtx_enter(&proctree_lock);
    busy = freeze_round();
    mtx_leave(&proctree_lock);
    if (busy == 0)
      return 0;
    tsleep(&freeze_round, PWAIT, "freeze", 1);
  }
  printf("suspend: %s would not stop\n", busy);
  proc_thaw_all();
  return -1;
}

void proc_thaw_all(void) {
  struct process *pr;
  struct proc *t;

  mtx_enter(&proctree_lock);
  FOREACH_PROCESS(pr) {
    if (!(pr->ps_flags & PS_FROZEN))
      continue;
    pr->ps_flags &= ~PS_FROZEN;
    if ((pr->ps_flags & PS_STOPPED) || pr->trapped)
      continue;
    FOREACH_THREAD(pr, t) {
      mtx_enter(&t->lock);
      if (t->state == STOPPED)
        setrunnable(t);
      mtx_leave(&t->lock);
    }
  }
  mtx_leave(&proctree_lock);
}

// proctree_lock held
static int sig_held(struct process *pr, struct proc *t, int sig) {
  struct proc *q;

  if (t != 0)
    return (t->sig_mask & FNU_SIGBIT(sig)) != 0;
  FOREACH_THREAD(pr, q) {
    if (q->sig_mask & FNU_SIGBIT(sig))
      return 1;
  }
  return 0;
}

static void post(struct process *pr, struct proc *t, int sig, pid_t sender) {
  struct proc *q;
  int i;

  if (pr == 0 || sig <= 0 || sig >= FNU_SIG_COUNT || (pr->ps_flags & PS_ZOMBIE))
    return;
  knote_signal(pr, sig);

  // FNU_SIG_CONTINUE
  if (pr->sig_act[sig].handler == (uint64_t)FNU_SIGACT_IGNORE && !sig_continues(sig))
    return;
  if (pr->sig_act[sig].handler == (uint64_t)FNU_SIGACT_DEFAULT &&
      sig_default_ignores(sig) && !sig_continues(sig) && !pr->traced &&
      !sig_held(pr, t, sig))
    return;

  if (sig_continues(sig)) {
    for (i = 1; i < FNU_SIG_COUNT; i++)
      if (sig_stops(i)) {
        pr->ps_siglist &= ~FNU_SIGBIT(i);
        FOREACH_THREAD(pr, q) {
          q->sig_pending &= ~FNU_SIGBIT(i);
        }
      }
    process_continue(pr, 1);
  } else if (sig_stops(sig)) {
    pr->ps_siglist &= ~FNU_SIGBIT(FNU_SIG_CONTINUE);
    FOREACH_THREAD(pr, q) {
      q->sig_pending &= ~FNU_SIGBIT(FNU_SIG_CONTINUE);
    }
  }

  if (sig == FNU_SIG_KILL && pr->pid != 1) {
    pr->xsig = FNU_SIG_KILL;
    FOREACH_THREAD(pr, q) {
      q->killed = 1;
      mtx_enter(&q->lock);
      if (q->state == STOPPED)
        setrunnable(q);
      mtx_leave(&q->lock);
      sig_wake(q);
    }
  }

  if (t == 0) {
    FOREACH_THREAD(pr, q) {
      if (q->state != EMBRYO && !(q->sig_mask & FNU_SIGBIT(sig))) {
        t = q;
        break;
      }
    }
  }
  if (t == 0) {
    pr->ps_siglist |= FNU_SIGBIT(sig);
    pr->ps_sender[sig] = sender;
    return;
  }
  t->sig_pending |= FNU_SIGBIT(sig);
  t->sig_sender[sig] = sender;
  sig_wake(t);
}

void sig_post_locked(struct proc *p, int sig, pid_t sender) {
  post(p->p_p, p, sig, sender);
}

void sig_post(struct proc *p, int sig, pid_t sender) {
  mtx_enter(&proctree_lock);
  post(p->p_p, p, sig, sender);
  mtx_leave(&proctree_lock);
}

// with proctree_lock held
void sig_post_process(struct process *pr, int sig, pid_t sender) {
  post(pr, 0, sig, sender);
}

uint64_t sig_pending_of(struct proc *p) {
  return p->sig_pending | p->p_p->ps_siglist;
}

// proctree_lock held
static pid_t take(struct proc *p, int sig) {
  struct process *pr = p->p_p;
  pid_t sender;

  if (p->sig_pending & FNU_SIGBIT(sig)) {
    p->sig_pending &= ~FNU_SIGBIT(sig);
    sender = p->sig_sender[sig];
    p->sig_sender[sig] = 0;
  } else {
    pr->ps_siglist &= ~FNU_SIGBIT(sig);
    sender = pr->ps_sender[sig];
    pr->ps_sender[sig] = 0;
  }
  return sender;
}

pid_t sig_forget(struct proc *p, int sig) {
  pid_t sender;

  mtx_enter(&proctree_lock);
  sender = take(p, sig);
  mtx_leave(&proctree_lock);
  return sender;
}

// proctree_lock held
static void process_stop(struct process *pr, int sig) {
  struct proc *t;

  pr->xsig = sig;
  pr->xflags &= ~PX_WAITED;
  pr->ps_flags |= PS_STOPPED;
  FOREACH_THREAD(pr, t) {
    if (t != curproc)
      sig_wake(t);
  }
  if (pr->parent != 0) {
    wakeup(pr->parent);
    sig_post_process(pr->parent, FNU_SIG_CHILD, pr->pid);
  }
}

// proctree_lock held: stopped, and back with it held
static void stop_self(struct proc *p) {
  mtx_enter(&p->lock);
  p->state = STOPPED;
  mtx_leave(&proctree_lock);
  sched();
  mtx_leave(&p->lock);
  mtx_enter(&proctree_lock);
}

void proc_stop(int sig) {
  struct proc *p = curproc;

  mtx_enter(&proctree_lock);
  process_stop(p->p_p, sig);
  stop_self(p);
  mtx_leave(&proctree_lock);
}

void proc_suspend(void) {
  struct proc *p = curproc;

  mtx_enter(&proctree_lock);
  while ((p->p_p->ps_flags & PS_HELD) && !p->killed)
    stop_self(p);
  mtx_leave(&proctree_lock);
}

int proc_trace_stop(int sig) {
  struct proc *p = curproc;
  struct process *pr = p->p_p;

  mtx_enter(&proctree_lock);
  pr->trapped = 1;
  pr->pt_thread = p;
  process_stop(pr, sig);
  stop_self(p);
  pr->trapped = 0;
  sig = pr->xsig;
  pr->xsig = 0;
  mtx_leave(&proctree_lock);
  return sig;
}

// for ptrace
struct proc *proc_hold(pid_t pid) {
  struct process *pr;

  mtx_enter(&proctree_lock);
  if ((pr = process_find(pid)) != 0 && pr->ps_threads != 0)
    return pr->trapped && pr->pt_thread != 0 ? pr->pt_thread : pr->ps_threads;
  mtx_leave(&proctree_lock);
  return 0;
}

void proc_release(void) { mtx_leave(&proctree_lock); }

void proc_lock(void) { mtx_enter(&proctree_lock); }

void proc_unlock(void) { mtx_leave(&proctree_lock); }

void proc_pin(struct proc *p) { p->p_p->pt_pinned++; }

void proc_unpin(struct proc *p) {
  struct process *pr = p->p_p;

  mtx_enter(&proctree_lock);
  if (--pr->pt_pinned == 0 && pr->pt_wake) {
    pr->pt_wake = 0;
    process_continue(pr, 0);
  }
  mtx_leave(&proctree_lock);
}

// proctree_lock held
void proc_resume(struct proc *p) { process_continue(p->p_p, 0); }

// FNU_PT_ATTACH
void proc_trace_attach(struct proc *p, struct process *tracer) {
  struct process *pr = p->p_p;

  orphan_link(pr->parent, pr);
  reparent(pr, tracer);
}

// FNU_PT_DETACH
void proc_trace_detach(struct proc *p) {
  struct process *pr = p->p_p, *op = pr->oparent;

  if (op == 0)
    return;
  orphan_unlink(pr);
  reparent(pr, op);
}

// ending by a signal
void exit_signal(int sig) {
  struct proc *p = curproc;

  p->p_p->xexit = 0;
  p->p_p->xsig = sig;
  exit();
}

// THE CLOCK
// every cpu ticks; ticks is the furthest
void hardclock(int usermode) {
  static const int alarm_of[FNU_TIMER_COUNT] = {FNU_SIG_ALARM, FNU_SIG_VTALARM, FNU_SIG_PROFILE};
  struct cpu_info *c = curcpu();
  struct proc *cur = c->proc;
  struct process *pr;
  uint64_t global, mine;
  int i;

  mine = ++c->pcputicks;
  global = ticks;
  while (mine > global)
    if (__atomic_compare_exchange_n((uint64_t *)&ticks, &global, mine, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
      callout_hardclock((int)(mine - global));
      sched_tick(global, mine);
      break;
    }
  if (c->idle)
    c->ticks_idle++;
  else
    c->ticks_busy++;
  sched_clock(c, cur, usermode);
  if (cur == 0 || cur->kfn != 0)
    return;
  pr = cur->p_p;
  __atomic_fetch_add(usermode ? &pr->uticks : &pr->sticks, 1, __ATOMIC_RELAXED);
  if (pr->rss * (PGSIZE / 1024) > pr->maxrss)
    pr->maxrss = pr->rss * (PGSIZE / 1024);
  if (pr->rlim[FNU_LIMIT_CPU].cur == FNU_LIMIT_NONE &&
      pr->itimer_value[FNU_TIMER_VIRTUAL] == 0 && pr->itimer_value[FNU_TIMER_PROFILE] == 0)
    return;
  mtx_enter(&proctree_lock);
  if (pr->rlim[FNU_LIMIT_CPU].cur != FNU_LIMIT_NONE &&
      (uint64_t)((pr->uticks + pr->sticks) / HZ) > pr->rlim[FNU_LIMIT_CPU].cur)
    sig_post_process(pr, FNU_SIG_CPULIMIT, 0);
  for (i = FNU_TIMER_VIRTUAL; i <= FNU_TIMER_PROFILE; i++) {
    if (pr->itimer_value[i] == 0)
      continue;
    if (i == FNU_TIMER_VIRTUAL && !usermode)
      continue;
    if (--pr->itimer_value[i] > 0)
      continue;
    pr->itimer_value[i] = pr->itimer_interval[i];
    sig_post_process(pr, alarm_of[i], 0);
  }
  mtx_leave(&proctree_lock);
}

static int64_t ticks_of(const struct fnu_timespec *t) {
  int64_t n;

  if (t->sec < 0 || t->sec > 100000000 || t->nsec < 0 || t->nsec >= 1000000000)
    return -1;
  n = t->sec * HZ + t->nsec / (1000000000 / HZ);
  if (n == 0 && (t->sec != 0 || t->nsec != 0))
    n = 1;
  return n;
}

static void ticks_to_time(int64_t n, struct fnu_timespec *t) {
  t->sec = n / HZ;
  t->nsec = (int32_t)((n % HZ) * (1000000000 / HZ));
  t->pad = 0;
}

int setitimer(int which, const struct fnu_timer *in, struct fnu_timer *out) {
  struct process *pr = curproc->p_p;
  int64_t value, interval;

  if (which < 0 || which >= FNU_TIMER_COUNT)
    return FNU_ERR_INVALID;
  if (in != 0) {
    if ((value = ticks_of(&in->value)) < 0 ||
        (interval = ticks_of(&in->interval)) < 0)
      return FNU_ERR_INVALID;
  }
  mtx_enter(&proctree_lock);
  if (out != 0) {
    // ITIMER_REAL
    if (which == FNU_TIMER_REAL)
      ticks_to_time(pr->ps_realat == 0     ? 0
                    : pr->ps_realat > ticks ? (int64_t)(pr->ps_realat - ticks)
                                            : 1,
                    &out->value);
    else
      ticks_to_time(pr->itimer_value[which], &out->value);
    ticks_to_time(pr->itimer_interval[which], &out->interval);
  }
  if (in != 0) {
    pr->itimer_interval[which] = value == 0 ? 0 : interval;
    if (which != FNU_TIMER_REAL)
      pr->itimer_value[which] = value;
    else if ((pr->ps_realat = value != 0 ? ticks + (uint64_t)value : 0) != 0)
      realit_arm(pr);
    else
      callout_stop_async(&pr->ps_realit);
  }
  mtx_leave(&proctree_lock);
  return 0;
}

int setrlimit(int which, const struct fnu_limit *in, struct fnu_limit *out) {
  struct process *pr = curproc->p_p;

  if (which < 0 || which >= FNU_LIMIT_COUNT)
    return FNU_ERR_INVALID;
  if (out != 0)
    *out = pr->rlim[which];
  if (in != 0) {
    if (in->max > pr->rlim[which].max && pr->uid != 0)
      return FNU_ERR_NOTPERMITTED;
    if (in->cur > in->max)
      return FNU_ERR_INVALID;
    pr->rlim[which] = *in;
  }
  return 0;
}

static void time_add(struct fnu_timespec *a, const struct fnu_timespec *b) {
  a->sec += b->sec;
  a->nsec += b->nsec;
  if (a->nsec >= 1000000000) {
    a->nsec -= 1000000000;
    a->sec++;
  }
}

static void rusage_of(struct process *pr, struct fnu_usage *out) {
  memset(out, 0, sizeof(*out));
  ticks_to_time(pr->uticks, &out->utime);
  ticks_to_time(pr->sticks, &out->stime);
  out->maxrss = (int64_t)pr->maxrss;
  out->nsignals = pr->nsignals;
  out->nvcsw = pr->nvcsw;
  out->nivcsw = pr->nivcsw;
}

// a reaped child joins cru
void ruadd(struct process *parent, struct process *child) {
  struct fnu_usage own, *c = &parent->cru;

  rusage_of(child, &own);
  time_add(&c->utime, &own.utime);
  time_add(&c->stime, &own.stime);
  time_add(&c->utime, &child->cru.utime);
  time_add(&c->stime, &child->cru.stime);
  if (own.maxrss > c->maxrss)
    c->maxrss = own.maxrss;
  if (child->cru.maxrss > c->maxrss)
    c->maxrss = child->cru.maxrss;
  c->nsignals += own.nsignals + child->cru.nsignals;
  c->nvcsw += own.nvcsw + child->cru.nvcsw;
  c->nivcsw += own.nivcsw + child->cru.nivcsw;
}

int proc_rusage(int who, struct fnu_usage *out) {
  struct process *pr = curproc->p_p;

  mtx_enter(&proctree_lock);
  if (who == FNU_USAGE_SELF)
    rusage_of(pr, out);
  else if (who == FNU_USAGE_CHILDREN)
    *out = pr->cru;
  mtx_leave(&proctree_lock);
  return who == FNU_USAGE_SELF || who == FNU_USAGE_CHILDREN ? 0 : FNU_ERR_INVALID;
}

// nice(2)
static int kernel_process(struct process *pr);

static int donice(struct process *self, struct process *pr, int how, int *value) {
  struct proc *t;
  int want = how == FNU_NICE_ADD ? pr->nice + *value : *value;

  if (self->uid != 0 && self->uid != pr->uid && self->ruid != pr->uid)
    return FNU_ERR_NOTPERMITTED;
  if (want < FNU_NICE_MIN)
    want = FNU_NICE_MIN;
  if (want > FNU_NICE_MAX)
    want = FNU_NICE_MAX;
  if (want < pr->nice && self->uid != 0)
    return FNU_ERR_DENIED;
  pr->nice = want;
  FOREACH_THREAD(pr, t) {
    mtx_enter(&t->lock);
    sched_resetpriority(t);
    mtx_leave(&t->lock);
  }
  return 0;
}

static int nice_match(struct process *pr, struct process *self, int whom, int id) {
  if (kernel_process(pr) || (pr->ps_flags & PS_ZOMBIE))
    return 0;
  switch (whom) {
  case FNU_NICE_PROCESS: return pr->pid == (id != 0 ? id : self->pid);
  case FNU_NICE_GROUP: return pr->pgid == (id != 0 ? id : self->pgid);
  default: return pr->uid == (id != 0 ? (uint)id : self->uid);
  }
}

int proc_niceness(int whom, int id, int how, int *value) {
  struct process *self = curproc->p_p, *pr;
  int found = 0, least = FNU_NICE_MAX + 1, error = 0, e, v;

  if (whom < FNU_NICE_PROCESS || whom > FNU_NICE_USER || how < FNU_NICE_GET ||
      how > FNU_NICE_ADD || id < 0)
    return FNU_ERR_INVALID;
  mtx_enter(&proctree_lock);
  FOREACH_PROCESS(pr) {
    if (!nice_match(pr, self, whom, id))
      continue;
    found = 1;
    v = *value;
    if (how != FNU_NICE_GET && (e = donice(self, pr, how, &v)) != 0)
      error = e;
    if (pr->nice < least)
      least = pr->nice;
  }
  mtx_leave(&proctree_lock);
  if (!found)
    return FNU_ERR_NOPROCESS;
  *value = least;
  return error;
}

int proc_umask(int mask) {
  struct process *pr = curproc->p_p;
  int old = pr->umask;

  if (mask >= 0)
    pr->umask = (ushort)(mask & 0777);
  return old;
}

// what ends an interruptible sleep
int proc_interrupted(void) {
  struct proc *p = curproc;
  struct process *pr;

  if (p == 0)
    return 0;
  pr = p->p_p;
  if (p->killed)
    return 1;
  if ((sig_pending_of(p) & ~p->sig_mask) != 0 || (pr->ps_flags & PS_STOPPED)) {
    p->sig_restart = 1;
    return 1;
  }
  return 0;
}

int groupmember(uint gid) {
  struct proc *p = curproc;
  int i;

  if (p == 0)
    return 0;
  if (p->p_p->gid == gid)
    return 1;
  for (i = 0; i < p->p_p->ngroups; i++)
    if (p->p_p->groups[i] == gid)
      return 1;
  return 0;
}

int proc_uid_of(pid_t pid, uint *uid) {
  struct process *pr;
  int found = -1;

  mtx_enter(&proctree_lock);
  if ((pr = process_find(pid)) != 0) {
    *uid = pr->uid;
    found = 0;
  }
  mtx_leave(&proctree_lock);
  return found;
}

static int cansignal(struct process *self, struct process *pr, int sig) {
  if (self == 0 || self->uid == 0)
    return 1;
  if (sig == FNU_SIG_CONTINUE && pr->sid == self->sid)
    return 1;
  return self->ruid == pr->ruid || self->ruid == pr->uid ||
         self->uid == pr->ruid || self->uid == pr->uid;
}

static int kernel_process(struct process *pr) {
  return pr->ps_threads != 0 && pr->ps_threads->kfn != 0;
}

static int killable(struct process *pr, struct process *self, int all) {
  return !(pr->ps_flags & PS_ZOMBIE) && !kernel_process(pr) && pr->pid > 1 &&
         !(all && pr == self);
}

static int kill_one(struct process *pr, struct process *self, int sig,
                    int ask, pid_t sender) {
  if (ask && !cansignal(self, pr, sig))
    return 0;
  if (sig != 0)
    post(pr, 0, sig, sender);
  return 1;
}

static int dokill(int pid, int sig, int ask) {
  struct proc *me = curproc;
  struct process *pr, *self = me ? me->p_p : 0;
  struct pgrp *pg = 0;
  pid_t sender = self ? self->pid : 0;
  int found = 0, sent = 0;

  if (sig < 0 || sig >= FNU_SIG_COUNT)
    return FNU_ERR_INVALID;

  mtx_enter(&proctree_lock);
  if (pid > 0) {
    if ((pr = process_find(pid)) != 0 && !kernel_process(pr)) {
      found = 1;
      sent = kill_one(pr, self, sig, ask, sender);
    } else if ((pr = zombie_find(pid)) != 0) {
      // a zombie is still there, and takes no signal
      found = 1;
      sent = !ask || cansignal(self, pr, sig);
    }
  } else if (pid == -1) {
    FOREACH_PROCESS(pr) {
      if (!killable(pr, self, 1))
        continue;
      found = 1;
      sent += kill_one(pr, self, sig, ask, sender);
    }
  } else {
    pg = pid == 0 ? (self ? self->ps_pgrp : 0) : pgfind(-pid);
    if (pg != 0)
      FOREACH_MEMBER(pg, pr) {
        if (!killable(pr, self, 0))
          continue;
        found = 1;
        sent += kill_one(pr, self, sig, ask, sender);
      }
  }
  mtx_leave(&proctree_lock);
  return sent ? 0 : (found ? FNU_ERR_NOTPERMITTED : FNU_ERR_NOPROCESS);
}

int kill(int pid, int sig) { return dokill(pid, sig, 0); }

int kill_perm(int pid, int sig) { return dokill(pid, sig, 1); }

int session_tty(struct process *pr) {
  int tty;

  mtx_enter(&proctree_lock);
  tty = pr->ps_pgrp->pg_session->s_tty;
  mtx_leave(&proctree_lock);
  return tty;
}

// FNU_TTYCONTROL
int session_take_tty(struct process *pr, int tty) {
  struct session *s;
  int error = FNU_ERR_NOTPERMITTED;

  mtx_enter(&proctree_lock);
  s = pr->ps_pgrp->pg_session;
  if (s->s_leader == pr && (s->s_tty == -1 || s->s_tty == tty)) {
    s->s_tty = tty;
    error = 0;
  }
  mtx_leave(&proctree_lock);
  return error;
}

void tty_pgsignal(pid_t pgid, pid_t sid, int sig) {
  struct pgrp *pg;

  mtx_enter(&proctree_lock);
  if ((pg = pgfind(pgid)) != 0 && pg->pg_session->s_sid == sid)
    pgsignal(pg, sig, 0);
  mtx_leave(&proctree_lock);
}

pid_t setsid(void) {
  struct process *pr = curproc->p_p;
  struct pgalloc a;

  pgalloc_get(&a, 1);
  mtx_enter(&proctree_lock);
  if (pr->pgid == pr->pid || pgfind(pr->pid) != 0) {
    mtx_leave(&proctree_lock);
    pgalloc_put(&a);
    return -FNU_ERR_NOTPERMITTED;
  }
  enterpgrp(pr, pr->pid, 1, &a);
  mtx_leave(&proctree_lock);
  pgalloc_put(&a);
  console_leave_session(pr->pid);
  return pr->pid;
}

// setpgid(2)
int setpgid(pid_t pid, pid_t pgid) {
  struct process *self = curproc->p_p, *target;
  struct pgrp *pg;
  struct pgalloc a;
  int ret = FNU_ERR_NOPROCESS;

  if (pid < 0 || pgid < 0)
    return FNU_ERR_INVALID;
  pgalloc_get(&a, 0);
  mtx_enter(&proctree_lock);
  target = pid == 0 || pid == self->pid ? self : process_find(pid);
  if (target == 0 || (target != self && target->parent != self))
    goto out;
  ret = FNU_ERR_NOTPERMITTED;
  if (target->ps_pgrp->pg_session != self->ps_pgrp->pg_session)
    goto out;
  ret = FNU_ERR_DENIED;
  if (target != self && (target->ps_flags & PS_EXEC))
    goto out;
  ret = FNU_ERR_NOTPERMITTED;
  if (target->ps_pgrp->pg_session->s_leader == target)
    goto out;
  if (pgid == 0)
    pgid = target->pid;
  else if (pgid != target->pid &&
           ((pg = pgfind(pgid)) == 0 ||
            pg->pg_session != self->ps_pgrp->pg_session))
    goto out;
  enterpgrp(target, pgid, 0, &a);
  ret = 0;

out:
  mtx_leave(&proctree_lock);
  pgalloc_put(&a);
  return ret;
}

int proc_ids(pid_t pid, struct fnu_procids *out) {
  struct process *pr;
  int error = FNU_ERR_NOPROCESS;

  mtx_enter(&proctree_lock);
  if ((pr = pid == 0 ? curproc->p_p : process_find(pid)) != 0) {
    out->pid = pr->pid;
    out->ppid = pr->parent != 0 ? pr->parent->pid : 0;
    out->pgid = pr->pgid;
    out->sid = pr->sid;
    error = 0;
  }
  mtx_leave(&proctree_lock);
  return error;
}

pid_t pgrp_session(pid_t pgid) {
  struct pgrp *pg;
  pid_t ret = -FNU_ERR_NOPROCESS;

  mtx_enter(&proctree_lock);
  if ((pg = pgfind(pgid)) != 0)
    ret = pg->pg_session->s_sid;
  mtx_leave(&proctree_lock);
  return ret;
}

int proc_sig_masked(struct proc *p, int sig) {
  return (p->sig_mask & FNU_SIGBIT(sig)) != 0 ||
         p->p_p->sig_act[sig].handler == (uint64_t)FNU_SIGACT_IGNORE;
}

// pg_jobc == 0
int proc_pgrp_orphaned(struct process *pr) {
  int r;

  mtx_enter(&proctree_lock);
  r = pr->ps_pgrp == 0 || pr->ps_pgrp->pg_jobc == 0;
  mtx_leave(&proctree_lock);
  return r;
}

// the ^T line
void pgrp_status(pid_t pgid, char *buf, size_t n) {
  struct process *pr, *pick = 0;
  struct proc *t, *tpick = 0;
  struct fnu_timespec ut, st;
  uint32_t cpu, pickcpu = 0;
  int run, pickrun = 0;
  const char *state;
  struct pgrp *pg;

  mtx_enter(&proctree_lock);
  if ((pg = pgfind(pgid)) == 0 || pg->pg_members == 0) {
    mtx_leave(&proctree_lock);
    snprintf(buf, n, "empty foreground process group\n");
    return;
  }
  FOREACH_MEMBER(pg, pr) {
    run = 0;
    cpu = 0;
    FOREACH_THREAD(pr, t) {
      run |= t->state == RUNNING || t->state == RUNNABLE;
      cpu += t->pctcpu;
    }
    if (pick == 0 || (run && !pickrun) || (run == pickrun && cpu >= pickcpu)) {
      pick = pr;
      pickrun = run;
      pickcpu = cpu;
    }
  }
  FOREACH_THREAD(pick, t) {
    if (tpick == 0 || t->state == RUNNING ||
        (t->state == RUNNABLE && tpick->state != RUNNING) || t->pctcpu > tpick->pctcpu)
      tpick = t;
  }
  state = tpick == 0 ? "zombie"
          : tpick->state == RUNNING ? "running"
          : tpick->state == RUNNABLE ? "runnable"
          : tpick->state == STOPPED ? "stopped"
          : tpick->wmesg != 0 ? tpick->wmesg : "iowait";
  ticks_to_time(pick->uticks, &ut);
  ticks_to_time(pick->sticks, &st);
  snprintf(buf, n, " cmd: %s %d [%s] %ld.%02du %ld.%02ds %u%% %luk\n", pick->name,
           pick->pid, state, (long)ut.sec, ut.nsec / 10000000, (long)st.sec,
           st.nsec / 10000000, (pickcpu * 100 + FSCALE / 2) / FSCALE,
           (unsigned long)(pick->rss * (PGSIZE / 1024)));
  mtx_leave(&proctree_lock);
}

int signal_take(uint64_t set, struct fnu_siginfo *out, int block) {
  struct proc *p = curproc;
  uint64_t saved, ready;
  int sig;

  set &= ~FNU_SIG_UNCATCHABLE;
  if (set == 0)
    return FNU_ERR_INVALID;

  mtx_enter(&proctree_lock);
  saved = p->sig_mask;
  p->sig_mask &= ~set;
  for (;;) {
    ready = sig_pending_of(p) & set;
    if (ready != 0)
      break;
    if (!block || p->killed || (sig_pending_of(p) & ~p->sig_mask & ~set) != 0) {
      p->sig_mask = saved;
      mtx_leave(&proctree_lock);
      return block ? FNU_ERR_INTERRUPTED : FNU_ERR_WOULDBLOCK;
    }
    msleep(&p->sig_mask, &proctree_lock, PWAIT | PCATCH, "sigwait", 0);
  }
  for (sig = 1; sig < FNU_SIG_COUNT; sig++)
    if (ready & FNU_SIGBIT(sig))
      break;
  p->sig_mask = saved;
  memset(out, 0, sizeof(*out));
  out->sig = sig;
  out->sender = take(p, sig);
  out->from = out->sender ? FNU_SIGFROM_PROCESS : FNU_SIGFROM_SYSTEM;
  mtx_leave(&proctree_lock);
  return 0;
}

static void thread_start(struct proc *p) {
  curcpu()->spinlock_intr = FL_IF;
  mtx_leave(&p->lock);
}

void fork_trampoline(void) { thread_start(curproc); }

static void kthreadret(void) {
  struct proc *p;

  p = curproc;
  thread_start(p);
  p->kfn(p->kfnarg);
  kthread_exit(0);
}

int kthread_create(void (*func)(void *), void *arg, struct proc **newpp,
                   const char *name) {
  struct proc *p;

  if (func == 0)
    return FNU_ERR_INVALID;
  if ((p = allocproc(0, 0, 0)) == 0)
    return FNU_ERR_WOULDBLOCK;

  p->p_p->pgdir = kernel_pgdir;
  p->kfn = func;
  p->kfnarg = arg;
  p->pinned = 0;
  p->context->rip = (uint64_t)kthreadret;
  strlcpy(p->p_p->name, name, sizeof(p->p_p->name));
  snprintf(p->p_p->args, sizeof(p->p_p->args), "[%s]", name);

  mtx_enter(&p->lock);
  setrunnable(p);
  mtx_leave(&p->lock);
  if (newpp != 0)
    *newpp = p;
  return 0;
}

void kthread_exit(int ret __unused) {
  struct proc *p = curproc;

  mtx_enter(&proctree_lock);
  p->p_p->ps_flags |= PS_ZOMBIE;
  wakeup(initprocess->p_p);
  mtx_enter(&p->lock);
  p->state = ZOMBIE;
  mtx_leave(&proctree_lock);
  sched();
  panic("zombie kthread exit");
}

// SLEEP
void cpu_park_others(void) {
  struct cpu_info *me;
  int i, all;

  spinlock_enter();
  me = curcpu();
  spinlock_exit();
  for (i = 0; i < ncpus; i++)
    if (&cpus[i] != me && cpus[i].started)
      cpus[i].park = 1;
  for (;;) {
    sleep_setup(&parked_chan, PWAIT, "cpupark");
    all = 1;
    for (i = 0; i < ncpus; i++)
      if (cpus[i].park && !cpus[i].parked)
        all = 0;
    sleep_finish(INFSLP, !all);
    if (all)
      return;
  }
}

void sched_peg_curproc(struct cpu_info *ci) {
  struct proc *p = curproc;

  mtx_enter(&p->lock);
  p->pinned = ci;
  mtx_leave(&p->lock);
  for (;;) {
    struct cpu_info *c;

    spinlock_enter();
    c = curcpu();
    spinlock_exit();
    if (c == ci)
      break;
    yield(1);
  }
}

void sched_unpeg_curproc(void) {
  struct proc *p = curproc;

  mtx_enter(&p->lock);
  p->pinned = 0;
  mtx_leave(&p->lock);
}

// proctree_lock held
static int sleep_signal(struct proc *p) {
  struct process *pr = p->p_p;
  uint64_t ready;
  uint64_t handler;
  int sig;

  for (;;) {
    if (p->killed)
      return 1;
    ready = sig_pending_of(p) & ~p->sig_mask;
    if (pr->traced && (ready != 0 || (pr->ps_flags & PS_STOPPED))) {
      p->sig_restart = 1;
      return 1;
    }
    if (pr->ps_flags & PS_HELD) {
      stop_self(p);
      continue;
    }
    if (ready == 0)
      return 0;
    for (sig = 1; sig < FNU_SIG_COUNT; sig++)
      if (ready & FNU_SIGBIT(sig))
        break;
    handler = pr->sig_act[sig].handler;
    if (handler == (uint64_t)FNU_SIGACT_IGNORE ||
        (handler == (uint64_t)FNU_SIGACT_DEFAULT &&
         (pr->pid == 1 || sig_default_ignores(sig)))) {
      take(p, sig);
      continue;
    }
    if (handler == (uint64_t)FNU_SIGACT_DEFAULT && sig_stops(sig)) {
      take(p, sig);
      process_stop(pr, sig);
      continue;
    }
    p->sig_restart = 1;
    return 1;
  }
}

// no locks held
static int sleep_core(struct proc *p, int timo, int do_sleep) {
  int armed = 0, intr = 0, timedout, catch = p->slpcatch, b;

  while (do_sleep) {
    if (catch) {
      mtx_enter(&proctree_lock);
      intr = sleep_signal(p);
      mtx_leave(&proctree_lock);
      if (intr)
        break;
    }
    mtx_enter(&p->lock);
    if (p->woken || p->timedout) {
      mtx_leave(&p->lock);
      break;
    }
    if (catch && p->sigcheck) {
      p->sigcheck = 0;
      mtx_leave(&p->lock);
      continue;
    }
    if (!armed && (armed = timo > 0) != 0)
      callout_reset(&p->sleeptimer, timo, endtsleep, p);
    p->state = SLEEPING;
    __atomic_fetch_add(&p->p_p->nvcsw, 1, __ATOMIC_RELAXED);
    sched();
    mtx_leave(&p->lock);
  }
  b = (int)SLPQ(p->chan);
  mtx_enter(&slpque[b].lock);
  slpq_del(p);
  p->chan = NULL;
  p->waiting = 0;
  mtx_leave(&slpque[b].lock);
  mtx_enter(&p->lock);
  timedout = p->timedout;
  mtx_leave(&p->lock);
  if (armed && !timedout && !callout_stop_async(&p->sleeptimer))
    callout_stop(&p->sleeptimer);
  if (!intr && catch) {
    mtx_enter(&proctree_lock);
    intr = sleep_signal(p);
    mtx_leave(&proctree_lock);
  }
  mtx_enter(&p->lock);
  p->woken = 0;
  p->timedout = 0;
  mtx_leave(&p->lock);
  p->slpcatch = 0;
  return intr ? FNU_ERR_INTERRUPTED : timedout ? FNU_ERR_WOULDBLOCK : 0;
}

static int nsec_to_ticks(uint64_t nsecs) {
  uint64_t t;

  if (nsecs == INFSLP)
    return 0;
  t = nsecs / (1000000000ull / HZ) + (nsecs % (1000000000ull / HZ) != 0);
  if (t > 0x7fffffff)
    t = 0x7fffffff;
  return t == 0 ? 1 : (int)t;
}

void sleep_setup(const volatile void *ident, int prio, const char *wmesg) {
  struct proc *p = curproc;
  int b = (int)SLPQ(ident);

  if (p == 0)
    panic("sleep_setup");
  mtx_enter(&slpque[b].lock);
  p->chan = (void *)(uintptr_t)ident;
  p->wmesg = wmesg;
  p->slppri = (uint8_t)(prio & PRIMASK);
  p->slpcatch = (prio & PCATCH) != 0;
  p->waiting = 1;
  slpq_add(p);
  mtx_enter(&p->lock);
  p->woken = 0;
  mtx_leave(&p->lock);
  mtx_leave(&slpque[b].lock);
}

int sleep_finish(uint64_t nsecs, int do_sleep) {
  return sleep_core(curproc, nsec_to_ticks(nsecs), do_sleep);
}

// 0, EWOULDBLOCK or EINTR
int msleep(void *chan, struct mutex *lk, int pri, const char *wmesg, int timo) {
  struct proc *p = curproc;
  int error;

  if (p == 0)
    panic("sleep");
  if (lk == 0)
    panic("sleep without lk");
  sleep_setup(chan, pri, wmesg);
  mtx_leave(lk);
  error = sleep_finish(timo > 0 ? (uint64_t)timo * (1000000000ull / HZ) : INFSLP, 1);
  if ((pri & PNORELOCK) == 0)
    mtx_enter(lk);
  return error;
}

int tsleep(void *chan, int pri, const char *wmesg, int timo) {
  sleep_setup(chan, pri, wmesg);
  return sleep_finish(timo > 0 ? (uint64_t)timo * (1000000000ull / HZ) : INFSLP, 1);
}

// in nanoseconds; INFSLP for ever
int tsleep_nsec(void *chan, int pri, const char *wmesg, uint64_t nsecs) {
  sleep_setup(chan, pri, wmesg);
  return sleep_finish(nsecs, 1);
}

int msleep_nsec(void *chan, struct mutex *lk, int pri, const char *wmesg,
                uint64_t nsecs) {
  int timo = nsec_to_ticks(nsecs);

  return msleep(chan, lk, pri, wmesg, timo);
}

