#ifndef _SIGNAL_H_
#define _SIGNAL_H_

#include <sys/types.h>
// POSIX signal numbers; libc maps them to FNU_SIG_. A set has signal n at bit n - 1
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGEMT 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGBUS 10
#define SIGSEGV 11
#define SIGSYS 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGURG 16
#define SIGSTOP 17
#define SIGTSTP 18
#define SIGCONT 19
#define SIGCHLD 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGIO 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGINFO 29
#define SIGUSR1 30
#define SIGUSR2 31
#define NSIG 32

// si_code values
#define SI_NOINFO 32767
#define SI_USER 0
#define SI_LWP (-1)
#define SI_QUEUE (-2)
#define SI_TIMER (-3)
#define SI_ASYNCIO (-4)
#define SI_MESGQ (-5)

#define ILL_ILLOPC 1
#define ILL_ILLOPN 2
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
#define ILL_BTCFI 9

#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define BUS_OBJERR 3

#define TRAP_BRKPT 1
#define TRAP_TRACE 2

typedef void (*sig_t)(int);
#define SIG_DFL ((sig_t)0)
#define SIG_IGN ((sig_t)1)
#define SIG_ERR ((sig_t)-1)

typedef struct {
  int si_signo;
  int si_code;
  pid_t si_pid;
  int __si_pad;
  void *si_addr;
} siginfo_t;

struct sigaction {
  union {
    void (*__sa_handler)(int);
    void (*__sa_sigaction)(int, siginfo_t *, void *);
  } __sigaction_u;
  sigset_t sa_mask;
  int sa_flags;
};
#define sa_handler __sigaction_u.__sa_handler
#define sa_sigaction __sigaction_u.__sa_sigaction

#define SA_ONSTACK 0x0001
#define SA_RESTART 0x0002
#define SA_RESETHAND 0x0004
#define SA_NODEFER 0x0010
#define SA_NOCLDWAIT 0x0020
#define SA_SIGINFO 0x0040
#define SA_NOCLDSTOP 0x0008

#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3

#define SS_ONSTACK 0x1
#define SS_DISABLE 0x4
#define MINSIGSTKSZ 8192

typedef struct sigaltstack {
  void *ss_sp;
  size_t ss_size;
  int ss_flags;
} stack_t;
#define SIGSTKSZ (MINSIGSTKSZ + 32768)

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
sig_t signal(int sig, sig_t func);
int sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);
int sigwait(const sigset_t *set, int *sig);
int sigwaitinfo(const sigset_t *set, siginfo_t *info);
int sigaltstack(const stack_t *ss, stack_t *oss);
int kill(pid_t pid, int sig);
int killpg(pid_t pgrp, int sig);
int raise(int sig);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signo);
int sigdelset(sigset_t *set, int signo);
int sigismember(const sigset_t *set, int signo);
int siginterrupt(int sig, int flag);
const char *strsignal(int sig);
typedef int sig_atomic_t;
extern const int sys_nsig;
int sigblock(int mask);
int sigsetmask(int mask);
#define sigmask(signo) (1U << ((signo) - 1))
// sig2str(3) and str2sig(3), the names without the SIG
#define SIG2STR_MAX 17
int sig2str(int, char *);
int str2sig(const char *, int *);

extern const char *const sys_signame[NSIG];
extern const char *const sys_siglist[NSIG];

#endif
