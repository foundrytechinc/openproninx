/* Inside libc: what signal(3) and siginterrupt(3) share. */
#ifndef _LIBC_SIGNAL_H_
#define _LIBC_SIGNAL_H_

#include_next <signal.h>

extern sigset_t __sigintr;

#endif /* _LIBC_SIGNAL_H_ */
