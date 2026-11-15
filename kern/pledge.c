// PLEDGE(2)
#include "defs.h"
#include "file.h"
#include "inc/abi.h"
#include "inc/error.h"
#include "inc/signal.h"
#include "inc/stat.h"
#include "inc/syscall.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"

struct promise {
  const char *name;
  uint64_t bit;
};

static const struct promise promises[] = {
    {"stdio", FNU_PLEDGE_STDIO},       {"rpath", FNU_PLEDGE_RPATH},
    {"wpath", FNU_PLEDGE_WPATH},       {"cpath", FNU_PLEDGE_CPATH},
    {"dpath", FNU_PLEDGE_DPATH},
    {"inet", FNU_PLEDGE_INET},         {"dns", FNU_PLEDGE_DNS},
    {"fattr", FNU_PLEDGE_FATTR},       {"chown", FNU_PLEDGE_CHOWN},
    {"flock", FNU_PLEDGE_FLOCK},       {"getpw", FNU_PLEDGE_GETPW},
    {"tape", FNU_PLEDGE_TAPE},
    {"proc", FNU_PLEDGE_PROC},         {"exec", FNU_PLEDGE_EXEC},
    {"id", FNU_PLEDGE_ID},             {"tty", FNU_PLEDGE_TTY},
    {"ps", FNU_PLEDGE_PS},             {"vminfo", FNU_PLEDGE_VMINFO},
    {"settime", FNU_PLEDGE_SETTIME},   {"audio", FNU_PLEDGE_AUDIO},
    {"disklabel", FNU_PLEDGE_DISKLABEL}, {"mount", FNU_PLEDGE_MOUNT},
    {"protexec", FNU_PLEDGE_PROTEXEC}, {"error", FNU_PLEDGE_ERROR},
    {"unveil", FNU_PLEDGE_UNVEIL},     {"route", FNU_PLEDGE_ROUTE},
    {"unix", FNU_PLEDGE_UNIX},         {"sendfd", FNU_PLEDGE_SENDFD},
    {"recvfd", FNU_PLEDGE_RECVFD},
};

#define S FNU_PLEDGE_STDIO

