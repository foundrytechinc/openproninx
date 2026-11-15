#ifndef _SYS_FILIO_H_
#define _SYS_FILIO_H_

#include <sys/ioctl.h>

#define FIOCLEX FNU_IOCLOSEEXEC
#define FIONCLEX FNU_IOKEEPEXEC
#define FIONREAD FNU_IOWAITING
#define FIONBIO FNU_IONOWAIT
#define FIODTYPE _IOR('f', 122, int) // what kind of device: D_* of sys/device.h

#endif
