// signals: POSIX numbers in, native out
// __sig_dispatch says the number back and builds the siginfo_t
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fnu/sys.h>
#include "posix.h"

static const unsigned char to_native[NSIG] = {
    [SIGHUP] = FNU_SIG_HANGUP,       [SIGINT] = FNU_SIG_INTERRUPT,
    [SIGQUIT] = FNU_SIG_QUIT,        [SIGILL] = FNU_SIG_ILLEGAL,
    [SIGTRAP] = FNU_SIG_BREAK,       [SIGABRT] = FNU_SIG_ABORT,
    [SIGFPE] = FNU_SIG_ARITH,        [SIGKILL] = FNU_SIG_KILL,
    [SIGBUS] = FNU_SIG_BUS,          [SIGSEGV] = FNU_SIG_MEMORY,
    [SIGSYS] = FNU_SIG_BADCALL,      [SIGPIPE] = FNU_SIG_BROKENPIPE,
    [SIGALRM] = FNU_SIG_ALARM,       [SIGTERM] = FNU_SIG_TERMINATE,
    [SIGURG] = FNU_SIG_URGENT,       [SIGSTOP] = FNU_SIG_STOP,
    [SIGTSTP] = FNU_SIG_TTYSTOP,     [SIGCONT] = FNU_SIG_CONTINUE,
    [SIGCHLD] = FNU_SIG_CHILD,       [SIGTTIN] = FNU_SIG_TTYIN,
    [SIGTTOU] = FNU_SIG_TTYOUT,      [SIGIO] = FNU_SIG_IO,
    [SIGXCPU] = FNU_SIG_CPULIMIT,    [SIGXFSZ] = FNU_SIG_FILELIMIT,
    [SIGVTALRM] = FNU_SIG_VTALARM,   [SIGPROF] = FNU_SIG_PROFILE,
    [SIGWINCH] = FNU_SIG_WINDOW,     [SIGINFO] = FNU_SIG_INFO,
    [SIGUSR1] = FNU_SIG_USER1,       [SIGUSR2] = FNU_SIG_USER2,
};

// system number to POSIX; 0 for no POSIX name
int __sig_of_native(int n) {
  int s;

  for (s = 1; s < NSIG; s++)
    if (to_native[s] == n)
      return s;
  return 0;
}

// and back, 0 for one the system does not have (SIGEMT)
int __sig_to_native(int s) {
  return s > 0 && s < NSIG ? to_native[s] : 0;
}

uint64_t __set_to_native(sigset_t set) {
  uint64_t out = 0;
  int s;

  for (s = 1; s < NSIG; s++)
    if ((set & sigmask(s)) && to_native[s] != 0)
      out |= FNU_SIGBIT(to_native[s]);
  return out;
}

sigset_t __set_of_native(uint64_t set) {
  sigset_t out = 0;
  int s;

  for (s = 1; s < NSIG; s++)
    if (to_native[s] != 0 && (set & FNU_SIGBIT(to_native[s])))
      out |= sigmask(s);
  return out;
}

// sigaction as given, by POSIX number
static struct {
  void (*plain)(int);
  void (*info)(int, siginfo_t *, void *);
  int flags;
} handler[NSIG];

// si_code: faults by kind, the rest as sent
static int __si_code(const struct fnu_siginfo *si) {
  static const int fault[] = {
      [FNU_FAULT_INTDIV] = FPE_INTDIV,   [FNU_FAULT_INTOVF] = FPE_INTOVF,
      [FNU_FAULT_FLTDIV] = FPE_FLTDIV,   [FNU_FAULT_FLTOVF] = FPE_FLTOVF,
      [FNU_FAULT_FLTUND] = FPE_FLTUND,   [FNU_FAULT_FLTRES] = FPE_FLTRES,
      [FNU_FAULT_FLTINV] = FPE_FLTINV,   [FNU_FAULT_FLTSUB] = FPE_FLTSUB,
      [FNU_FAULT_UNMAPPED] = SEGV_MAPERR, [FNU_FAULT_ACCESS] = SEGV_ACCERR,
      [FNU_FAULT_ALIGN] = BUS_ADRALN,    [FNU_FAULT_OBJECT] = BUS_OBJERR,
      [FNU_FAULT_PRIVOP] = ILL_PRVOPC,   [FNU_FAULT_COPROC] = ILL_COPROC,
      [FNU_FAULT_BREAK] = TRAP_BRKPT,
  };

  if (si->from != FNU_SIGFROM_FAULT)
    return SI_USER;
  if (si->why <= 0 || si->why >= (int)(sizeof(fault) / sizeof(fault[0])))
    return SI_NOINFO;
  return fault[si->why];
}

