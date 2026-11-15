#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <fnu/sys.h>
#include <sys/syscall.h>
#include "inc/syscall.h"
#include <err.h>

static int number(const char *text, int fallback) {
  int value = 0;

  if (text == 0 || *text == 0)
    return fallback;
  while (*text) {
    if (*text < '0' || *text > '9')
      return fallback;
    value = value * 10 + *text++ - '0';
  }
  return value;
}

static void report(pid_t pid, int status) {
  if (WIFEXITED(status))
    printf("trace: %d exited with status %d\n", pid, WEXITSTATUS(status));
  else if (WIFSIGNALED(status))
    printf("trace: %d killed by signal %d\n", pid, WTERMSIG(status));
  else if (WIFSTOPPED(status))
    printf("trace: %d stopped by signal %d\n", pid, WSTOPSIG(status));
  else
    printf("trace: %d continued\n", pid);
}

#include "kern/syscalls.c"

static const char *callname(int n) {
  return n >= 0 && n < NSYSCALL ? syscallnames[n] : 0;
}

int main(int argc, char **argv) {
  int info;
  pid_t got;
  struct fnu_regs regs;
  struct fnu_ptrace_state st;
  struct fnu_ptrace_event ev;
  struct fnu_dbregs db;
  int steps = 8, first = 1, pid, i, syscalls = 0, forks = 0, live = 1;
  uint64_t bp = 0;
  int64_t word;

  while (first < argc && argv[first][0] == '-') {
    if (strcmp(argv[first], "-s") == 0 && first + 1 < argc) {
      steps = number(argv[first + 1], 8);
      first += 2;
    } else if (strcmp(argv[first], "-S") == 0) {
      syscalls = 1;
      first++;
    } else if (strcmp(argv[first], "-f") == 0) {
      forks = 1;
      first++;
    } else if (strcmp(argv[first], "-b") == 0 && first + 1 < argc) {
      bp = (uint64_t)strtoll(argv[first + 1], 0, 0);
      first += 2;
    } else {
      break;
    }
  }
  if (first >= argc) {
    dprintf(2, "usage: trace [-s COUNT] [-S] [-f] [-b ADDR] program [args...]\n");
    exit(1);
  }

  if ((pid = fork()) < 0) {
    warn("fork");
    exit(1);
  }
  if (pid == 0) {
    if (ptrace(FNU_PT_TRACE_ME, 0, 0, 0) < 0) {
      printf("trace: this kernel will not trace me\n");
      exit(1);
    }
    execvp(argv[first], argv + first);
    warn("%s", argv[first]);
    exit(1);
  }

  if (waitpid(pid, &info, 0) != pid || !WIFSTOPPED(info)) {
    printf("trace: %s never stopped for us\n", argv[first]);
    exit(1);
  }
  ptrace(FNU_PT_GET_PROCESS_STATE, pid, (uintptr_t)&st, 0);
  printf("trace: %s stopped before its first instruction (event %x)\n",
         argv[first], st.pe_report_event);

  if (forks) {
    ev.pe_set_event = FNU_PTRACE_FORK;
    if (ptrace(FNU_PT_SET_EVENT_MASK, pid, (uintptr_t)&ev, 0) < 0)
      warn("FNU_PT_SET_EVENT_MASK");
  }
  if (bp != 0) {
    memset(&db, 0, sizeof(db));
    db.dr[0] = bp;
    db.dr[7] = 0x1 | (0x3 << 16) | (0x3 << 18);
    if (ptrace(FNU_PT_SETDBREGS, pid, (uintptr_t)&db, 0) < 0)
      warn("FNU_PT_SETDBREGS");
  }

  for (i = 0; i < steps; i++) {
    if (ptrace(FNU_PT_GETREGS, pid, (uintptr_t)&regs, 0) < 0) {
      warn("FNU_PT_GETREGS");
      break;
    }
    word = ptrace(FNU_PT_READ_D, pid, (uintptr_t)regs.rip, 0);
    printf("%d: rip %llx  rsp %llx  rax %llx  [rip] %llx\n", i, regs.rip, regs.rsp,
           regs.rax, word);
    if (ptrace(FNU_PT_STEP, pid, FNU_PT_CONTINUE_HERE, 0) < 0) {
      warn("FNU_PT_STEP");
      break;
    }
    if (waitpid(pid, &info, 0) != pid || !WIFSTOPPED(info)) {
      report(pid, info);
      exit(0);
    }
  }

  if (ptrace(syscalls ? FNU_PT_SYSCALL : FNU_PT_CONTINUE, pid, FNU_PT_CONTINUE_HERE, 0) < 0) {
    warn("FNU_PT_CONTINUE");
    exit(1);
  }
  while (live > 0 && (got = waitpid(-1, &info, 0)) > 0) {
    if (!WIFSTOPPED(info)) {
      report(got, info);
      live--;
      continue;
    }
    if (WSTOPSIG(info) != SIGTRAP) {
      ptrace(syscalls ? FNU_PT_SYSCALL : FNU_PT_CONTINUE, got, FNU_PT_CONTINUE_HERE,
             WSTOPSIG(info));
      continue;
    }
    memset(&st, 0, sizeof(st));
    ptrace(FNU_PT_GET_PROCESS_STATE, got, (uintptr_t)&st, 0);
    switch (st.pe_report_event) {
    case FNU_PTRACE_SCE:
      ptrace(FNU_PT_GETREGS, got, (uintptr_t)&regs, 0);
      if (callname((int)regs.rax) != 0)
        printf("trace: %d %s(%llx, %llx, %llx)", got, callname((int)regs.rax),
               regs.rdi, regs.rsi, regs.rdx);
      else
        printf("trace: %d syscall %ld(%llx, %llx, %llx)", got, (long)regs.rax,
               regs.rdi, regs.rsi, regs.rdx);
      break;
    case FNU_PTRACE_SCX:
      ptrace(FNU_PT_GETREGS, got, (uintptr_t)&regs, 0);
      printf(" = %ld\n", (long)regs.rax);
      break;
    case FNU_PTRACE_FORK:
      printf("trace: %d forked, the other side is %d\n", got,
             st.pe_other_pid);
      if (st.pe_other_pid != 0 && got != pid)
        live++;
      break;
    case FNU_PTRACE_EXEC:
      printf("trace: %d ran a new program\n", got);
      break;
    case FNU_PTRACE_HWBP:
      ptrace(FNU_PT_GETREGS, got, (uintptr_t)&regs, 0);
      ptrace(FNU_PT_GETDBREGS, got, (uintptr_t)&db, 0);
      printf("trace: %d touched %llx at rip %llx (dr6 %llx)\n", got, bp,
             regs.rip, db.dr[6]);
      break;
    default:
      printf("trace: %d trap\n", got);
      break;
    }
    ptrace(syscalls ? FNU_PT_SYSCALL : FNU_PT_CONTINUE, got, FNU_PT_CONTINUE_HERE, 0);
  }
  exit(0);
}
