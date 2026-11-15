#ifndef _SYS_FILE_H_
#define _SYS_FILE_H_

#include <sys/types.h>
#include <fcntl.h>

#define LOCK_SH 0x01
#define LOCK_EX 0x02
#define LOCK_NB 0x04
#define LOCK_UN 0x08

int flock(int fd, int op);

#endif
