// vfork is fork. nothing shares anything

#include <unistd.h>

pid_t
vfork(void)
{
	return (fork());
}
