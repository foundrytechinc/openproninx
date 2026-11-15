#ifndef _SYS_PARAM_H_
#define _SYS_PARAM_H_

#include <sys/types.h>
#include <limits.h>
#include <signal.h>
#include "inc/cdefs.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)
#define MAXBSIZE 65536
#define NGROUPS NGROUPS_MAX
#define MAXHOSTNAMELEN 256
#define MAXPATHLEN PATH_MAX
#define DEV_BSIZE 512
#define MACHINE "amd64"
#define MACHINE_ARCH "amd64"
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#define ALIGNBYTES _ALIGNBYTES
#define ALIGN(p) _ALIGN(p)
#define powerof2(x) ((((x) - 1) & (x)) == 0)

#define MAXLOGNAME 33 // a login name and its terminator

#endif
