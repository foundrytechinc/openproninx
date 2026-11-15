// exit status packing
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

void __rusage_of(const struct fnu_usage *k, struct rusage *ru);
int __sig_of_native(int);

int __status_of(const struct fnu_exitinfo *e) {
  switch (e->reason) {
  case FNU_EXIT_KILLED:
    return W_EXITCODE(0, __sig_of_native(e->status));
  case FNU_EXIT_STOPPED:
    return W_STOPCODE(__sig_of_native(e->status));
  case FNU_EXIT_CONTINUED:
    return 0x13;
  default:
    return W_EXITCODE(e->status, 0);
  }
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage) {
  struct fnu_exitinfo e;
  int flags = 0;
  pid_t got;

  if (options & ~(WNOHANG | WUNTRACED | WCONTINUED)) {
    errno = EINVAL;
    return -1;
  }
  if (options & WNOHANG)
    flags |= FNU_WAIT_NOHANG;
  if (options & WUNTRACED)
    flags |= FNU_WAIT_STOPPED;
  if (options & WCONTINUED)
    flags |= FNU_WAIT_CONTINUED;
  got = (pid_t)__posix(fnu_wait(pid, &e, flags));
  if (got > 0) {
    if (status != 0)
      *status = __status_of(&e);
    if (rusage != 0)
      __rusage_of(&e.ru, rusage);
  }
  return got;
}

pid_t wait3(int *status, int options, struct rusage *rusage) {
  return wait4(-1, status, options, rusage);
}

pid_t waitpid(pid_t pid, int *status, int options) {
  return wait4(pid, status, options, 0);
}

pid_t wait(int *status) { return wait4(-1, status, 0, 0); }
