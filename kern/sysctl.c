// SYSCTL
#include "defs.h"
#include "malloc.h"
#include "inc/abi.h"
#include "inc/error.h"
#include "inc/product.h"
#include "inc/syscall.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"

#define SYSCTL_VALUE 256

struct sysctl_leaf {
  const char *name;
  int type;
  int flags;
  int *ip;
  int64_t *qp;
  char *sp;
  size_t size;
  int64_t (*get)(void);
  int (*gets)(char *out, size_t max);
  int64_t (*getarg)(void *arg);
  int (*getsarg)(void *arg, char *out, size_t max);
  uint64_t (*length)(void *arg);
  int (*readat)(void *arg, char *out, uint64_t off, int n);
  void *garg;
  int (*apply)(void *arg, int old);
  void *arg;
  void (*refresh)(void *arg); // the value read fresh
  void *rarg;
  int64_t lo, hi;
  uint64_t need;  // pledge to read it
};

static struct {
  struct mutex lock;
  struct sysctl_leaf *leaf;
  int n, cap;
} tree;

static void lock_with_room(void) {
  struct sysctl_leaf *grown, *old;
  int cap;

  for (;;) {
    mtx_enter(&tree.lock);
    if (tree.n < tree.cap)
      return;
    cap = tree.cap;
    mtx_leave(&tree.lock);
    grown = malloc((size_t)(cap ? cap * 2 : 128) * sizeof(*grown), M_DEVBUF,
                   M_WAITOK);
    old = grown;
    mtx_enter(&tree.lock);
    if (tree.cap == cap) {
      if (tree.n > 0)
        memmove(grown, tree.leaf, (size_t)tree.n * sizeof(*grown));
      old = tree.leaf;
      tree.leaf = grown;
      tree.cap = cap ? cap * 2 : 128;
    }
    mtx_leave(&tree.lock);
    if (old != 0)
      free(old, M_DEVBUF);
  }
}

static struct sysctl_leaf *add(const char *name, int type, int flags) {
  struct sysctl_leaf *l;

  l = &tree.leaf[tree.n];
  memset(l, 0, sizeof(*l));
  l->name = name;
  l->type = type;
  l->flags = flags;
  tree.n++;
  return l;
}

// one name once
int sysctl_add_int(const char *name, int *var, int flags, int lo, int hi) {
  struct sysctl_leaf *l;
  int i;

  lock_with_room();
  for (i = 0; i < tree.n; i++)
    if (strcmp(tree.leaf[i].name, name) == 0) {
      mtx_leave(&tree.lock);
      return -1;
    }
  l = add(name, FNU_CTL_INT, flags);
  l->ip = var;
  l->lo = lo;
  l->hi = hi;
  mtx_leave(&tree.lock);
  return 0;
}

int sysctl_on_write(const char *name, int (*fn)(void *arg, int old), void *arg) {
  int i;

  mtx_enter(&tree.lock);
  for (i = 0; i < tree.n; i++)
    if (strcmp(tree.leaf[i].name, name) == 0) {
      tree.leaf[i].apply = fn;
      tree.leaf[i].arg = arg;
      mtx_leave(&tree.lock);
      return 0;
    }
  mtx_leave(&tree.lock);
  return -1;
}

int sysctl_on_read(const char *name, void (*fn)(void *arg), void *arg) {
  int i;

  mtx_enter(&tree.lock);
  for (i = 0; i < tree.n; i++)
    if (strcmp(tree.leaf[i].name, name) == 0) {
      tree.leaf[i].refresh = fn;
      tree.leaf[i].rarg = arg;
      mtx_leave(&tree.lock);
      return 0;
    }
  mtx_leave(&tree.lock);
  return -1;
}

int sysctl_add_string(const char *name, char *var, size_t size, int flags) {
  struct sysctl_leaf *l;

  lock_with_room();
  l = add(name, FNU_CTL_STRING, flags);
  l->sp = var;
  l->size = size;
  mtx_leave(&tree.lock);
  return 0;
}

// one per device
int sysctl_add_quad_fn(const char *name, int64_t (*get)(void *), void *arg) {
  struct sysctl_leaf *l;

  lock_with_room();
  l = add(name, FNU_CTL_QUAD, 0);
  l->getarg = get;
  l->garg = arg;
  l->need = FNU_PLEDGE_VMINFO;
  mtx_leave(&tree.lock);
  return 0;
}

