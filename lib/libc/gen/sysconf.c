#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include "inc/abi.h"

long sysconf(int name) {
  struct rlimit rl;
  size_t len;
  int n;

  switch (name) {
  case _SC_ARG_MAX:
    return ARG_MAX;
  case _SC_CHILD_MAX:
    return getrlimit(RLIMIT_NPROC, &rl) ? -1 : (long)rl.rlim_cur;
  case _SC_CLK_TCK:
    return FNU_HZ;
  case _SC_NGROUPS_MAX:
    return NGROUPS_MAX;
  case _SC_OPEN_MAX:
    return getrlimit(RLIMIT_NOFILE, &rl) ? -1 : (long)rl.rlim_cur;
  case _SC_JOB_CONTROL:
  case _SC_SAVED_IDS:
    return 1;
  case _SC_VERSION:
    return 200112;
  case _SC_LINE_MAX:
    return LINE_MAX;
  case _SC_STREAM_MAX:
    return FOPEN_MAX;
  case _SC_TZNAME_MAX:
    return 255;
  case _SC_PAGESIZE:
    return PAGE_SIZE;
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
    len = sizeof(n);
    return sysctlbyname("hw.ncpu", &n, &len, NULL, 0) < 0 ? -1 : n;
  default:
    errno = EINVAL;
    return -1;
  }
}
