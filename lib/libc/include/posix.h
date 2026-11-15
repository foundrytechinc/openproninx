// the POSIX shim: -error in, -1 and errno out
#ifndef _LIBC_POSIX_H_
#define _LIBC_POSIX_H_

#include <errno.h>

int __errno_of(int);

// the POSIX layer's numbers in the system's
int __oflags_to_native(int);
int __oflags_of_native(int);
int __at_fd(int);
int __at_flags(int);
int __family_to_native(int);
int __socktype_to_native(int);
const void *__addr_in(const void *, unsigned int, void *);
void __addr_out(void *, unsigned int);
int __tty_ioctl(int, unsigned long, void *);

static inline long __posix(long r) {
	if (r < 0) {
		errno = __errno_of((int)-r);
		return -1;
	}
	return r;
}

#endif /* _LIBC_POSIX_H_ */
