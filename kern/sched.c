// SCHED
#include "defs.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "sched.h"
#include "x86.h"
#include "inc/abi.h"

struct schedq {
  struct mutex lock;
  struct proc *head[SCHED_NQS];
  struct proc *tail[SCHED_NQS];
  uint32_t bits;
  int nrun;
  long uload;
  int ucount;
  int core;
  int sibling;
};

static struct schedq *sq;
static int *corefirst, ncores, coresize;
static uint32_t loadavg[3];

static int rrinterval = (ESTCPUFREQ + 9) / 10;
static int weight1 = 30;   // stay on the cpu
static int weight2 = 180;  // near the waker
static int weight3 = 10;   // threads queued
static int weight4 = 120;  // a cpu running worse
static int weight6 = 0;    // the rover's margin
static int weight7 = -100; // an idle cpu's margin
static int features = 0x2f;
static int fast_resched = SCHED_PPQ / 2;
static int forkbias = 1;
static int pctcpu_decay = 10;

#define F_PULL 0x01
#define F_PUSH 0x08
#define F_ROVER 0x04

static int me(void) { return (int)(curcpu() - cpus); }

static int usable(int i) {
  return !cpus[i].park && (cpus[i].started || i == me());
}

static int allowed(struct proc *p, int i) {
  return p->pinned == 0 || p->pinned == &cpus[i];
}

uint8_t sched_priority(struct proc *p) {
  return p->usrpri < p->lentpri ? p->usrpri : p->lentpri;
}

// QUEUES: q->lock and p->lock held
static void rq_insert(struct schedq *q, struct proc *p) {
  int i = p->runpri / SCHED_PPQ;

  if (p->onrunq)
    panic("rq_insert: already queued");
  p->onrunq = 1;
  p->state = RUNNABLE;
  if (p->rrcount >= rrinterval) {
    p->rrcount = 0;
    p->rqnext = 0;
    p->rqprev = q->tail[i];
    if (q->tail[i] != 0)
      q->tail[i]->rqnext = p;
    else
      q->head[i] = p;
    q->tail[i] = p;
  } else {
    p->rqprev = 0;
    p->rqnext = q->head[i];
    if (q->head[i] != 0)
      q->head[i]->rqprev = p;
    else
      q->tail[i] = p;
    q->head[i] = p;
  }
  q->bits |= 1u << i;
  q->nrun++;
}

static void rq_remove(struct schedq *q, struct proc *p) {
  int i = p->runpri / SCHED_PPQ;

  if (p->rqprev != 0)
    p->rqprev->rqnext = p->rqnext;
  else
    q->head[i] = p->rqnext;
  if (p->rqnext != 0)
    p->rqnext->rqprev = p->rqprev;
  else
    q->tail[i] = p->rqprev;
  p->rqnext = p->rqprev = 0;
  p->onrunq = 0;
  if (q->head[i] == 0)
    q->bits &= ~(1u << i);
  q->nrun--;
}

// LOAD: q is sq[p->qcpu], q->lock held
static int lptouload(struct proc *p) {
  int u = (int)(p->estcpu / SCHED_NQS);

  return u - u * p->p_p->nice / (PRIO_MAX + 1);
}

static void load_add(struct schedq *q, struct proc *p) {
  if (!p->inload) {
    p->inload = 1;
    q->uload += p->uload;
    q->ucount++;
  }
}

static void load_del(struct schedq *q, struct proc *p) {
  if (p->inload) {
    p->inload = 0;
    q->uload -= p->uload;
    q->ucount--;
  }
}

// p->lock held
static void changeqcpu(struct proc *p, int to) {
  struct schedq *q = &sq[p->qcpu];

  if (p->qcpu == to)
    return;
  if (p->inload) {
    mtx_enter(&q->lock);
    load_del(q, p);
    mtx_leave(&q->lock);
  }
  p->qcpu = to;
}

static void enqueue(struct proc *p, int to) {
  struct schedq *q = &sq[to];

  changeqcpu(p, to);
  mtx_enter(&q->lock);
  load_add(q, p);
  rq_insert(q, p);
  mtx_leave(&q->lock);
}

