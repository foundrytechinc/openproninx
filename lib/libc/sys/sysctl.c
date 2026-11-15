#include <sys/sysctl.h>
#include <fnu/sys.h>
#include "posix.h"

// ENOMEM: *oldlenp is what it would need
int
sysctlbyname(const char *name, void *oldp, size_t *oldlenp, const void *newp,
    size_t newlen)
{
	uint64_t len = oldlenp != 0 ? *oldlenp : 0;
	int r;

	r = (int)__posix(fnu_sysctl(name, oldp, oldlenp != 0 ? &len : 0, newp,
	    newlen));
	if (oldlenp != 0)
		*oldlenp = (size_t)len;
	return (r);
}
