#ifndef _SYS_SYSCTL_H_
#define _SYS_SYSCTL_H_

#include <sys/types.h>
#include "inc/abi.h"

// knobs are named (kern.hostname, hw.ncpu): no numbers, no header to keep in step
#define CTLTYPE_INT FNU_CTL_INT
#define CTLTYPE_QUAD FNU_CTL_QUAD
#define CTLTYPE_STRING FNU_CTL_STRING
#define CTLTYPE_OPAQUE FNU_CTL_OPAQUE

int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    const void *newp, size_t newlen);

#endif