static void __sig_dispatch(int n, struct fnu_siginfo *si, void *ctx) {
  int s = __sig_of_native(n);
  siginfo_t info;

  if (s == 0)
    return;
  if (!(handler[s].flags & SA_SIGINFO)) {
    if (handler[s].plain != 0)
      handler[s].plain(s);
    return;
  }
  if (handler[s].info == 0)
    return;
  memset(&info, 0, sizeof(info));
  info.si_signo = s;
  info.si_code = __si_code(si);
  info.si_pid = si->sender;
  info.si_addr = (void *)(uintptr_t)si->addr;
  handler[s].info(s, &info, ctx);
}

static uint32_t flags_to_native(int sa) {
  return ((sa & SA_RESETHAND) ? FNU_SIGACT_ONCE : 0) |
         ((sa & SA_NODEFER) ? FNU_SIGACT_REENTER : 0) |
         ((sa & SA_RESTART) ? FNU_SIGACT_RESTART : 0) |
         ((sa & SA_ONSTACK) ? FNU_SIGACT_ALTSTACK : 0);
}

static int flags_of_native(uint32_t f) {
  return ((f & FNU_SIGACT_ONCE) ? SA_RESETHAND : 0) |
         ((f & FNU_SIGACT_REENTER) ? SA_NODEFER : 0) |
         ((f & FNU_SIGACT_RESTART) ? SA_RESTART : 0) |
         ((f & FNU_SIGACT_ALTSTACK) ? SA_ONSTACK : 0);
}

static int bad(int e) {
  errno = e;
  return -1;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
  struct fnu_sigact in, out;
  int n = __sig_to_native(sig);
  void (*wasplain)(int) = 0;
  void (*wasinfo)(int, siginfo_t *, void *) = 0;
  int wasflags = 0;

  if (n == 0)
    return bad(EINVAL);
  memset(&in, 0, sizeof(in));
  if (act != 0) {
    in.mask = __set_to_native(act->sa_mask) & ~FNU_SIG_UNCATCHABLE;
    in.flags = flags_to_native(act->sa_flags);
    if (act->sa_handler == SIG_DFL)
      in.handler = FNU_SIGACT_DEFAULT;
    else if (act->sa_handler == SIG_IGN)
      in.handler = FNU_SIGACT_IGNORE;
    else
      in.handler = (uint64_t)(uintptr_t)__sig_dispatch;
    wasplain = handler[sig].plain;
    wasinfo = handler[sig].info;
    wasflags = handler[sig].flags;
    if (in.handler == (uint64_t)(uintptr_t)__sig_dispatch) {
      handler[sig].plain = (act->sa_flags & SA_SIGINFO) ? 0 : act->sa_handler;
      handler[sig].info = (act->sa_flags & SA_SIGINFO) ? act->sa_sigaction : 0;
      handler[sig].flags = act->sa_flags;
    }
  }
  if (__posix(fnu_sigact(n, act ? &in : 0, &out)) < 0) {
    if (act != 0) {
      handler[sig].plain = wasplain;
      handler[sig].info = wasinfo;
      handler[sig].flags = wasflags;
    }
    return -1;
  }
  if (act == 0) {
    wasplain = handler[sig].plain;
    wasinfo = handler[sig].info;
    wasflags = handler[sig].flags;
  }
  if (oact != 0) {
    memset(oact, 0, sizeof(*oact));
    if (out.handler == FNU_SIGACT_DEFAULT)
      oact->sa_handler = SIG_DFL;
    else if (out.handler == FNU_SIGACT_IGNORE)
      oact->sa_handler = SIG_IGN;
    else if (wasflags & SA_SIGINFO)
      oact->sa_sigaction = wasinfo;
    else
      oact->sa_handler = wasplain;
    oact->sa_mask = __set_of_native(out.mask);
    oact->sa_flags = flags_of_native(out.flags) |
                     (out.handler > FNU_SIGACT_IGNORE ? wasflags & SA_SIGINFO : 0);
  }
  return 0;
}

sigset_t __sigintr; // the signals siginterrupt(3) left interrupting

sig_t signal(int sig, sig_t func) {
  struct sigaction sa, old;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = func;
  if (!sigismember(&__sigintr, sig))
    sa.sa_flags = SA_RESTART;
  if (sigaction(sig, &sa, &old) < 0)
    return SIG_ERR;
  return old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset) {
  uint64_t was, n = set != 0 ? __set_to_native(*set) : 0;

  if (set == 0)
    was = fnu_sigmask(0, 0);
  else if (how == SIG_BLOCK)
    was = fnu_sigmask(n, 0);
  else if (how == SIG_UNBLOCK)
    was = fnu_sigmask(0, n);
  else if (how == SIG_SETMASK)
    was = fnu_sigmask(n, ~n);
  else
    return bad(EINVAL);
  if (oset != 0)
    *oset = __set_of_native(was);
  return 0;
}

int sigpending(sigset_t *set) {
  *set = __set_of_native(fnu_sigraised());
  return 0;
}

int sigsuspend(const sigset_t *mask) {
  return (int)__posix(fnu_sigpause(__set_to_native(*mask)));
}

