#include "inc/syscall.h"
#include "buf.h"
#include "defs.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

// INT T_SYSCALL

int sys_exit(struct proc *, const register_t *, register_t *);
int sys_fork(struct proc *, const register_t *, register_t *);
int sys_exec(struct proc *, const register_t *, register_t *);
int sys_wait(struct proc *, const register_t *, register_t *);
int sys_signal(struct proc *, const register_t *, register_t *);
int sys_procids(struct proc *, const register_t *, register_t *);
int sys_session_new(struct proc *, const register_t *, register_t *);
int sys_pgroup_set(struct proc *, const register_t *, register_t *);
int sys_ident_get(struct proc *, const register_t *, register_t *);
int sys_ident_set(struct proc *, const register_t *, register_t *);
int sys_niceness(struct proc *, const register_t *, register_t *);
int sys_createmask(struct proc *, const register_t *, register_t *);
int sys_limit(struct proc *, const register_t *, register_t *);
int sys_usage(struct proc *, const register_t *, register_t *);
int sys_timer(struct proc *, const register_t *, register_t *);
int sys_sleep(struct proc *, const register_t *, register_t *);
int sys_halt(struct proc *, const register_t *, register_t *);
int sys_reboot(struct proc *, const register_t *, register_t *);
int sys_poweroff(struct proc *, const register_t *, register_t *);
int sys_ptrace(struct proc *, const register_t *, register_t *);
int sys_sigact(struct proc *, const register_t *, register_t *);
int sys_sigmask(struct proc *, const register_t *, register_t *);
int sys_sigraised(struct proc *, const register_t *, register_t *);
int sys_sigtake(struct proc *, const register_t *, register_t *);
int sys_sigpause(struct proc *, const register_t *, register_t *);
int sys_sigreturn(struct proc *, const register_t *, register_t *);
int sys_altstack(struct proc *, const register_t *, register_t *);
int sys_thread_start(struct proc *, const register_t *, register_t *);
int sys_thread_exit(struct proc *, const register_t *, register_t *);
int sys_thread_tcb(struct proc *, const register_t *, register_t *);
int sys_thread_id(struct proc *, const register_t *, register_t *);
int sys_thread_yield(struct proc *, const register_t *, register_t *);
int sys_futex(struct proc *, const register_t *, register_t *);
int sys_read(struct proc *, const register_t *, register_t *);
int sys_write(struct proc *, const register_t *, register_t *);
int sys_seek(struct proc *, const register_t *, register_t *);
int sys_close(struct proc *, const register_t *, register_t *);
int sys_dup(struct proc *, const register_t *, register_t *);
int sys_pipe(struct proc *, const register_t *, register_t *);
int sys_ioctl(struct proc *, const register_t *, register_t *);
int sys_fdstate_get(struct proc *, const register_t *, register_t *);
int sys_fdstate_set(struct proc *, const register_t *, register_t *);
int sys_ready(struct proc *, const register_t *, register_t *);
int sys_flush(struct proc *, const register_t *, register_t *);
int sys_readdir(struct proc *, const register_t *, register_t *);
int sys_eventq(struct proc *, const register_t *, register_t *);
int sys_events(struct proc *, const register_t *, register_t *);
int sys_open(struct proc *, const register_t *, register_t *);
int sys_getattr(struct proc *, const register_t *, register_t *);
int sys_setattr(struct proc *, const register_t *, register_t *);
int sys_make(struct proc *, const register_t *, register_t *);
int sys_remove(struct proc *, const register_t *, register_t *);
int sys_link(struct proc *, const register_t *, register_t *);
int sys_rename(struct proc *, const register_t *, register_t *);
int sys_readlink(struct proc *, const register_t *, register_t *);
int sys_chdir(struct proc *, const register_t *, register_t *);
int sys_access(struct proc *, const register_t *, register_t *);
int sys_pathconf(struct proc *, const register_t *, register_t *);
int sys_pathof(struct proc *, const register_t *, register_t *);
int sys_mount(struct proc *, const register_t *, register_t *);
int sys_unmount(struct proc *, const register_t *, register_t *);
int sys_fsstat(struct proc *, const register_t *, register_t *);
int sys_swapon(struct proc *, const register_t *, register_t *);
int sys_swapoff(struct proc *, const register_t *, register_t *);
int sys_unveil(struct proc *, const register_t *, register_t *);
int sys_map(struct proc *, const register_t *, register_t *);
int sys_unmap(struct proc *, const register_t *, register_t *);
int sys_protect(struct proc *, const register_t *, register_t *);
int sys_memsync(struct proc *, const register_t *, register_t *);
int sys_memuse(struct proc *, const register_t *, register_t *);
int sys_socket(struct proc *, const register_t *, register_t *);
int sys_socketpair(struct proc *, const register_t *, register_t *);
int sys_bind(struct proc *, const register_t *, register_t *);
int sys_listen(struct proc *, const register_t *, register_t *);
int sys_accept(struct proc *, const register_t *, register_t *);
int sys_connect(struct proc *, const register_t *, register_t *);
int sys_send(struct proc *, const register_t *, register_t *);
int sys_recv(struct proc *, const register_t *, register_t *);
int sys_shutdown(struct proc *, const register_t *, register_t *);
int sys_sockaddr(struct proc *, const register_t *, register_t *);
int sys_sockopt_set(struct proc *, const register_t *, register_t *);
int sys_sockopt_get(struct proc *, const register_t *, register_t *);
int sys_info(struct proc *, const register_t *, register_t *);
int sys_procinfo(struct proc *, const register_t *, register_t *);
int sys_hwinfo(struct proc *, const register_t *, register_t *);
int sys_diskinfo(struct proc *, const register_t *, register_t *);
int sys_netinfo(struct proc *, const register_t *, register_t *);
int sys_netconfig(struct proc *, const register_t *, register_t *);
int sys_rtctime(struct proc *, const register_t *, register_t *);
int sys_gettime(struct proc *, const register_t *, register_t *);
int sys_random(struct proc *, const register_t *, register_t *);
int sys_sysctl(struct proc *, const register_t *, register_t *);
int sys_sysctlnode(struct proc *, const register_t *, register_t *);
int sys_pledge(struct proc *, const register_t *, register_t *);
int sys_sendsyslog(struct proc *, const register_t *, register_t *);
int sys_flock(struct proc *, const register_t *, register_t *);
int sys_settime(struct proc *, const register_t *, register_t *);
int sys_slewtime(struct proc *, const register_t *, register_t *);