static void preempt(int i, uint8_t pri) {
  struct cpu_info *c = &cpus[i];

  if ((c->upri & ~PPQMASK) <= (pri & ~PPQMASK))
    return;
  if ((c->upri & ~PPQMASK) <= (pri & ~PPQMASK) + fast_resched) {
    c->slicedown = 1;
    return;
  }
  c->resched = 1;
  if (i != me())
    sched_notify(i);
}

// after our spinlocks, else the woken thread is still held. пиздец
void sched_notify(int i) {
  struct cpu_info *c;

  spinlock_enter();
  c = curcpu();
  c->notify[i / 64] |= 1ull << (i % 64);
  c->notifying = 1;
  spinlock_exit();
}

// interrupts off, no spin lock held
void sched_notify_flush(struct cpu_info *c) {
  uint64_t m;
  int w, b;

  c->notifying = 0;
  for (w = 0; w < MAXCPU / 64; w++)
    for (m = c->notify[w], c->notify[w] = 0; m != 0; m &= m - 1) {
      b = __builtin_ctzll(m);
      lapic_notify(cpus[w * 64 + b].apicid);
    }
}

// PRIORITY: p->lock held. urgent: may take the cpu. aging isn't
static void reprioritize(struct proc *p, int urgent) {
  struct schedq *q = &sq[p->qcpu];
  int pri, u;

  pri = PUSER + (p->p_p->nice - PRIO_MIN) * NICE_PRIS / PRIO_RANGE +
        (int)(p->estcpu * EST_PRIS / ESTCPUMAX);
  if (pri > MAXPRI)
    pri = MAXPRI;
  if (p->kfn != 0)
    pri = PUSER;
  if ((p->usrpri ^ pri) & ~PPQMASK) {
    if (p->usrpri < pri)
      p->rrcount = 0;
    p->usrpri = (uint8_t)pri;
    if (p->onrunq && p->runpri >= PUSER &&
        (p->runpri & ~PPQMASK) != (sched_priority(p) & ~PPQMASK)) {
      mtx_enter(&q->lock);
      rq_remove(q, p);
      p->runpri = sched_priority(p);
      rq_insert(q, p);
      mtx_leave(&q->lock);
      if (urgent)
        preempt(p->qcpu, p->runpri);
    }
  } else
    p->usrpri = (uint8_t)pri;

  u = lptouload(p);
  if (p->inload) {
    mtx_enter(&q->lock);
    q->uload += u - p->uload;
    mtx_leave(&q->lock);
  }
  p->uload = u;
}

void sched_resetpriority(struct proc *p) { reprioritize(p, 1); }

// never more than one cpu
static void updatepcpu(struct proc *p, uint32_t cpticks, uint64_t ttl) {
  uint32_t acc = (uint32_t)(((cpticks < ttl ? cpticks : ttl) << FSHIFT) / ttl);

  if (ttl >= ESTCPUFREQ)
    p->pctcpu = acc;
  else
    p->pctcpu = (uint32_t)((acc * ttl + p->pctcpu * (ESTCPUFREQ - ttl)) / ESTCPUFREQ);
}

// %cpu window only
static void recalculate(struct proc *p) {
  uint64_t now = ticks, ttl;

  if (p->slptime > 1) {
    p->cpbase = now;
    p->cpticks = 0;
    return;
  }
  if (p->cpbase == now || (ttl = now - p->cpbase) < 4)
    return;
  updatepcpu(p, p->cpticks, ttl);
  p->cpbase += ttl;
  p->cpticks = 0;
}

// 2L seconds, in tenths. everyone on the same tick
void sched_decay(struct proc *p) {
  uint64_t steps, load;

  if (p->kfn != 0)
    return;
  load = (uint64_t)__atomic_load_n(&sq[p->qcpu].ucount, __ATOMIC_RELAXED);
  if (load < 1)
    load = 1;
  steps = 2 * load * ESTCPUSTEPS;
  p->estcpu = (uint32_t)((uint64_t)p->estcpu * (steps - 1) / steps);
  reprioritize(p, 0);
}

