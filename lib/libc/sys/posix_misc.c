// The last few POSIX names over the system's own calls
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <fnu/sys.h>
#include "posix.h"

int sendsyslog(const char *buf, size_t len, int flags) {
  return (int)__posix(fnu_sendsyslog(buf, len, flags));
}

__dead void _exit(int status) {
  fnu_exit(status);
  for (;;)
    ;
}
