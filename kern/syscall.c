#include "inc/syscall.h"
#include "buf.h"
#include "defs.h"
#include "proc.h"

// User code makes a system call with INT T_SYSCALL.
// System call number in %rax.
// Arguments passed in registers: rdi, rsi, rdx, rcx, r8, r9.
// Return value in %rax.

// Fetch the int at addr from the current process.
int fetchint(uintptr_t addr, uint64_t *ip) {
  struct proc *curproc = myproc();

  if (addr >= curproc->sz || addr + 8 > curproc->sz) {
    cprintf("fetchint failed: sz: %d, addr: 0x%x\n", curproc->sz, addr);
    return -1;
  }
  *ip = *((uint64_t *)addr);
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Doesn't actually copy the string - just sets *pp to point at it.
// Returns length of string, not including nul.
int fetchstr(uintptr_t addr, char **pp) {
  char *s, *ep;
  struct proc *curproc = myproc();

  if (addr >= curproc->sz)
    return -1;
  *pp = (char *)addr;
  ep = (char *)curproc->sz;
  for (s = *pp; s < ep; s++) {
    if (*s == 0)
      return s - *pp;
  }
  return -1;
}

// Fetch the nth 64-bit system call argument.
int arg(int n, uint64_t *ip) {
  uint64_t v;

  switch (n) {
  case 0: v = myproc()->tf->rdi; break;
  case 1: v = myproc()->tf->rsi; break;
  case 2: v = myproc()->tf->rdx; break;
  case 3: v = myproc()->tf->rcx; break;
  case 4: v = myproc()->tf->r8;  break;
  case 5: v = myproc()->tf->r9;  break;
  default:
    cprintf("arg index out of range (0..5)\n");
    return -1;
  }

  *ip = v;
  return 0;
}

// Fetch the nth 32-bit system call argument.
int argint(int n, int *ip) {
  uint64_t v;
  if (arg(n, &v) < 0)
    return -1;
  *ip = (int64_t)v;
  return 0;
}

// Fetch the nth word-sized system call argument as a pointer
// to a block of memory of size bytes. Check that the pointer
// lies within the process address space.
int argptr(int n, char **pp, int size) {
  uintptr_t i;
  struct proc *curproc = myproc();

  if (arg(n, &i) < 0)
    return -1;
  if (size < 0 || i >= curproc->sz || i + size > curproc->sz)
    return -1;
  *pp = (char *)i;
  return 0;
}

// Fetch the nth word-sized system call argument as a string pointer.
// Check that the pointer is valid and the string is nul-terminated.
// (There is no shared writable memory, so the string can't change
// between this check and being used by the kernel.)
int argstr(int n, char **pp) {
  uintptr_t addr;
  if (arg(n, (uint64_t *)&addr) < 0)
    return -1;
  return fetchstr(addr, pp);
}

/* ========================================================================== */
/* STABLE CORE ABI syscall declarations                                       */
/* ========================================================================== */
extern int64_t sys_fork(void);
extern int64_t sys_exit(void);
extern int64_t sys_wait(void);
extern int64_t sys_waitpid(void);
extern int64_t sys_pipe(void);
extern int64_t sys_read(void);
extern int64_t sys_kill(void);
extern int64_t sys_signal(void);
extern int64_t sys_sigcatch(void);
extern int64_t sys_sigwait(void);
extern int64_t sys_halt(void);
extern int64_t sys_hwinfo(void);
extern int64_t sys_diskinfo(void);
extern int64_t sys_exec(void);
extern int64_t sys_stat(void);
extern int64_t sys_chdir(void);
extern int64_t sys_getcwd(void);
extern int64_t sys_dup(void);
extern int64_t sys_getpid(void);
extern int64_t sys_sbrk(void);
extern int64_t sys_sleep(void);
extern int64_t sys_open(void);
extern int64_t sys_write(void);
extern int64_t sys_mknod(void);
extern int64_t sys_unlink(void);
extern int64_t sys_link(void);
extern int64_t sys_mkdir(void);
extern int64_t sys_close(void);
extern int64_t sys_malloc(void);
extern int64_t sys_free(void);
extern int64_t sys_info(void);
extern int64_t sys_reboot(void);
extern int64_t sys_poweroff(void);
extern int64_t sys_procinfo(void);
extern int64_t sys_ioctl(void);
extern int64_t sys_getdents(void);
extern int64_t sys_getuid(void);
extern int64_t sys_setuid(void);
extern int64_t sys_setforeground(void);
extern int64_t sys_chmod(void);
extern int64_t sys_chown(void);

/* ========================================================================== */
/* EXPERIMENTAL / NON-ABI syscall declarations                                */
/* (lwIP transport handles, in-kernel auth -- subject to redesign)            */
/* ========================================================================== */
extern int64_t sys_ping(void);
extern int64_t sys_udp_open(void);
extern int64_t sys_udp_bind(void);
extern int64_t sys_udp_sendto(void);
extern int64_t sys_udp_recvfrom(void);
extern int64_t sys_udp_close(void);
extern int64_t sys_netinfo(void);
extern int64_t sys_netconfig(void);
extern int64_t sys_tcp_open(void);
extern int64_t sys_tcp_bind(void);
extern int64_t sys_tcp_listen(void);
extern int64_t sys_tcp_accept(void);
extern int64_t sys_tcp_connect(void);
extern int64_t sys_tcp_send(void);
extern int64_t sys_tcp_recv(void);
extern int64_t sys_tcp_close(void);
extern int64_t sys_dns_resolve(void);
extern int64_t sys_login(void);
extern int64_t sys_doas_auth(void);
extern int64_t sys_useradd(void);
extern int64_t sys_passwd(void);
extern int64_t sys_users(void);
/* FNU userland utility syscalls — implemented in kclock.c / fs.c */
extern int64_t sys_rtctime(void);
extern int64_t sys_df(void);

static int64_t (*syscalls[])(void) = {
    /* ---- Stable Core ABI ---- */
    [SYS_exit]          = sys_exit,
    [SYS_read]          = sys_read,
    [SYS_write]         = sys_write,
    [SYS_open]          = sys_open,
    [SYS_close]         = sys_close,
    [SYS_fork]          = sys_fork,
    [SYS_wait]          = sys_wait,
    [SYS_waitpid]       = sys_waitpid,
    [SYS_kill]          = sys_kill,
    [SYS_signal]        = sys_signal,
    [SYS_sigcatch]      = sys_sigcatch,
    [SYS_sigwait]       = sys_sigwait,
    [SYS_halt]          = sys_halt,
    [SYS_hwinfo]        = sys_hwinfo,
    [SYS_diskinfo]      = sys_diskinfo,
    [SYS_exec]          = sys_exec,
    [SYS_sbrk]          = sys_sbrk,
    [SYS_malloc]        = sys_malloc,
    [SYS_free]          = sys_free,
    [SYS_stat]          = sys_stat,
    [SYS_pipe]          = sys_pipe,
    [SYS_info]          = sys_info,
    [SYS_reboot]        = sys_reboot,
    [SYS_procinfo]      = sys_procinfo,
    [SYS_ioctl]         = sys_ioctl,
    [SYS_getdents]      = sys_getdents,
    [SYS_link]          = sys_link,
    [SYS_mkdir]         = sys_mkdir,
    [SYS_unlink]        = sys_unlink,
    [SYS_dup]           = sys_dup,
    [SYS_mknod]         = sys_mknod,
    [SYS_chdir]         = sys_chdir,
    [SYS_getcwd]        = sys_getcwd,
    [SYS_getpid]        = sys_getpid,
    [SYS_sleep]         = sys_sleep,
    [SYS_getuid]        = sys_getuid,
    [SYS_setuid]        = sys_setuid,
    [SYS_setforeground] = sys_setforeground,
    [SYS_chmod]         = sys_chmod,
    [SYS_chown]         = sys_chown,
    [SYS_poweroff]      = sys_poweroff,

    /* ---- Experimental / Non-ABI ---- */
    [SYS_ping]          = sys_ping,
    [SYS_udp_open]      = sys_udp_open,
    [SYS_udp_bind]      = sys_udp_bind,
    [SYS_udp_sendto]    = sys_udp_sendto,
    [SYS_udp_recvfrom]  = sys_udp_recvfrom,
    [SYS_udp_close]     = sys_udp_close,
    [SYS_netinfo]       = sys_netinfo,
    [SYS_netconfig]     = sys_netconfig,
    [SYS_tcp_open]      = sys_tcp_open,
    [SYS_tcp_bind]      = sys_tcp_bind,
    [SYS_tcp_listen]    = sys_tcp_listen,
    [SYS_tcp_accept]    = sys_tcp_accept,
    [SYS_tcp_connect]   = sys_tcp_connect,
    [SYS_tcp_send]      = sys_tcp_send,
    [SYS_tcp_recv]      = sys_tcp_recv,
    [SYS_tcp_close]     = sys_tcp_close,
    [SYS_dns_resolve]   = sys_dns_resolve,
    [SYS_login]         = sys_login,
    [SYS_doas_auth]     = sys_doas_auth,
    [SYS_useradd]       = sys_useradd,
    [SYS_passwd]        = sys_passwd,
    [SYS_users]         = sys_users,
    /* FNU userland utility ABIs (new in Core 0.1.5-dev FNU refresh) */
    [SYS_rtctime]       = sys_rtctime,
    [SYS_df]            = sys_df,
};

void syscall(void) {
  size_t num;
  struct proc *curproc = myproc();

  num = curproc->tf->rax;
  if (num < NELEM(syscalls) && syscalls[num]) {
    curproc->tf->rax = syscalls[num]();
  } else {
    cprintf("%d %s: unknown sys call %d\n", curproc->pid, curproc->name, num);
    curproc->tf->rax = -1;
  }
}