// CHOICE
static long cpu_load(int i, struct proc *p, uint8_t pri, int dd) {
  long load = sq[i].uload + sq[i].ucount * weight3;

  if ((cpus[i].upri & ~PPQMASK) > (pri & ~PPQMASK))
    load -= weight4;
  if (p->inload && i == dd)
    load -= p->uload + weight3;
  return load;
}

static long pairing(int leaf) {
  int loadav = (int)((loadavg[0] + FSCALE / 2) >> FSHIFT);

  if (!leaf)
    return -weight2;
  return loadav >= ncpus ? -weight2 : weight2;
}

static int choose_best(struct proc *p, uint8_t pri) {
  int dd = p->qcpu, wake = p->wakefrom, k, i, bestk = -1, best = -1, n;
  long load, lowest = 0;

  if (p->pinned != 0)
    return (int)(p->pinned - cpus);
  if (dd < 0 || dd >= ncpus || !usable(dd))
    dd = me();
  for (k = 0; k < ncores; k++) {
    load = 0;
    n = 0;
    for (i = corefirst[k]; i >= 0; i = sq[i].sibling)
      if (usable(i)) {
        load += cpu_load(i, p, pri, dd);
        n++;
      }
    if (n == 0)
      continue;
    load /= n;
    if (k == sq[dd].core)
      load -= weight1;
    if (wake >= 0 && wake < ncpus && sq[wake].core == k)
      load += pairing(coresize == 1);
    if (bestk < 0 || load < lowest || (load == lowest && k == sq[dd].core)) {
      lowest = load;
      bestk = k;
    }
  }
  if (bestk < 0)
    return dd;
  for (i = corefirst[bestk]; i >= 0; i = sq[i].sibling) {
    if (!usable(i))
      continue;
    load = cpu_load(i, p, pri, dd);
    if (i == dd)
      load -= weight1;
    if (coresize > 1 && i == wake)
      load += pairing(1);
    if (best < 0 || load < lowest || (load == lowest && i == dd)) {
      lowest = load;
      best = i;
    }
  }
  return best;
}

static int choose_worst(int dd, int forceit) {
  int k, i, bestk = -1, best = -1, n, runq, highrunq = 0;
  long load, highest = 0;

  for (k = 0; k < ncores; k++) {
    load = 0;
    n = runq = 0;
    for (i = corefirst[k]; i >= 0; i = sq[i].sibling)
      if (usable(i)) {
        load += sq[i].uload + sq[i].ucount * weight3;
        runq += sq[i].nrun;
        n++;
      }
    if (n == 0)
      continue;
    load /= n;
    if (!forceit && k == sq[dd].core)
      load += weight1 / 2;
    if (bestk < 0 ||
        (runq && (highest < load || (highest == load && k == sq[dd].core))) ||
        (runq && highrunq < runq + 1)) {
      highest = load;
      highrunq = runq;
      bestk = k;
    }
  }
  if (bestk < 0)
    return -1;
  for (i = corefirst[bestk]; i >= 0; i = sq[i].sibling) {
    if (!usable(i))
      continue;
    load = sq[i].uload + sq[i].ucount * weight3;
    if (!forceit && i == dd)
      load += weight1 / 2;
    if (best < 0 ||
        (sq[i].nrun && (highest < load || (highest == load && i == dd))) ||
        (sq[i].nrun && highrunq < sq[i].nrun + 1)) {
      highest = load;
      highrunq = sq[i].nrun;
      best = i;
    }
  }
  return best == dd ? -1 : best;
}

// off rq for cpu dd, locked; busy ones are skipped and *passed says so
static struct proc *chooseproc(int rq, int dd, int worst, int *passed) {
  struct schedq *q = &sq[rq];
  struct proc *p;
  uint32_t bits;
  int i;