int sysctl_add_string_fn(const char *name, int (*gets)(void *, char *, size_t),
                         void *arg, uint64_t need) {
  struct sysctl_leaf *l;

  lock_with_room();
  l = add(name, FNU_CTL_STRING, 0);
  l->getsarg = gets;
  l->garg = arg;
  l->need = need;
  mtx_leave(&tree.lock);
  return 0;
}

int sysctl_add_opaque(const char *name, uint64_t (*length)(void *),
                      int (*readat)(void *, char *, uint64_t, int), void *arg,
                      int flags, uint64_t need) {
  struct sysctl_leaf *l;

  lock_with_room();
  l = add(name, FNU_CTL_OPAQUE, flags & ~FNU_CTLF_WRITE);
  l->length = length;
  l->readat = readat;
  l->garg = arg;
  l->need = need;
  mtx_leave(&tree.lock);
  return 0;
}

static void add_fn(const char *name, int type, int flags, int64_t (*get)(void),
                   int (*gets)(char *, size_t), uint64_t need) {
  struct sysctl_leaf *l;

  lock_with_room();
  l = add(name, type, flags);
  l->get = get;
  l->gets = gets;
  l->need = need;
  mtx_leave(&tree.lock);
}

static int find(const char *name, struct sysctl_leaf *out) {
  int i;

  mtx_enter(&tree.lock);
  for (i = 0; i < tree.n; i++)
    if (strcmp(tree.leaf[i].name, name) == 0) {
      *out = tree.leaf[i];
      mtx_leave(&tree.lock);
      return 0;
    }
  mtx_leave(&tree.lock);
  return -1;
}

static int value_of(const struct sysctl_leaf *l, char *buf, size_t *len) {
  int64_t q;

  if (l->refresh != 0)
    l->refresh(l->rarg);
  switch (l->type) {
  case FNU_CTL_INT:
    q = l->ip != 0 ? *l->ip : l->get != 0 ? l->get() : 0;
    *(int32_t *)buf = (int32_t)q;
    *len = sizeof(int32_t);
    return 0;
  case FNU_CTL_QUAD:
    q = l->qp != 0 ? *l->qp
        : l->getarg != 0 ? l->getarg(l->garg)
        : l->get != 0 ? l->get() : 0;
    memmove(buf, &q, sizeof(q));
    *len = sizeof(q);
    return 0;
  case FNU_CTL_STRING:
    buf[0] = 0;
    if (l->getsarg != 0)
      l->getsarg(l->garg, buf, SYSCTL_VALUE);
    else if (l->gets != 0)
      l->gets(buf, SYSCTL_VALUE);
    else
      strlcpy(buf, l->sp, SYSCTL_VALUE);
    buf[SYSCTL_VALUE - 1] = 0;
    *len = strlen(buf) + 1;
    return 0;
  }
  return FNU_ERR_INVALID;
}

static int store(struct sysctl_leaf *l, const char *buf, size_t len) {
  int64_t v;
  int error = 0, old = 0;

  switch (l->type) {
  case FNU_CTL_INT:
    if (len != sizeof(int32_t) || l->ip == 0)
      return FNU_ERR_INVALID;
    v = *(const int32_t *)buf;
    if (l->hi > l->lo && (v < l->lo || v > l->hi))
      return FNU_ERR_INVALID;
    old = *l->ip;
    *l->ip = (int)v;
    break;
  case FNU_CTL_QUAD:
    if (len != sizeof(int64_t) || l->qp == 0)
      return FNU_ERR_INVALID;
    memmove(&v, buf, sizeof(v));
    if (l->hi > l->lo && (v < l->lo || v > l->hi))
      return FNU_ERR_INVALID;
    *l->qp = v;
    break;
  case FNU_CTL_STRING:
    if (len > 0 && buf[len - 1] == 0)
      len--;
    if (l->sp == 0 || len >= l->size)
      return FNU_ERR_INVALID;
    for (v = 0; v < (int64_t)len; v++)
      if (buf[v] == 0)
        return FNU_ERR_INVALID;
    memmove(l->sp, buf, len);
    l->sp[len] = 0;
    break;
  default:
    return FNU_ERR_INVALID;
  }
  if (l->apply != 0 && (error = l->apply(l->arg, old)) != 0 && l->ip != 0)
    *l->ip = old;
  return error;
}

