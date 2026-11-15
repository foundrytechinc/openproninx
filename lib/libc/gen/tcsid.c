// session of a terminal, and how one claims it

#include <sys/ioctl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>

pid_t
tcgetsid(int fd)
{
	int sid;

	if (ioctl(fd, TIOCGSID, &sid) < 0)
		return (-1);
	return ((pid_t)sid);
}

int
tcsetsid(int fd, pid_t pid)
{
	// only the caller's own session may be handed a terminal
	if (pid != getsid(0)) {
		errno = EPERM;
		return (-1);
	}
	return (ioctl(fd, TIOCSCTTY));
}