static int (*syscalls[])(struct proc *, const register_t *, register_t *) = {
    [SYS_exit]         = sys_exit,
    [SYS_fork]         = sys_fork,
    [SYS_exec]         = sys_exec,
    [SYS_wait]         = sys_wait,
    [SYS_signal]       = sys_signal,
    [SYS_procids]      = sys_procids,
    [SYS_session_new]  = sys_session_new,
    [SYS_pgroup_set]   = sys_pgroup_set,
    [SYS_ident_get]    = sys_ident_get,
    [SYS_ident_set]    = sys_ident_set,
    [SYS_niceness]     = sys_niceness,
    [SYS_createmask]   = sys_createmask,
    [SYS_limit]        = sys_limit,
    [SYS_usage]        = sys_usage,
    [SYS_timer]        = sys_timer,
    [SYS_sleep]        = sys_sleep,
    [SYS_halt]         = sys_halt,
    [SYS_reboot]       = sys_reboot,
    [SYS_poweroff]     = sys_poweroff,
    [SYS_ptrace]       = sys_ptrace,
    [SYS_sigact]       = sys_sigact,
    [SYS_sigmask]      = sys_sigmask,
    [SYS_sigraised]    = sys_sigraised,
    [SYS_sigtake]      = sys_sigtake,
    [SYS_sigpause]     = sys_sigpause,
    [SYS_sigreturn]    = sys_sigreturn,
    [SYS_altstack]     = sys_altstack,
    [SYS_thread_start] = sys_thread_start,
    [SYS_thread_exit]  = sys_thread_exit,
    [SYS_thread_tcb]   = sys_thread_tcb,
    [SYS_thread_id]    = sys_thread_id,
    [SYS_thread_yield] = sys_thread_yield,
    [SYS_futex]        = sys_futex,
    [SYS_read]         = sys_read,
    [SYS_write]        = sys_write,
    [SYS_seek]         = sys_seek,
    [SYS_close]        = sys_close,
    [SYS_dup]          = sys_dup,
    [SYS_pipe]         = sys_pipe,
    [SYS_ioctl]        = sys_ioctl,
    [SYS_fdstate_get]  = sys_fdstate_get,
    [SYS_fdstate_set]  = sys_fdstate_set,
    [SYS_ready]        = sys_ready,
    [SYS_flush]        = sys_flush,
    [SYS_readdir]      = sys_readdir,
    [SYS_eventq]       = sys_eventq,
    [SYS_events]       = sys_events,
    [SYS_open]         = sys_open,
    [SYS_getattr]      = sys_getattr,
    [SYS_setattr]      = sys_setattr,
    [SYS_make]         = sys_make,
    [SYS_remove]       = sys_remove,
    [SYS_link]         = sys_link,
    [SYS_rename]       = sys_rename,
    [SYS_readlink]     = sys_readlink,
    [SYS_chdir]        = sys_chdir,
    [SYS_access]       = sys_access,
    [SYS_pathconf]     = sys_pathconf,
    [SYS_pathof]       = sys_pathof,
    [SYS_mount]        = sys_mount,
    [SYS_unmount]      = sys_unmount,
    [SYS_fsstat]       = sys_fsstat,
    [SYS_swapon]       = sys_swapon,
    [SYS_swapoff]      = sys_swapoff,
    [SYS_unveil]       = sys_unveil,
    [SYS_map]          = sys_map,
    [SYS_unmap]        = sys_unmap,
    [SYS_protect]      = sys_protect,
    [SYS_memsync]      = sys_memsync,
    [SYS_memuse]       = sys_memuse,
    [SYS_socket]       = sys_socket,
    [SYS_socketpair]   = sys_socketpair,
    [SYS_bind]         = sys_bind,
    [SYS_listen]       = sys_listen,
    [SYS_accept]       = sys_accept,
    [SYS_connect]      = sys_connect,
    [SYS_send]         = sys_send,
    [SYS_recv]         = sys_recv,
    [SYS_shutdown]     = sys_shutdown,
    [SYS_sockaddr]     = sys_sockaddr,
    [SYS_sockopt_set]  = sys_sockopt_set,
    [SYS_sockopt_get]  = sys_sockopt_get,
    [SYS_info]         = sys_info,
    [SYS_procinfo]     = sys_procinfo,
    [SYS_hwinfo]       = sys_hwinfo,
    [SYS_diskinfo]     = sys_diskinfo,
    [SYS_netinfo]      = sys_netinfo,
    [SYS_netconfig]    = sys_netconfig,
    [SYS_rtctime]      = sys_rtctime,
    [SYS_gettime]      = sys_gettime,
    [SYS_random]       = sys_random,
    [SYS_sysctl]       = sys_sysctl,
    [SYS_sysctlnode]   = sys_sysctlnode,
    [SYS_pledge]       = sys_pledge,
    [SYS_sendsyslog]   = sys_sendsyslog,
    [SYS_flock]        = sys_flock,
    [SYS_settime]      = sys_settime,
    [SYS_slewtime]     = sys_slewtime,
};