static const uint64_t needs[NSYSCALL] = {
    // processes
    [SYS_exit] = FNU_PLEDGE_ALWAYS,
    [SYS_fork] = FNU_PLEDGE_PROC,
    [SYS_exec] = FNU_PLEDGE_EXEC,
    [SYS_wait] = S,
    [SYS_signal] = FNU_PLEDGE_PROC | S,
    [SYS_procids] = S,
    [SYS_session_new] = FNU_PLEDGE_PROC,
    [SYS_pgroup_set] = FNU_PLEDGE_PROC,
    [SYS_ident_get] = S,
    [SYS_ident_set] = FNU_PLEDGE_ID,
    [SYS_niceness] = FNU_PLEDGE_PROC,
    [SYS_createmask] = S,
    [SYS_limit] = S,
    [SYS_usage] = S,
    [SYS_timer] = S,
    [SYS_sleep] = S,
    // signals
    [SYS_sigact] = S,
    [SYS_sigmask] = S,
    [SYS_sigraised] = S,
    [SYS_sigtake] = S,
    [SYS_sigpause] = S,
    [SYS_sigreturn] = FNU_PLEDGE_ALWAYS,
    [SYS_altstack] = S,
    // threads
    [SYS_thread_start] = S,
    [SYS_thread_exit] = S,
    [SYS_thread_tcb] = FNU_PLEDGE_ALWAYS,
    [SYS_thread_id] = S,
    [SYS_thread_yield] = S,
    [SYS_futex] = S,
    // descriptors
    [SYS_read] = S,
    [SYS_write] = S,
    [SYS_seek] = S,
    [SYS_close] = S,
    [SYS_dup] = S,
    [SYS_pipe] = S,
    [SYS_ioctl] = S | FNU_PLEDGE_TTY | FNU_PLEDGE_AUDIO | FNU_PLEDGE_DISKLABEL | FNU_PLEDGE_TAPE,
    [SYS_fdstate_get] = S,
    [SYS_fdstate_set] = S,
    [SYS_ready] = S,
    [SYS_flush] = S,
    [SYS_readdir] = FNU_PLEDGE_RPATH,
    [SYS_eventq] = S,
    [SYS_events] = S,
    // names
    [SYS_open] = S | FNU_PLEDGE_RPATH | FNU_PLEDGE_WPATH | FNU_PLEDGE_CPATH,
    [SYS_getattr] = S | FNU_PLEDGE_RPATH,
    [SYS_setattr] = S | FNU_PLEDGE_WPATH | FNU_PLEDGE_FATTR | FNU_PLEDGE_CHOWN,
    [SYS_make] = FNU_PLEDGE_CPATH | FNU_PLEDGE_DPATH,
    [SYS_remove] = FNU_PLEDGE_CPATH,
    [SYS_link] = FNU_PLEDGE_CPATH,
    [SYS_rename] = FNU_PLEDGE_CPATH,
    [SYS_readlink] = FNU_PLEDGE_RPATH,
    [SYS_chdir] = FNU_PLEDGE_RPATH,
    [SYS_access] = FNU_PLEDGE_RPATH,
    [SYS_pathconf] = FNU_PLEDGE_RPATH,
    [SYS_pathof] = S | FNU_PLEDGE_RPATH,
    [SYS_mount] = FNU_PLEDGE_MOUNT,
    [SYS_unmount] = FNU_PLEDGE_MOUNT,
    [SYS_fsstat] = FNU_PLEDGE_RPATH,
    [SYS_unveil] = FNU_PLEDGE_UNVEIL,
    // memory
    [SYS_map] = S,
    [SYS_unmap] = S,
    [SYS_protect] = S,
    [SYS_memsync] = S,
    [SYS_memuse] = S,
    // sockets
    [SYS_socket] = FNU_PLEDGE_INET | FNU_PLEDGE_UNIX | FNU_PLEDGE_DNS,
    [SYS_socketpair] = S,
    [SYS_bind] = FNU_PLEDGE_INET | FNU_PLEDGE_UNIX,
    [SYS_listen] = FNU_PLEDGE_INET | FNU_PLEDGE_UNIX,
    [SYS_accept] = FNU_PLEDGE_INET | FNU_PLEDGE_UNIX,
    [SYS_connect] = FNU_PLEDGE_INET | FNU_PLEDGE_UNIX | FNU_PLEDGE_DNS,
    [SYS_send] = S | FNU_PLEDGE_DNS,
    [SYS_recv] = S,
    [SYS_shutdown] = S,
    [SYS_sockaddr] = S,
    [SYS_sockopt_set] = S,
    [SYS_sockopt_get] = S,
    // the machine
    [SYS_info] = FNU_PLEDGE_VMINFO | S,
    [SYS_procinfo] = FNU_PLEDGE_PS,
    [SYS_hwinfo] = FNU_PLEDGE_VMINFO,
    [SYS_diskinfo] = FNU_PLEDGE_DISKLABEL,
    [SYS_netinfo] = FNU_PLEDGE_INET | FNU_PLEDGE_PS,
    [SYS_rtctime] = S,
    [SYS_gettime] = S,
    [SYS_random] = S,
    [SYS_sysctl] = S,
    [SYS_sysctlnode] = S,
    [SYS_pledge] = FNU_PLEDGE_ALWAYS,
    [SYS_sendsyslog] = S,
    [SYS_flock] = FNU_PLEDGE_FLOCK,
    [SYS_settime] = FNU_PLEDGE_SETTIME,
    [SYS_slewtime] = S | FNU_PLEDGE_SETTIME,
};

#undef S

static int parse(const char *text, uint64_t *out) {
  uint64_t bits = 0;
  const char *p = text;
  int i;

  while (*p != 0) {
    const char *word;
    size_t len;

    while (*p == ' ' || *p == '\t')
      p++;
    word = p;
    while (*p != 0 && *p != ' ' && *p != '\t')
      p++;
    if ((len = (size_t)(p - word)) == 0)
      continue;
    for (i = 0; i < (int)(sizeof(promises) / sizeof(promises[0])); i++)
      if (strlen(promises[i].name) == len &&
          strncmp(promises[i].name, word, len) == 0)
        break;
    if (i == (int)(sizeof(promises) / sizeof(promises[0])))
      return -1;
    bits |= promises[i].bit;
  }
  *out = bits;
  return 0;
}

