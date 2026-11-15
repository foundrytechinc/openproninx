// sig2str(3), str2sig(3). no realtime signals, no RTMIN+n

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
sig2str(int signum, char *str)
{
	if (signum <= 0 || signum >= NSIG || sys_signame[signum] == NULL)
		return (-1);
	if (strlcpy(str, sys_signame[signum], SIG2STR_MAX) >= SIG2STR_MAX)
		return (-1);
	return (0);
}

int
str2sig(const char *str, int *pnum)
{
	const char *p;
	char *end;
	long n;
	int sig;

	if (str == NULL || *str == '\0')
		return (-1);
	// a name, with or without the SIG in front of it
	p = str;
	if (strncasecmp(p, "SIG", 3) == 0 && p[3] != '\0')
		p += 3;
	for (sig = 1; sig < NSIG; sig++)
		if (sys_signame[sig] != NULL && strcasecmp(sys_signame[sig], p) == 0) {
			*pnum = sig;
			return (0);
		}
	// or a number, which POSIX allows as well
	n = strtol(str, &end, 10);
	if (end == str || *end != '\0' || n <= 0 || n >= NSIG)
		return (-1);
	*pnum = (int)n;
	return (0);
}