  mtx_enter(&q->lock);
  for (bits = q->bits; bits != 0;) {
    i = worst ? 31 - __builtin_clz(bits) : __builtin_ctz(bits);
    bits &= ~(1u << i);
    for (p = worst ? q->tail[i] : q->head[i]; p != 0;
         p = worst ? p->rqprev : p->rqnext) {
      if (!allowed(p, dd))
        continue;
      if (!mtx_enter_try(&p->lock)) {
        *passed = 1;
        continue;
      }
      if (p->p_p->swaplock) {
        mtx_leave(&p->lock);
        continue;
      }
      rq_remove(q, p);
      if (rq != dd)
        load_del(q, p);
      mtx_leave(&q->lock);
      if (rq != dd) {
        p->qcpu = dd;
        mtx_enter(&sq[dd].lock);
        load_add(&sq[dd], p);
        mtx_leave(&sq[dd].lock);
      }
      return p;
    }
  }
  mtx_leave(&q->lock);
  return 0;
}

// CALLS: p->lock held unless said

void sched_setrunqueue(struct proc *p, uint8_t pri) {
  int to;

  p->runpri = pri;
  to = choose_best(p, pri);
  p->forked = 0;
  enqueue(p, to);
  preempt(to, pri);
}

void sched_requeue(struct proc *p, int voluntary) {
  int i = me(), to = i;

  if (voluntary)
    p->rrcount = rrinterval;
  p->runpri = sched_priority(p);
  if (p->pinned != 0)
    to = (int)(p->pinned - cpus);
  else if ((features & F_PUSH) && (uint)(ticks / 8 % (uint)ncpus) == (uint)i)
    to = choose_best(p, p->runpri);
  enqueue(p, to);
  if (to != i)
    preempt(to, p->runpri);
}

// next to run or 0; a skipped one leaves resched set
struct proc *sched_chooseproc(struct cpu_info *c) {
  int i = (int)(c - cpus), r, passed = 0;
  struct proc *p;

  // down before the scan
  __atomic_store_n(&c->resched, 0, __ATOMIC_SEQ_CST);
  p = chooseproc(i, i, 0, &passed);

  if (p == 0 && (features & F_PULL) && (r = choose_worst(i, 0)) >= 0 &&
      sq[i].uload + weight7 < sq[r].uload && cpus[r].proc != 0)
    p = chooseproc(r, i, 1, &passed);
  if (p != 0)
    c->upri = p->runpri;
  if (passed)
    __atomic_store_n(&c->resched, 1, __ATOMIC_SEQ_CST);
  return p;
}

void sched_release(struct cpu_info *c) { c->upri = NULLPRI; }

int sched_wanted(void) {
  int r;

  spinlock_enter();
  r = curcpu()->resched;
  spinlock_exit();
  return r;
}

// no locks held
void sched_clock(struct cpu_info *c, struct proc *p, int usermode) {
  int i = (int)(c - cpus), r, passed = 0;
  struct proc *np;
  uint8_t pri;

  if (p != 0) {
    mtx_enter(&p->lock);
    if (c->slicedown) {
      c->slicedown = 0;
      if (p->rrcount < rrinterval)
        p->rrcount = rrinterval - 1;
    }
    p->cpticks++;
    if (++p->rrcount >= rrinterval)
      c->resched = 1;
    if (p->kfn == 0) {
      p->estcpu = ESTCPULIM(p->estcpu + ESTCPUKICK);
      sched_resetpriority(p);
      if (usermode) {
        p->priority = sched_priority(p);
        c->upri = p->priority;
        // it got worse than one waiting here: no full slice for it
        if (sq[i].bits != 0 &&
            (uint)__builtin_ctz(sq[i].bits) * SCHED_PPQ + SCHED_PPQ + (uint)fast_resched <=
                (uint)(p->priority & ~PPQMASK))
          c->resched = 1;
      }
    }
    mtx_leave(&p->lock);
  }
  if ((features & F_ROVER) && (ticks & 7) == 0 &&
      (uint)(ticks / 8 % (uint)ncpus) == (uint)i &&
      (r = choose_worst(i, 1)) >= 0 && sq[i].uload + weight6 / 2 < sq[r].uload &&
      (np = chooseproc(r, i, 1, &passed)) != 0) {
    pri = np->runpri;
    mtx_enter(&sq[i].lock);
    rq_insert(&sq[i], np);
    mtx_leave(&sq[i].lock);
    mtx_leave(&np->lock);
    preempt(i, pri);
  }
}

