#ifndef _SYS_IOCCOM_H_
#define _SYS_IOCCOM_H_

#include "inc/ioccom.h"

int ioctl(int fd, unsigned long request, ...);

#endif
