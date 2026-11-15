// SIGNALS
#ifndef PRONINX_SIGNAL_H
#define PRONINX_SIGNAL_H

#include "inc/types.h"

// asked of it from outside
#define FNU_SIG_HANGUP 1 // its terminal went away
#define FNU_SIG_INTERRUPT 2
#define FNU_SIG_QUIT 3
#define FNU_SIG_TERMINATE 4
#define FNU_SIG_KILL 5 // neither caught nor blocked
#define FNU_SIG_STOP 6 // neither caught nor blocked
#define FNU_SIG_TTYSTOP 7 // the terminal's stop key
#define FNU_SIG_CONTINUE 8
#define FNU_SIG_USER1 9
#define FNU_SIG_USER2 10

// its own doing
#define FNU_SIG_ILLEGAL 16
#define FNU_SIG_ARITH 17
#define FNU_SIG_MEMORY 18
#define FNU_SIG_BUS 19
#define FNU_SIG_BREAK 20
#define FNU_SIG_BADCALL 21
#define FNU_SIG_ABORT 22
#define FNU_SIG_CPULIMIT 23
#define FNU_SIG_FILELIMIT 24

// what happened around it
#define FNU_SIG_CHILD 32
#define FNU_SIG_BROKENPIPE 33
#define FNU_SIG_ALARM 34
#define FNU_SIG_VTALARM 35
#define FNU_SIG_PROFILE 36
#define FNU_SIG_TTYIN 37
#define FNU_SIG_TTYOUT 38
#define FNU_SIG_WINDOW 39 // its terminal changed size
#define FNU_SIG_INFO 40
#define FNU_SIG_IO 41
#define FNU_SIG_URGENT 42

#define FNU_SIG_LAST 42
#define FNU_SIG_COUNT (FNU_SIG_LAST + 1)

#define FNU_SIGBIT(s) (1ull << (s))
#define FNU_SIG_UNCATCHABLE (FNU_SIGBIT(FNU_SIG_KILL) | FNU_SIGBIT(FNU_SIG_STOP))

// who sent it
#define FNU_SIGFROM_PROCESS 0
#define FNU_SIGFROM_SYSTEM 1
#define FNU_SIGFROM_FAULT 2 // addr is where

#define FNU_FAULT_INTDIV 1   // integer division by zero
#define FNU_FAULT_INTOVF 2   // integer overflow
#define FNU_FAULT_FLTDIV 3   // floating division by zero
#define FNU_FAULT_FLTOVF 4   // floating overflow
#define FNU_FAULT_FLTUND 5   // floating underflow
#define FNU_FAULT_FLTRES 6   // inexact floating result
#define FNU_FAULT_FLTINV 7   // invalid floating operation
#define FNU_FAULT_FLTSUB 8   // subscript out of range
#define FNU_FAULT_UNMAPPED 9 // nothing mapped at addr
#define FNU_FAULT_ACCESS 10
#define FNU_FAULT_ALIGN 11   // misaligned
#define FNU_FAULT_OBJECT 12
#define FNU_FAULT_PRIVOP 13
#define FNU_FAULT_COPROC 14  // the coprocessor
#define FNU_FAULT_BREAK 15

struct fnu_siginfo {
  int32_t sig;
  int32_t from;
  pid_t sender;
  int32_t why; // FNU_FAULT_*, from FNU_SIGFROM_FAULT
  uint64_t addr;
};

struct fnu_sigcontext {
  uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t trapno;
  uint64_t err;
  uint64_t rip;
  uint16_t cs;
  uint16_t cs_pad1;
  uint32_t cs_pad2;
  uint64_t rflags;
  uint64_t rsp;
  uint16_t ss;
  uint16_t ss_pad1;
  uint32_t ss_pad2;
  uint64_t mask;
  int32_t sig;
  int32_t pad;
  uint64_t cookie;
  uint64_t fpstate;
};

#define FNU_FPSTATE_SIZE 512

struct fnu_sigframe {
  struct fnu_siginfo si;
  struct fnu_sigcontext sc;
};

#define FNU_SIGACT_DEFAULT 0
#define FNU_SIGACT_IGNORE 1

struct fnu_sigact {
  uint64_t handler;
  uint64_t mask;
  uint32_t flags;
  uint32_t pad;
};

#define FNU_SIGACT_ONCE 0x1
#define FNU_SIGACT_REENTER 0x2
#define FNU_SIGACT_RESTART 0x4
#define FNU_SIGACT_ALTSTACK 0x8

#endif
