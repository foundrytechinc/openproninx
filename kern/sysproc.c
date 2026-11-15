#include "buf.h"
#include "defs.h"
#include "memlayout.h"
#include "file.h"
#include "fs.h"
#include "net/if.h"
#include "net/inet.h"
#include "inc/abi.h"
#include "inc/stat.h"
#include "inc/signal.h"
#include "inc/syscall.h"
#include "sched.h"
#include "malloc.h"
#include "param.h"
#include "proc.h"
#include "pathname.h"
#include "ufs2.h"
#include "unveil.h"
#include "vfs.h"
#include "blockdev.h"
#include "x86.h"

int sys_info(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_info inf;
  uint64_t dst;

  dst = uap[0];

  memset(&inf, 0, sizeof(inf));
  inf.uptime = ticks;
  inf.total_ram = get_total_ram();
  inf.free_ram = get_free_ram();
  swap_stats(&inf.swap_total, &inf.swap_used, &inf.swap_outs, &inf.swap_ins);
  inf.nprocs = (uint32_t)get_nprocs();
  sched_loadavg(inf.loadavg);
  if (copyout(p->p_p->pgdir, (uintptr_t)dst, &inf, sizeof(inf)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

static int request_shutdown(struct proc *p, int sig) {
  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  return kill(1, sig);
}

int sys_reboot(struct proc *p, const register_t *uap __unused, register_t *retval __unused) { return request_shutdown(p, FNU_SIG_INTERRUPT); }

int sys_poweroff(struct proc *p, const register_t *uap __unused, register_t *retval __unused) { return request_shutdown(p, FNU_SIG_USER2); }

int sys_halt(struct proc *p, const register_t *uap, register_t *retval __unused) {
  int howto;

  howto = (int)uap[0];
  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if (p->p_p->pid != 1 && !(howto & FNU_HALT_FORCE))
    return FNU_ERR_NOTPERMITTED;
  system_halt(howto);
  return FNU_ERR_IO;
}

int sys_procinfo(struct proc *p, const register_t *uap, register_t *retval) {
  struct fnu_procinfo *buf;
  uint64_t dst;
  int capacity, room, n, error = 0;

  dst = uap[0];
  capacity = (int)uap[1];
  if (capacity < 0)
    return FNU_ERR_INVALID;
  if (capacity == 0) {
    *retval = procinfo_all(0, 0);
    return 0;
  }
  if ((room = (int)get_nprocs()) > capacity)
    room = capacity;
  buf = malloc((size_t)room * sizeof(*buf), M_TEMP, M_WAITOK);
  if ((n = procinfo_all(buf, room)) > room)
    n = room;
  if (copyout(p->p_p->pgdir, (uintptr_t)dst, buf, (size_t)n * sizeof(*buf)) < 0)
    error = FNU_ERR_FAULT;
  free(buf, M_TEMP);
  *retval = n;
  return error;
}

int sys_fork(struct proc *p __unused, const register_t *uap __unused, register_t *retval) {
  pid_t pid = fork();

  if (pid < 0)
    return (int)-pid;
  *retval = pid;
  yield(1);
  return 0;
}

int sys_exit(struct proc *p, const register_t *uap, register_t *retval __unused) {
  console_flush_if_dirty();
  p->p_p->xexit = (int)uap[0] & 0xff;
  p->p_p->xsig = 0;
  exit();
  return 0;
}

static int wait_common(struct proc *p, pid_t pid, uint64_t uout, int flags,
                       register_t *retval) {
  struct fnu_exitinfo info;
  struct fnu_exitinfo *out = uout != 0 ? &info : 0;
  pid_t got;

  memset(&info, 0, sizeof(info));
  got = waitpid(pid, out, flags);
  if (got < 0)
    return (int)-got;
  if (got > 0 && out != 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)uout, &info, sizeof(info)) < 0)
    return FNU_ERR_FAULT;
  *retval = got;
  return 0;
}

int sys_wait(struct proc *p, const register_t *uap, register_t *retval) {
  int flags = (int)uap[2];

  console_flush_if_dirty();
  if (flags & ~(FNU_WAIT_NOHANG | FNU_WAIT_STOPPED | FNU_WAIT_CONTINUED))
    return FNU_ERR_INVALID;
  return wait_common(p, (int)uap[0], uap[1], flags, retval);
}

int sys_signal(struct proc *p __unused, const register_t *uap, register_t *retval __unused) {
  return kill_perm((int)uap[0], (int)uap[1]);
}

int sys_sigtake(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_siginfo si;
  int error;

  if ((error = signal_take((uint64_t)uap[0], &si, (int)uap[2])) != 0)
    return error;
  if (copyout(p->p_p->pgdir, (uintptr_t)uap[1], &si, sizeof(si)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

// THREADS
int sys_thread_start(struct proc *p, const register_t *uap, register_t *retval) {
  struct fnu_thread_start st;
  pid_t tid;
  int error;

  if (copyin(p->p_p->pgdir, &st, (uintptr_t)uap[0], sizeof(st)) < 0)
    return FNU_ERR_FAULT;
  if ((error = thread_fork(&st, &tid)) != 0)
    return error;
  *retval = tid;
  return 0;
}

int sys_thread_exit(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uint64_t cleared = uap[0];
  uint32_t zero = 0;

  if (cleared != 0 && (cleared & 3) == 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)cleared, &zero, sizeof(zero)) == 0)
    futex_wake(p, (uintptr_t)cleared, ~0u, 0);
  thread_exit();
}

int sys_thread_tcb(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uint64_t tcb;

  tcb = uap[0];
  if (tcb >= (1ull << 47))
    return FNU_ERR_INVALID;
  p->tcb = tcb;
  wrmsr(MSR_FS_BASE, tcb);
  return 0;
}

int sys_thread_yield(struct proc *p __unused, const register_t *uap __unused, register_t *retval __unused) {
  yield(1);
  return 0;
}

int sys_thread_id(struct proc *p, const register_t *uap __unused, register_t *retval) {
  *retval = p->tid;
  return 0;
}

int sys_sigpause(struct proc *p __unused, const register_t *uap, register_t *retval __unused) {
  return kern_sigsuspend((uint64_t)uap[0]);
}

int sys_sigact(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_sigact act, old;
  uint64_t uact = uap[1], uold = uap[2];
  int sig = (int)uap[0], error;

  if (uact && copyin(p->p_p->pgdir, &act, (uintptr_t)uact, sizeof(act)) < 0)
    return FNU_ERR_FAULT;
  if ((error = kern_sigact(sig, uact ? &act : 0, uold ? &old : 0)) != 0)
    return error;
  if (uold && copyout(p->p_p->pgdir, (uintptr_t)uold, &old, sizeof old) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

int sys_sigmask(struct proc *p __unused, const register_t *uap, register_t *retval) {
  *retval = (register_t)kern_sigmask((uint64_t)uap[0], (uint64_t)uap[1]);
  return 0;
}

int sys_sigraised(struct proc *p, const register_t *uap __unused, register_t *retval) {
  *retval = (register_t)sig_pending_of(p);
  return 0;
}

int sys_sigreturn(struct proc *p __unused, const register_t *uap, register_t *retval __unused) {
  uint64_t scp;

  scp = uap[0];
  return kern_sigreturn((uintptr_t)scp);
}

int sys_procids(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_procids ids;
  int error;

  memset(&ids, 0, sizeof(ids));
  if ((error = proc_ids((int)uap[0], &ids)) != 0)
    return error;
  return copyout(p->p_p->pgdir, (uintptr_t)uap[1], &ids, sizeof(ids)) < 0
             ? FNU_ERR_FAULT
             : 0;
}

int sys_map(struct proc *p, const register_t *uap, register_t *retval) {
  struct file *f = 0;
  struct inode *ip = 0;
  uintptr_t addr, want;
  uint64_t len, offset;
  int prot, flags, fd, error;

  addr = uap[0];
  len = uap[1];
  prot = (int)uap[2];
  flags = (int)uap[3];
  fd = (int)uap[4];
  offset = uap[5];
  // bounded by the address space and RLIMIT_DATA
  if (len == 0)
    return FNU_ERR_INVALID;
  if (len > USERTOP)
    return FNU_ERR_NOMEMORY;
  if (prot & ~(FNU_MEM_READ | FNU_MEM_WRITE | FNU_MEM_EXEC))
    return FNU_ERR_INVALID;
  if ((flags & (FNU_MEM_SHARED | FNU_MEM_PRIVATE)) == 0 ||
      (flags & (FNU_MEM_SHARED | FNU_MEM_PRIVATE)) == (FNU_MEM_SHARED | FNU_MEM_PRIVATE))
    return FNU_ERR_INVALID;
  if (!(flags & FNU_MEM_ANON)) {
    if ((f = fd_getfile(p->p_p, fd)) == 0)
      return FNU_ERR_BADHANDLE;
    if (f->type != FD_INODE || f->ip == 0 || f->ip->type != FNU_TYPE_FILE)
      error = FNU_ERR_WRONGDEVICE;
    else if (!f->readable ||
             ((flags & FNU_MEM_SHARED) && (prot & FNU_MEM_WRITE) && !f->writable))
      error = FNU_ERR_DENIED;
    else
      error = 0;
    if (error != 0) {
      FRELE(f);
      return error;
    }
    ip = f->ip;
  } else {
    offset = 0;
  }
  error = vm_mmap(p, addr, (size_t)len, prot, flags, ip, offset, &want);
  if (f != 0)
    FRELE(f);
  if (error != 0)
    return error;
  *retval = (register_t)want;
  return 0;
}

int sys_unmap(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uintptr_t addr;
  uint64_t len;

  addr = uap[0];
  len = uap[1];
  if (len == 0 || (addr & (PGSIZE - 1)) != 0)
    return FNU_ERR_INVALID;
  len = PGROUNDUP(len);
  if (!vm_holds(p, addr))
    return FNU_ERR_INVALID;
  return vm_munmap(p, addr, (size_t)len);
}

int sys_memsync(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uintptr_t addr;
  uint64_t len;
  int flags;

  addr = uap[0];
  len = uap[1];
  flags = (int)uap[2];
  if ((addr & (PGSIZE - 1)) != 0 || (flags & ~(FNU_MEMSYNC_WAIT | FNU_MEMSYNC_START)) ||
      (flags & (FNU_MEMSYNC_WAIT | FNU_MEMSYNC_START)) == (FNU_MEMSYNC_WAIT | FNU_MEMSYNC_START))
    return FNU_ERR_INVALID;
  return vm_msync(p, addr, (size_t)PGROUNDUP(len));
}

int sys_memuse(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uintptr_t addr;
  uint64_t len;
  int advice;

  addr = uap[0];
  len = uap[1];
  advice = (int)uap[2];
  if ((addr & (PGSIZE - 1)) != 0)
    return FNU_ERR_INVALID;
  if (len == 0)
    return 0;
  return vm_madvise(p, addr, (size_t)PGROUNDUP(len), advice);
}

int sys_protect(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uintptr_t addr;
  uint64_t len;
  int prot;

  addr = uap[0];
  len = uap[1];
  prot = (int)uap[2];
  if ((addr & (PGSIZE - 1)) != 0 || (prot & ~(FNU_MEM_READ | FNU_MEM_WRITE | FNU_MEM_EXEC)) != 0)
    return FNU_ERR_INVALID;
  if (len == 0)
    return 0;
  len = PGROUNDUP(len);
  if (!vm_holds(p, addr))
    return FNU_ERR_NOMEMORY;
  return vm_mprotect(p, addr, (size_t)len, prot);
}

int sys_swapon(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct pathname path;
  struct inode *ip;
  int r;

  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if ((r = pn_get((uintptr_t)uap[0], &path)) != 0)
    return r;
  r = namei_at(0, path.pn_buf, UV_READ | UV_WRITE, &ip);
  pn_free(&path);
  if (r != 0)
    return r;
  ilock(ip);
  r = swapon(ip);
  iunlockput(ip);
  return r;
}

int sys_swapoff(struct proc *p, const register_t *uap __unused, register_t *retval __unused) {
  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  return swapoff();
}

int sys_altstack(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_altstack in, out;
  uint64_t uin, uout;
  int error;

  uin = uap[0];
  uout = uap[1];
  if (uin != 0 &&
      (error = pledge_need(p, FNU_PLEDGE_PROC | FNU_PLEDGE_ID, SYS_limit)) != 0)
    return error;
  if (uin != 0 && copyin(p->p_p->pgdir, &in, (uintptr_t)uin, sizeof(in)) < 0)
    return FNU_ERR_FAULT;
  memset(&out, 0, sizeof(out));
  if ((error = kern_sigstack(uin ? &in : 0, uout ? &out : 0)) != 0)
    return error;
  if (uout != 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)uout, &out, sizeof(out)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

// fnu_ident_get: who the caller is
int sys_ident_get(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct process *pr = p->p_p;
  struct fnu_ident id;

  memset(&id, 0, sizeof(id));
  id.uid = pr->ruid;
  id.euid = pr->uid;
  id.svuid = pr->svuid;
  id.gid = pr->rgid;
  id.egid = pr->gid;
  id.svgid = pr->svgid;
  id.ngroups = (uint32)pr->ngroups;
  memmove(id.groups, pr->groups, (size_t)pr->ngroups * sizeof(id.groups[0]));
  id.changed = (uint32)pr->sugid;
  strlcpy(id.login, pr->login, sizeof(id.login));
  return copyout(pr->pgdir, (uintptr_t)uap[0], &id, sizeof(id)) < 0 ? FNU_ERR_FAULT
                                                                      : 0;
}

static int among(uint v, uint r, uint e, uint s) { return v == r || v == e || v == s; }

#define IDENT_IDS                                                              \
  (FNU_IDENT_UID | FNU_IDENT_GID | FNU_IDENT_RUID | FNU_IDENT_EUID |           \
   FNU_IDENT_SVUID | FNU_IDENT_RGID | FNU_IDENT_EGID | FNU_IDENT_SVGID)

int sys_ident_set(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct process *pr = p->p_p;
  struct fnu_ident id;
  int what = (int)uap[1], root = pr->uid == 0;
  uint ruid = pr->ruid, euid = pr->uid, svuid = pr->svuid;
  uint rgid = pr->rgid, egid = pr->gid, svgid = pr->svgid;

  if (what & ~(IDENT_IDS | FNU_IDENT_GROUPS | FNU_IDENT_LOGIN))
    return FNU_ERR_INVALID;
  if (copyin(pr->pgdir, &id, (uintptr_t)uap[0], sizeof(id)) < 0)
    return FNU_ERR_FAULT;
  if ((what & FNU_IDENT_GROUPS) && id.ngroups > NGROUPS)
    return FNU_ERR_INVALID;
  if (what & FNU_IDENT_LOGIN) {
    id.login[sizeof(id.login) - 1] = 0;
    if (strlen(id.login) >= MAXLOGNAME)
      return FNU_ERR_INVALID;
  }
  if (!root &&
      ((what & (FNU_IDENT_GROUPS | FNU_IDENT_LOGIN)) ||
       ((what & FNU_IDENT_UID) && !among(id.uid, pr->ruid, pr->uid, pr->svuid)) ||
       ((what & FNU_IDENT_RUID) && !among(id.uid, pr->ruid, pr->uid, pr->svuid)) ||
       ((what & FNU_IDENT_EUID) && !among(id.euid, pr->ruid, pr->uid, pr->svuid)) ||
       ((what & FNU_IDENT_SVUID) && !among(id.svuid, pr->ruid, pr->uid, pr->svuid)) ||
       ((what & FNU_IDENT_GID) && !among(id.gid, pr->rgid, pr->gid, pr->svgid)) ||
       ((what & FNU_IDENT_RGID) && !among(id.gid, pr->rgid, pr->gid, pr->svgid)) ||
       ((what & FNU_IDENT_EGID) && !among(id.egid, pr->rgid, pr->gid, pr->svgid)) ||
       ((what & FNU_IDENT_SVGID) && !among(id.svgid, pr->rgid, pr->gid, pr->svgid))))
    return FNU_ERR_NOTPERMITTED;
  if (what & FNU_IDENT_UID) {
    if (id.uid == pr->uid || root)
      ruid = svuid = id.uid;
    euid = id.uid;
  }
  if (what & FNU_IDENT_GID) {
    if (id.gid == pr->gid || root)
      rgid = svgid = id.gid;
    egid = id.gid;
  }
  if (what & FNU_IDENT_RUID)
    ruid = id.uid;
  if (what & FNU_IDENT_EUID)
    euid = id.euid;
  if (what & FNU_IDENT_SVUID)
    svuid = id.svuid;
  if (what & FNU_IDENT_RGID)
    rgid = id.gid;
  if (what & FNU_IDENT_EGID)
    egid = id.egid;
  if (what & FNU_IDENT_SVGID)
    svgid = id.svgid;
  if (what & FNU_IDENT_GROUPS) {
    memset(pr->groups, 0, sizeof(pr->groups));
    memmove(pr->groups, id.groups, id.ngroups * sizeof(pr->groups[0]));
    pr->ngroups = (int)id.ngroups;
  }
  if (what & FNU_IDENT_LOGIN)
    strlcpy(pr->login, id.login, MAXLOGNAME);
  if (euid != pr->uid || svuid != pr->svuid || ruid != pr->ruid ||
      egid != pr->gid || svgid != pr->svgid || rgid != pr->rgid)
    pr->sugid = 1;
  pr->uid = euid;
  pr->svuid = svuid;
  pr->gid = egid;
  pr->rgid = rgid;
  pr->svgid = svgid;
  if (ruid != pr->ruid)
    proc_setruid(pr, ruid);
  return 0;
}

int sys_session_new(struct proc *p __unused, const register_t *uap __unused, register_t *retval) {
  pid_t sid = setsid();

  if (sid < 0)
    return (int)-sid;
  *retval = sid;
  return 0;
}

int sys_pgroup_set(struct proc *p __unused, const register_t *uap, register_t *retval __unused) {
  return setpgid((int)uap[0], (int)uap[1]);
}

int sys_limit(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_limit in, out;
  uint64_t uin = uap[1], uout = uap[2];
  int error;

  if (uin != 0 && copyin(p->p_p->pgdir, &in, (uintptr_t)uin, sizeof(in)) < 0)
    return FNU_ERR_FAULT;
  memset(&out, 0, sizeof(out));
  if ((error = setrlimit((int)uap[0], uin ? &in : 0, uout ? &out : 0)) != 0)
    return error;
  if (uout != 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)uout, &out, sizeof(out)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

int sys_usage(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_usage out;
  int error;

  memset(&out, 0, sizeof(out));
  if ((error = proc_rusage((int)uap[0], &out)) != 0)
    return error;
  return copyout(p->p_p->pgdir, (uintptr_t)uap[1], &out, sizeof(out)) < 0
             ? FNU_ERR_FAULT
             : 0;
}

int sys_niceness(struct proc *p, const register_t *uap, register_t *retval __unused) {
  int value = 0, error;

  if ((int)uap[2] != FNU_NICE_GET &&
      copyin(p->p_p->pgdir, &value, (uintptr_t)uap[3], sizeof(value)) < 0)
    return FNU_ERR_FAULT;
  if ((error = proc_niceness((int)uap[0], (int)uap[1], (int)uap[2], &value)) != 0)
    return error;
  return copyout(p->p_p->pgdir, (uintptr_t)uap[3], &value, sizeof(value)) < 0
             ? FNU_ERR_FAULT
             : 0;
}

// fnu_timer: as fnu_limit
int sys_timer(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_timer in, out;
  uint64_t uin = uap[1], uout = uap[2];
  int error;

  if (uin != 0 && copyin(p->p_p->pgdir, &in, (uintptr_t)uin, sizeof(in)) < 0)
    return FNU_ERR_FAULT;
  memset(&out, 0, sizeof(out));
  if ((error = setitimer((int)uap[0], uin ? &in : 0, uout ? &out : 0)) != 0)
    return error;
  if (uout != 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)uout, &out, sizeof(out)) < 0)
    return FNU_ERR_FAULT;
  return 0;
}

#define RANDOM_MAX 256

int sys_random(struct proc *p, const register_t *uap, register_t *retval __unused) {
  char bytes[RANDOM_MAX];
  int len = (int)uap[1];

  if (len < 1 || len > RANDOM_MAX)
    return FNU_ERR_INVALID;
  random_bytes(bytes, (uint)len);
  return copyout(p->p_p->pgdir, (uintptr_t)uap[0], bytes, (size_t)len) < 0
             ? FNU_ERR_FAULT
             : 0;
}

static int nowake;

int sys_sleep(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_timespec want, left;
  uint64_t uwant, uleft, n;
  uint64_t t0, gone;
  int error = 0;

  uwant = uap[0];
  uleft = uap[1];
  if (copyin(p->p_p->pgdir, &want, (uintptr_t)uwant, sizeof(want)) < 0)
    return FNU_ERR_FAULT;
  if (want.sec < 0 || want.nsec < 0 || want.nsec >= 1000000000)
    return FNU_ERR_INVALID;
  n = (uint64_t)want.sec > ~(uint64_t)0 / HZ - 1
          ? ~(uint64_t)0
          : (uint64_t)want.sec * HZ +
                (uint64_t)(want.nsec + (1000000000 / HZ) - 1) / (1000000000 / HZ);
  // the tick we are in is partly gone already
  if (n != 0 && n != ~(uint64_t)0)
    n++;

  while (n > 0 && error != FNU_ERR_INTERRUPTED) {
    t0 = ticks;
    error = tsleep(&nowake, PWAIT | PCATCH, "sleep",
                   n > 0x7fffffff ? 0x7fffffff : (int)n);
    gone = ticks - t0;
    n = gone >= n ? 0 : n - gone;
  }
  if (uleft != 0) {
    memset(&left, 0, sizeof(left));
    left.sec = (int64_t)(n / HZ);
    left.nsec = (int32_t)(n % HZ) * (1000000000 / HZ);
    copyout(p->p_p->pgdir, (uintptr_t)uleft, &left, sizeof(left));
  }
  p->sig_restart = 0;
  return error == FNU_ERR_INTERRUPTED ? FNU_ERR_INTERRUPTED : 0;
}

int sys_netinfo(struct proc *p, const register_t *uap, register_t *retval) {
  struct fnu_ifinfo *buf = 0;
  uint64_t dst;
  int capacity, n, error = 0;

  dst = uap[0];
  capacity = (int)uap[1];
  if (capacity < 0)
    return FNU_ERR_INVALID;
  if ((n = if_info_all(0, 0)) < capacity)
    capacity = n;
  if (capacity > 0)
    buf = malloc((size_t)capacity * sizeof(*buf), M_TEMP, M_WAITOK);
  n = if_info_all(buf, capacity);
  if (capacity > 0 &&
      copyout(p->p_p->pgdir, (uintptr_t)dst, buf,
              (size_t)(n < capacity ? n : capacity) * sizeof(*buf)) < 0)
    error = FNU_ERR_FAULT;
  if (buf != 0)
    free(buf, M_TEMP);
  *retval = n;
  return error;
}

int sys_netconfig(struct proc *p, const register_t *uap, register_t *retval __unused) {
  struct fnu_ifconfig config;
  struct ifnet *ifp;

  if (p->p_p->uid != 0)
    return FNU_ERR_NOTPERMITTED;
  if (copyin(p->p_p->pgdir, &config, (uintptr_t)uap[0], sizeof(config)) < 0)
    return FNU_ERR_FAULT;
  config.name[sizeof(config.name) - 1] = 0;
  if ((ifp = ifunit(config.name)) == 0)
    return FNU_ERR_NODEVICE;
  return net_stack_config(ifp, &config);
}