int pause(void) {
  sigset_t mask;

  return sigprocmask(SIG_BLOCK, NULL, &mask) ? -1 : sigsuspend(&mask);
}

int sigwaitinfo(const sigset_t *set, siginfo_t *info) {
  struct fnu_siginfo si;
  int s;

  if (__posix(fnu_sigtake(__set_to_native(*set), &si, 1)) < 0)
    return -1;
  s = __sig_of_native(si.sig);
  if (info != 0) {
    memset(info, 0, sizeof(*info));
    info->si_signo = s;
    info->si_code = __si_code(&si);
    info->si_pid = si.sender;
    info->si_addr = (void *)si.addr;
  }
  return s;
}

// the error comes back, errno is left alone
int sigwait(const sigset_t *set, int *sig) {
  int saved = errno, got = sigwaitinfo(set, 0), r = 0;

  if (got < 0)
    r = errno;
  else
    *sig = got;
  errno = saved;
  return r;
}

int sigaltstack(const stack_t *ss, stack_t *oss) {
  struct fnu_altstack in, out;

  if (ss != 0) {
    in.base = (uint64_t)ss->ss_sp;
    in.size = ss->ss_size;
    in.flags = ((ss->ss_flags & SS_DISABLE) ? FNU_SIGSTACK_OFF : 0) |
               ((ss->ss_flags & SS_ONSTACK) ? FNU_SIGSTACK_INUSE : 0);
    in.pad = 0;
  }
  if (__posix(fnu_altstack(ss ? &in : 0, oss ? &out : 0)) < 0)
    return -1;
  if (oss != 0) {
    oss->ss_sp = (void *)out.base;
    oss->ss_size = out.size;
    oss->ss_flags = ((out.flags & FNU_SIGSTACK_OFF) ? SS_DISABLE : 0) |
                    ((out.flags & FNU_SIGSTACK_INUSE) ? SS_ONSTACK : 0);
  }
  return 0;
}

int kill(pid_t pid, int sig) {
  int n = sig == 0 ? 0 : __sig_to_native(sig);

  if (sig != 0 && n == 0)
    return bad(EINVAL);
  return (int)__posix(fnu_signal(pid, n));
}

int raise(int sig) { return kill(getpid(), sig); }

int sigemptyset(sigset_t *set) {
  *set = 0;
  return 0;
}

int sigfillset(sigset_t *set) {
  *set = ~(sigset_t)0;
  return 0;
}

static int valid(int signo) {
  if (signo <= 0 || signo >= NSIG) {
    errno = EINVAL;
    return 0;
  }
  return 1;
}

int sigaddset(sigset_t *set, int signo) {
  if (!valid(signo))
    return -1;
  *set |= sigmask(signo);
  return 0;
}

int sigdelset(sigset_t *set, int signo) {
  if (!valid(signo))
    return -1;
  *set &= ~sigmask(signo);
  return 0;
}

int sigismember(const sigset_t *set, int signo) {
  if (!valid(signo))
    return -1;
  return (*set & sigmask(signo)) != 0;
}

const char *const sys_signame[NSIG] = {
    "Signal 0", "HUP",  "INT",  "QUIT", "ILL",  "TRAP", "ABRT", "EMT",
    "FPE",      "KILL", "BUS",  "SEGV", "SYS",  "PIPE", "ALRM", "TERM",
    "URG",      "STOP", "TSTP", "CONT", "CHLD", "TTIN", "TTOU", "IO",
    "XCPU",     "XFSZ", "VTALRM", "PROF", "WINCH", "INFO", "USR1", "USR2",
};

const char *const sys_siglist[NSIG] = {
    "Signal 0",
    "Hangup",
    "Interrupt",
    "Quit",
    "Illegal instruction",
    "Trace/BPT trap",
    "Abort trap",
    "EMT trap",
    "Floating point exception",
    "Killed",
    "Bus error",
    "Segmentation fault",
    "Bad system call",
    "Broken pipe",
    "Alarm clock",
    "Terminated",
    "Urgent I/O condition",
    "Suspended (signal)",
    "Suspended",
    "Continued",
    "Child exited",
    "Stopped (tty input)",
    "Stopped (tty output)",
    "I/O possible",
    "Cputime limit exceeded",
    "Filesize limit exceeded",
    "Virtual timer expired",
    "Profiling timer expired",
    "Window size changes",
    "Information request",
    "User defined signal 1",
    "User defined signal 2",
};

const int sys_nsig = NSIG;

const char *strsignal(int sig) {
  return sig > 0 && sig < NSIG ? sys_siglist[sig] : "Unknown signal";
}

// the 4.3BSD pair, in POSIX sets
int sigblock(int mask) {
  return (int)__set_of_native(fnu_sigmask(__set_to_native((sigset_t)mask), 0));
}

int sigsetmask(int mask) {
  uint64_t n = __set_to_native((sigset_t)mask);

  return (int)__set_of_native(fnu_sigmask(n, ~n));
}