static int opaque_out(struct proc *p, const struct sysctl_leaf *l, uint64_t oldp,
                      uint64_t oldlenp, uint64_t oldlen) {
  uint64_t len = l->length(l->garg), off = 0;
  char *page;
  int n, error = 0;

  if (oldp == 0)
    return copyout(p->p_p->pgdir, oldlenp, &len, sizeof(len)) < 0 ? FNU_ERR_FAULT : 0;
  if ((page = page_alloc()) == 0)
    return FNU_ERR_NOMEMORY;
  while (off < len && off < oldlen) {
    n = (int)(len - off < oldlen - off ? len - off : oldlen - off);
    if (n > PGSIZE)
      n = PGSIZE;
    if ((n = l->readat(l->garg, page, off, n)) <= 0)
      break;
    if (copyout(p->p_p->pgdir, oldp + off, page, (size_t)n) < 0) {
      error = FNU_ERR_FAULT;
      break;
    }
    off += (uint64_t)n;
  }
  page_free(page);
  if (error == 0 && off < len && off == oldlen)
    error = FNU_ERR_NOMEMORY;
  if (error != FNU_ERR_FAULT &&
      copyout(p->p_p->pgdir, oldlenp, error != 0 ? &len : &off, sizeof(off)) < 0)
    error = FNU_ERR_FAULT;
  return error;
}

