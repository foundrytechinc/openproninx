// the name the kernel keeps for the session

#include <errno.h>
#include <sys/param.h>
#include <unistd.h>

static char loginbuf[MAXLOGNAME];

char *
getlogin(void)
{
	int r;

	if ((r = getlogin_r(loginbuf, sizeof(loginbuf))) != 0) {
		errno = r;
		return (NULL);
	}
	return (loginbuf);
}
