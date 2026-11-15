// timespec_get(3): C11's one clock, the wall clock
#include <time.h>
#include <fnu/sys.h>

int
timespec_get(struct timespec *ts, int base)
{
	struct fnu_timespec t;

	if (base != TIME_UTC || fnu_gettime(&t, FNU_CLOCK_WALL) < 0)
		return 0;
	ts->tv_sec = t.sec;
	ts->tv_nsec = t.nsec;
	return base;
}