int pledge_check(struct proc *p, int num) {
  uint64_t want;

  if (!p->p_p->pledged)
    return 0;
  if (num < 0 || num >= NSYSCALL)
    return -1;
  want = needs[num];
  if (want == FNU_PLEDGE_ALWAYS)
    return 0;
  if (want != 0 && (want & p->p_p->pledge) != 0)
    return 0;
  return -1;
}

int pledge_need(struct proc *p, uint64_t bits, int num) {
  if (!p->p_p->pledged || (p->p_p->pledge & bits) != 0)
    return 0;
  pledge_fail(p, num);
  return FNU_ERR_NOTPERMITTED;
}

int pledge_need_all(struct proc *p, uint64_t bits, int num) {
  if (!p->p_p->pledged || (p->p_p->pledge & bits) == bits)
    return 0;
  pledge_fail(p, num);
  return FNU_ERR_NOTPERMITTED;
}

int pledge_socket(struct proc *p, int domain) {
  uint64_t have = p->p_p->pledge;

  if (!p->p_p->pledged)
    return 0;
  if (domain == FNU_NET_LOCAL)
    return (have & FNU_PLEDGE_UNIX) ? 0 : -1;
  return (have & (FNU_PLEDGE_INET | FNU_PLEDGE_DNS)) ? 0 : -1;
}

int pledge_passfd(struct proc *p, struct file *f, int recv) {
  uint64_t need = recv ? FNU_PLEDGE_RECVFD : FNU_PLEDGE_SENDFD;
  int num = recv ? SYS_recv : SYS_send;

  if (!p->p_p->pledged)
    return 0;
  if ((p->p_p->pledge & need) == 0) {
    pledge_fail(p, num);
    return FNU_ERR_NOTPERMITTED;
  }
  if (f->type != FD_INODE || f->ip == 0 || f->ip->type != FNU_TYPE_DIR)
    return 0;
  if (recv)
    return FNU_ERR_NOTPERMITTED;
  pledge_fail(p, num);
  return FNU_ERR_INVALID;
}

void pledge_fail(struct proc *p, int num) {
  printf("pledge: %s(%d) asked for %s\n", p->p_p->name, p->p_p->pid,
         num >= 0 && num < NSYSCALL && syscallnames[num] ? syscallnames[num] : "a call");
  if (p->p_p->pledge & FNU_PLEDGE_ERROR)
    return;
  sig_post(p, FNU_SIG_ABORT, 1);
}

static int promises_in(struct proc *p, uintptr_t u, uint64_t *out, int *have) {
  char text[1024];
  int error;

  *have = 0;
  if (u == 0)
    return 0;
  if ((error = copyinstr(p->p_p->pgdir, text, u, sizeof(text), 0)) != 0)
    return error;
  if (parse(text, out) < 0)
    return FNU_ERR_INVALID;
  *have = 1;
  return 0;
}

int sys_pledge(struct proc *p, const register_t *uap, register_t *retval __unused) {
  uint64_t promise = 0, execpromise = 0;
  int have, haveexec, error;

  if ((error = promises_in(p, (uintptr_t)uap[0], &promise, &have)) != 0 ||
      (error = promises_in(p, (uintptr_t)uap[1], &execpromise, &haveexec)) != 0)
    return error;
  if (have && p->p_p->pledged && (promise | p->p_p->pledge) != p->p_p->pledge)
    return FNU_ERR_NOTPERMITTED;
  if (haveexec && p->p_p->pledged && (execpromise | p->p_p->execpledge) != p->p_p->execpledge)
    return FNU_ERR_NOTPERMITTED;
  if (have) {
    p->p_p->pledge = promise;
    p->p_p->pledged = 1;
  }
  if (haveexec)
    p->p_p->execpledge = execpromise;
  return 0;
}