// once a second
void sched_second(struct proc *p) {
  if (p->state == ZOMBIE || p->state == DEAD)
    return;
  if ((p->state == SLEEPING || p->state == STOPPED) && ++p->slptime == 1 && p->inload) {
    mtx_enter(&sq[p->qcpu].lock);
    load_del(&sq[p->qcpu], p);
    mtx_leave(&sq[p->qcpu].lock);
  }
  if (p->slptime <= 1)
    recalculate(p);
  else
    p->pctcpu = p->pctcpu * (uint32_t)(pctcpu_decay - 1) / (uint32_t)pctcpu_decay;
}

void sched_wake(struct proc *p) {
  if (p->slptime)
    recalculate(p);
  p->slptime = 0;
  p->wakefrom = me();
}

// no locks held
void sched_userret(struct proc *p) {
  struct cpu_info *c;
  int i;

  // user priority already
  if (p->priority >= PUSER)
    return;
  mtx_enter(&p->lock);
  p->priority = sched_priority(p);
  c = curcpu();
  c->upri = p->priority;
  i = (int)(c - cpus);
  // sleep priority ends here. a queued sleeper beats it
  if (sq[i].bits != 0) {
    uint best = (uint)__builtin_ctz(sq[i].bits) * SCHED_PPQ;

    if (best < PUSER && best + (uint)fast_resched < (uint)(p->priority & ~PPQMASK))
      c->resched = 1;
  }
  mtx_leave(&p->lock);
}

// no locks held
void sched_fork(struct proc *parent, struct proc *child) {
  uint32_t est;
  int qcpu;

  mtx_enter(&parent->lock);
  est = parent->estcpu;
  qcpu = parent->qcpu;
  parent->estcpu = ESTCPULIM(parent->estcpu + ESTCPUPPQ / 16);
  sched_resetpriority(parent);
  mtx_leave(&parent->lock);
  mtx_enter(&child->lock);
  child->estcpu = ESTCPULIM(est + ESTCPUPPQ * (uint32_t)forkbias);
  child->forked = 1;
  child->qcpu = qcpu;
  child->cpbase = ticks;
  child->cpticks = 0;
  child->rrcount = 0;
  child->inload = 0;
  child->uload = 0;
  sched_resetpriority(child);
  mtx_leave(&child->lock);
}

void sched_exit(struct proc *p) {
  struct schedq *q = &sq[p->qcpu];

  mtx_enter(&q->lock);
  load_del(q, p);
  mtx_leave(&q->lock);
}

void sched_lend(struct proc *p, uint8_t pri) {
  struct schedq *q = &sq[p->qcpu];

  if (!p->onrunq || p->runpri <= pri)
    return;
  mtx_enter(&q->lock);
  rq_remove(q, p);
  p->runpri = pri;
  rq_insert(q, p);
  mtx_leave(&q->lock);
  preempt(p->qcpu, pri);
}

// one thread at a time, as its lock allows; no locks held
void sched_park(struct cpu_info *c) {
  struct schedq *q = &sq[c - cpus];
  struct proc *p;
  int i, busy;

  do {
    busy = 0;
    mtx_enter(&q->lock);
    for (i = 0, p = 0; i < SCHED_NQS && p == 0; i++)
      for (p = q->head[i]; p != 0; p = p->rqnext) {
        if (p->pinned == c)
          continue;
        if (mtx_enter_try(&p->lock))
          break;
        busy = 1;
      }
    if (p != 0)
      rq_remove(q, p);
    mtx_leave(&q->lock);
    if (p != 0) {
      sched_setrunqueue(p, p->runpri);
      mtx_leave(&p->lock);
    }
  } while (p != 0 || busy);
  c->upri = NULLPRI;
}

