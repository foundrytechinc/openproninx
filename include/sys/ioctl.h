#ifndef _SYS_IOCTL_H_
#define _SYS_IOCTL_H_

#include <sys/types.h>
#include <sys/ttycom.h>
#include <sys/filio.h>
#include "inc/abi.h"
#include "inc/ioccom.h"

int ioctl(int fd, unsigned long request, ...);

#endif
