#include <sys/mman.h>
#include <fenv.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static sigjmp_buf back;
static volatile int got_sig, got_code;
static void *volatile got_addr;
static int failures;

static void caught(int sig, siginfo_t *si, void *ctx) {
  (void)ctx;
  got_sig = sig;
  got_code = si->si_code;
  got_addr = si->si_addr;
  siglongjmp(back, 1);
}

static void ok(const char *what, int good) {
  printf("faulttest: %-40s %s\n", what, good ? "ok" : "FAILED");
  if (!good) {
    printf("faulttest:   signal %d code %d addr %p\n", got_sig, got_code, got_addr);
    failures++;
  }
}

static volatile int zero;
static volatile double fzero;

int main(void) {
  struct sigaction sa;
  volatile int i;
  volatile double d;
  char *page;
  int s[] = {SIGFPE, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGUSR1};
  unsigned k;

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = caught;
  sa.sa_flags = SA_SIGINFO;
  for (k = 0; k < sizeof(s) / sizeof(s[0]); k++)
    sigaction(s[k], &sa, 0);

  got_sig = 0;
  if (sigsetjmp(back, 1) == 0)
    i = 1000 / zero;
  ok("integer division by zero is FPE_INTDIV", got_sig == SIGFPE && got_code == FPE_INTDIV);

  got_sig = 0;
  feenableexcept(FE_DIVBYZERO);
  if (sigsetjmp(back, 1) == 0)
    d = 1.0 / fzero;
  fedisableexcept(FE_DIVBYZERO);
  feclearexcept(FE_ALL_EXCEPT);
  ok("floating division by zero is FPE_FLTDIV", got_sig == SIGFPE && got_code == FPE_FLTDIV);

  got_sig = 0;
  if (sigsetjmp(back, 1) == 0)
    i = *(volatile int *)16;
  ok("nothing mapped is SEGV_MAPERR at its address",
     got_sig == SIGSEGV && got_code == SEGV_MAPERR && got_addr == (void *)16);

  page = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  got_sig = 0;
  if (sigsetjmp(back, 1) == 0)
    *(volatile char *)page = 1;
  ok("a read-only page written is SEGV_ACCERR",
     got_sig == SIGSEGV && got_code == SEGV_ACCERR && got_addr == page);
  munmap(page, 4096);

  got_sig = 0;
  if (sigsetjmp(back, 1) == 0)
    __asm volatile("ud2");
  ok("an invalid opcode is SIGILL, ILL_PRVOPC", got_sig == SIGILL && got_code == ILL_PRVOPC);

  got_sig = 0;
  if (sigsetjmp(back, 1) == 0)
    __asm volatile("int3");
  ok("a breakpoint is TRAP_BRKPT", got_sig == SIGTRAP && got_code == TRAP_BRKPT);

  got_sig = 0;
  if (sigsetjmp(back, 1) == 0)
    kill(getpid(), SIGUSR1);
  ok("kill(2) is SI_USER", got_sig == SIGUSR1 && got_code == SI_USER);

  (void)i;
  (void)d;
  printf(failures ? "faulttest: FAILED\n" : "faulttest: ok\n");
  return failures != 0;
}