int sys_sysctl(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct sysctl_leaf l;
  char value[SYSCTL_VALUE], fresh[SYSCTL_VALUE];
  uint64_t oldp = uap[1], oldlenp = uap[2], newp = uap[3], newlen = uap[4];
  uint64_t oldlen = 0;
  char name[FNU_CTL_NAMELEN];
  size_t len = 0;
  int error;

  if ((error = copyinstr(p->p_p->pgdir, name, (uintptr_t)uap[0], sizeof(name),
                         0)) != 0)
    return error;
  if (find(name, &l) < 0)
    return FNU_ERR_NOTFOUND;
  if (p->p_p->pledged && !(l.flags & FNU_CTLF_STDIO) && !(p->p_p->pledge & l.need)) {
    pledge_fail(p, SYS_sysctl);
    return FNU_ERR_NOTPERMITTED;
  }
  if ((l.flags & FNU_CTLF_PRIVATE) && p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if (l.type == FNU_CTL_OPAQUE) {
    if (newp != 0)
      return FNU_ERR_NOTPERMITTED;
    if (oldp != 0 && oldlenp == 0)
      return FNU_ERR_INVALID;
    if (oldlenp == 0)
      return 0;
    if (copyin(p->p_p->pgdir, &oldlen, oldlenp, sizeof(oldlen)) < 0)
      return FNU_ERR_FAULT;
    return opaque_out(p, &l, oldp, oldlenp, oldlen);
  }
  if (newp != 0) {
    if (!(l.flags & FNU_CTLF_WRITE) || p->p_p->uid != 0 || p->p_p->pledged)
      return FNU_ERR_NOTPERMITTED;
    if ((newlen == 0 && l.type != FNU_CTL_STRING) || newlen > sizeof(fresh))
      return FNU_ERR_INVALID;
    if (copyin(p->p_p->pgdir, fresh, newp, (size_t)newlen) < 0)
      return FNU_ERR_FAULT;
  }
  if (oldlenp != 0 && copyin(p->p_p->pgdir, &oldlen, oldlenp, sizeof(oldlen)) < 0)
    return FNU_ERR_FAULT;
  if ((error = value_of(&l, value, &len)) != 0)
    return error;

  if (oldp != 0 || oldlenp != 0) {
    uint64_t wanted = len;
    int short_of_room = oldp != 0 && oldlen < len;

    if (oldp != 0) {
      size_t n = oldlen < len ? (size_t)oldlen : len;

      if (oldlenp == 0)
        return FNU_ERR_INVALID;
      if (n > 0 && copyout(p->p_p->pgdir, oldp, value, n) < 0)
        return FNU_ERR_FAULT;
    }
    if (oldlenp != 0 && copyout(p->p_p->pgdir, oldlenp, &wanted, sizeof(wanted)) < 0)
      return FNU_ERR_FAULT;
    if (short_of_room)
      return FNU_ERR_NOMEMORY;
  }
  if (newp != 0) {
    error = store(&l, fresh, (size_t)newlen);
    if (error != 0)
      return error;
  }
  return 0;
}

int sys_sysctlnode(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_sysctl_info info;
  uint64_t dst;
  int index;

  index = (int)uap[0];
  dst = uap[1];
  mtx_enter(&tree.lock);
  if (index < 0 || index >= tree.n) {
    mtx_leave(&tree.lock);
    return FNU_ERR_NOTFOUND;
  }
  memset(&info, 0, sizeof(info));
  strlcpy(info.name, tree.leaf[index].name, sizeof(info.name));
  info.type = (uint32_t)tree.leaf[index].type;
  info.flags = (uint32_t)tree.leaf[index].flags;
  mtx_leave(&tree.lock);
  return copyout(p->p_p->pgdir, (uintptr_t)dst, &info, sizeof(info)) < 0 ? FNU_ERR_FAULT : 0;
}

// WHAT THE KERNEL ANSWERS ITSELF

char kern_hostname[256];

static int sysctl_str(char *o, size_t n, const char *s) {
  strlcpy(o, s, n);
  return (int)strlen(o);
}

static int s_ostype(char *o, size_t n) { return sysctl_str(o, n, "OpenProninx"); }
static int s_osrelease(char *o, size_t n) { return sysctl_str(o, n, FNU_PRODUCT_VERSION); }
static int s_version(char *o, size_t n) {
  return sysctl_str(o, n, FNU_PRODUCT_NAME " " FNU_PRODUCT_VERSION " (" FNU_PRODUCT_VENDOR ")");
}
static int s_fnuversion(char *o, size_t n) { return sysctl_str(o, n, FNU_VERSION); }
static int s_machine(char *o, size_t n) { return sysctl_str(o, n, "amd64"); }
static int s_model(char *o, size_t n) {
  struct fnu_hwinfo hw;

  hwinfo_fill(&hw);
  return sysctl_str(o, n, hw.cpu_brand);
}
static int s_vendor(char *o, size_t n) {
  struct fnu_hwinfo hw;

  hwinfo_fill(&hw);
  return sysctl_str(o, n, hw.cpu_vendor);
}

static int64_t q_hz(void) { return HZ; }
static int64_t q_argmax(void) { return NCARGS; }
static int64_t q_nprocs(void) { return get_nprocs(); }
static int64_t q_boottime(void) {
  struct fnu_timespec now, up;

  time_now(&now);
  uptime_now(&up);
  return now.sec - up.sec - (now.nsec < up.nsec);
}
static int64_t q_ncpu(void) { return ncpus; }
static int64_t q_physmem(void) { return (int64_t)get_total_ram(); }
static int64_t q_freemem(void) { return (int64_t)get_free_ram(); }
static int64_t q_pagesize(void) { return PGSIZE; }
static int64_t q_byteorder(void) { return 1234; }
static int64_t q_cpufamily(void) {
  struct fnu_hwinfo hw;

  hwinfo_fill(&hw);
  return hw.cpu_family;
}
static int64_t q_cpumodel(void) {
  struct fnu_hwinfo hw;

  hwinfo_fill(&hw);
  return hw.cpu_model;
}
static int64_t q_cpustepping(void) {
  struct fnu_hwinfo hw;

  hwinfo_fill(&hw);
  return hw.cpu_stepping;
}
static int64_t q_swaptotal(void) {
  uint64_t total, used, outs, ins;

  swap_stats(&total, &used, &outs, &ins);
  return (int64_t)total;
}
static int64_t q_swapused(void) {
  uint64_t total, used, outs, ins;

  swap_stats(&total, &used, &outs, &ins);
  return (int64_t)used;
}
static int64_t q_swapouts(void) {
  uint64_t total, used, outs, ins;

  swap_stats(&total, &used, &outs, &ins);
  return (int64_t)outs;
}
static int64_t q_swapins(void) {
  uint64_t total, used, outs, ins;

  swap_stats(&total, &used, &outs, &ins);
  return (int64_t)ins;
}
static int64_t q_freemin(void) {
  int min, targ;

  pageout_targets(&min, &targ);
  return (int64_t)min * PGSIZE;
}
static int64_t q_freetarg(void) {
  int min, targ;

  pageout_targets(&min, &targ);
  return (int64_t)targ * PGSIZE;
}
static int64_t q_kmem(void) { return malloc_inuse(); }

// cp_times: busy, idle per cpu, two uint64 each
static uint64_t cp_times_length(void *arg __unused) {
  return (uint64_t)ncpus * 2 * sizeof(uint64_t);
}

static int cp_times_readat(void *arg, char *dst, uint64_t off, int n) {
  uint64_t v[2], len = cp_times_length(arg);
  int done = 0, c, k;

  while (done < n && off < len) {
    c = (int)(off / sizeof(v));
    v[0] = cpus[c].ticks_busy;
    v[1] = cpus[c].ticks_idle;
    k = (int)(sizeof(v) - off % sizeof(v));
    if (k > n - done)
      k = n - done;
    memmove(dst + done, (char *)v + off % sizeof(v), (size_t)k);
    done += k;
    off += (uint64_t)k;
  }
  return done;
}
static int64_t q_lends(void) { return sleeplock_lends(); }
static int64_t q_nbigpipes(void) { return pipe_nbig(); }
static int64_t q_maxbigpipes(void) { return pipe_maxbig(); }

static char malloc_options[FNU_CTL_MALLOC_MAX];

void sysctl_init(void) {
  const int S = FNU_CTLF_STDIO;

  mtx_init(&tree.lock, "sysctl");
  add_fn("kern.ostype", FNU_CTL_STRING, S, 0, s_ostype, 0);
  add_fn("kern.osrelease", FNU_CTL_STRING, S, 0, s_osrelease, 0);
  add_fn("kern.version", FNU_CTL_STRING, S, 0, s_version, 0);
  add_fn("kern.fnuversion", FNU_CTL_STRING, S, 0, s_fnuversion, 0);
  sysctl_add_string("kern.hostname", kern_hostname, sizeof(kern_hostname),
                    S | FNU_CTLF_WRITE);
  add_fn("kern.boottime", FNU_CTL_QUAD, S, q_boottime, 0, 0);
  add_fn("kern.hz", FNU_CTL_INT, S, q_hz, 0, 0);
  add_fn("kern.nprocs", FNU_CTL_INT, 0, q_nprocs, 0, FNU_PLEDGE_PS);
  add_fn("kern.argmax", FNU_CTL_INT, S, q_argmax, 0, 0);
  sysctl_add_string("kern.malloc_options", malloc_options, sizeof(malloc_options),
                    S | FNU_CTLF_WRITE);
  add_fn("vm.malloc.inuse", FNU_CTL_QUAD, 0, q_kmem, 0, FNU_PLEDGE_VMINFO);
  add_fn("kern.sleeplock.lends", FNU_CTL_INT, 0, q_lends, 0, FNU_PLEDGE_VMINFO);
  add_fn("kern.pipe.nbigpipes", FNU_CTL_INT, 0, q_nbigpipes, 0, FNU_PLEDGE_VMINFO);
  add_fn("kern.pipe.maxbigpipes", FNU_CTL_INT, 0, q_maxbigpipes, 0, FNU_PLEDGE_VMINFO);

  add_fn("hw.machine", FNU_CTL_STRING, S, 0, s_machine, 0);
  add_fn("hw.model", FNU_CTL_STRING, S, 0, s_model, 0);
  add_fn("hw.cpu.vendor", FNU_CTL_STRING, S, 0, s_vendor, 0);
  add_fn("hw.ncpu", FNU_CTL_INT, S, q_ncpu, 0, 0);
  add_fn("hw.byteorder", FNU_CTL_INT, S, q_byteorder, 0, 0);
  add_fn("hw.pagesize", FNU_CTL_INT, S, q_pagesize, 0, 0);
  add_fn("hw.physmem", FNU_CTL_QUAD, S, q_physmem, 0, 0);
  add_fn("hw.cpu.family", FNU_CTL_INT, S, q_cpufamily, 0, 0);
  add_fn("hw.cpu.model", FNU_CTL_INT, S, q_cpumodel, 0, 0);
  add_fn("hw.cpu.stepping", FNU_CTL_INT, S, q_cpustepping, 0, 0);

  add_fn("vm.freemem", FNU_CTL_QUAD, 0, q_freemem, 0, FNU_PLEDGE_VMINFO);
  add_fn("vm.swap.total", FNU_CTL_QUAD, 0, q_swaptotal, 0, FNU_PLEDGE_VMINFO);
  add_fn("vm.swap.used", FNU_CTL_QUAD, 0, q_swapused, 0, FNU_PLEDGE_VMINFO);
  sysctl_add_opaque("kern.cp_times", cp_times_length, cp_times_readat, 0, 0, 0);
  add_fn("vm.freemin", FNU_CTL_QUAD, 0, q_freemin, 0, FNU_PLEDGE_VMINFO);
  add_fn("vm.freetarg", FNU_CTL_QUAD, 0, q_freetarg, 0, FNU_PLEDGE_VMINFO);
  add_fn("vm.swap.outs", FNU_CTL_QUAD, 0, q_swapouts, 0, FNU_PLEDGE_VMINFO);
  add_fn("vm.swap.ins", FNU_CTL_QUAD, 0, q_swapins, 0, FNU_PLEDGE_VMINFO);
}