void sched_loadavg_update(void) {
  static const uint32_t cexp[3] = {1884, 2014, 2037};
  uint32_t nrun = 0;
  int i;

  for (i = 0; i < ncpus; i++)
    nrun += (uint32_t)sq[i].nrun + (cpus[i].proc != 0);
  for (i = 0; i < 3; i++)
    loadavg[i] = (cexp[i] * loadavg[i] + nrun * FSCALE * (FSCALE - cexp[i])) >> FSHIFT;
}

void sched_loadavg(uint32_t out[3]) {
  int i;

  for (i = 0; i < 3; i++)
    out[i] = __atomic_load_n(&loadavg[i], __ATOMIC_RELAXED);
}

// TOPOLOGY
static int log2up(uint32_t n) {
  int s = 0;

  while ((1u << s) < n)
    s++;
  return s;
}

static int smt_shift(void) {
  uint32_t a, b, c, d, max, emax, logical, vendor;

  cpuid_count(0, 0, &max, &vendor, &c, &d);
  if (max >= 0xb) {
    cpuid_count(0xb, 0, &a, &b, &c, &d);
    if ((b & 0xffff) != 0 && ((c >> 8) & 0xff) == 1)
      return (int)(a & 0x1f);
  }
  cpuid_count(0x80000000, 0, &emax, &b, &c, &d);
  // AuthenticAMD, HygonGenuine
  if (emax >= 0x8000001e && (vendor == 0x68747541 || vendor == 0x6f677948)) {
    cpuid_count(0x8000001e, 0, &a, &b, &c, &d);
    return log2up(((b >> 8) & 0xff) + 1);
  }
  cpuid_count(1, 0, &a, &b, &c, &d);
  if (!(d & (1u << 28)))
    return 0;
  logical = (b >> 16) & 0xff;
  if (max >= 4) {
    cpuid_count(4, 0, &a, &b, &c, &d);
    logical /= ((a >> 26) & 0x3f) + 1;
  }
  return log2up(logical);
}

void sched_init(void) {
  int i, j, shift = smt_shift(), *last;

  sq = malloc(sizeof(*sq) * (size_t)ncpus, M_DEVBUF, M_WAITOK | M_ZERO);
  corefirst = malloc(sizeof(int) * (size_t)ncpus, M_DEVBUF, M_WAITOK);
  last = malloc(sizeof(int) * (size_t)ncpus, M_DEVBUF, M_WAITOK);
  for (i = 0; i < ncpus; i++) {
    cpus[i].upri = NULLPRI;
    mtx_init(&sq[i].lock, "schedq");
    sq[i].sibling = -1;
    for (j = 0; j < ncores; j++)
      if (cpus[corefirst[j]].apicid >> shift == cpus[i].apicid >> shift)
        break;
    if (j == ncores) {
      corefirst[ncores++] = i;
    } else
      sq[last[j]].sibling = i;
    last[j] = i;
    sq[i].core = j;
  }
  free(last, M_DEVBUF);
  coresize = (ncpus + ncores - 1) / ncores;
  printf("SCHED: %d cpus on %d cores\n", ncpus, ncores);

  sysctl_add_int("kern.sched.rrinterval", &rrinterval, FNU_CTLF_WRITE, 1, HZ);
  sysctl_add_int("kern.sched.weight1", &weight1, FNU_CTLF_WRITE, -1000, 1000);
  sysctl_add_int("kern.sched.weight2", &weight2, FNU_CTLF_WRITE, -1000, 1000);
  sysctl_add_int("kern.sched.weight3", &weight3, FNU_CTLF_WRITE, -1000, 1000);
  sysctl_add_int("kern.sched.weight4", &weight4, FNU_CTLF_WRITE, -1000, 1000);
  sysctl_add_int("kern.sched.weight6", &weight6, FNU_CTLF_WRITE, -1000, 1000);
  sysctl_add_int("kern.sched.weight7", &weight7, FNU_CTLF_WRITE, -1000, 1000);
  sysctl_add_int("kern.sched.features", &features, FNU_CTLF_WRITE, 0, 0xff);
}