void syscall(void) {
  size_t num;
  struct proc *p = curproc;
  struct trapframe *tf = p->tf;
  register_t rval[2], args[6];
  int error;

  num = tf->rax;
  p->sig_callno = num;
  p->sig_restart = 0;
  rval[0] = 0;
  rval[1] = 0;
  if (num < nitems(syscalls) && syscalls[num]) {
    if (pledge_check(p, (int)num) < 0) {
      pledge_fail(p, (int)num);
      error = FNU_ERR_NOTPERMITTED;
    } else {
      args[0] = (register_t)tf->rdi;
      args[1] = (register_t)tf->rsi;
      args[2] = (register_t)tf->rdx;
      args[3] = (register_t)tf->rcx;
      args[4] = (register_t)tf->r8;
      args[5] = (register_t)tf->r9;
      error = syscalls[num](p, args, rval);
    }
  } else {
    printf("%d %s: unknown sys call %ld\n", p->p_p->pid, p->p_p->name, (long)num);
    error = FNU_ERR_NOCALL;
  }
  switch (error) {
  case 0:
    tf->rax = (uint64_t)rval[0];
    tf->rflags &= ~FL_CF;
    break;
  case CALL_JUSTRETURN:
    break;
  default:
    tf->rax = (uint64_t)error;
    tf->rflags |= FL_CF;
    break;
  }
}
