#ifndef _SYS_MMAN_H_
#define _SYS_MMAN_H_

#include <sys/types.h>
#include <sys/ioctl.h>
#include "inc/abi.h"

// libc maps these to the system's own
#define PROT_NONE 0x00
#define PROT_READ 0x01
#define PROT_WRITE 0x02
#define PROT_EXEC 0x04
#define MAP_SHARED 0x0001
#define MAP_PRIVATE 0x0002
#define MAP_FIXED 0x0010
#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)
#define MS_ASYNC 0x01
#define MS_SYNC 0x02
#define MS_INVALIDATE 0x04
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 6
#define POSIX_MADV_NORMAL MADV_NORMAL
#define POSIX_MADV_RANDOM MADV_RANDOM
#define POSIX_MADV_SEQUENTIAL MADV_SEQUENTIAL
#define POSIX_MADV_WILLNEED MADV_WILLNEED
#define POSIX_MADV_DONTNEED MADV_DONTNEED

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t len);
int msync(void *addr, size_t len, int flags);
int mprotect(void *addr, size_t len, int prot);

int madvise(void *, size_t, int);
int posix_madvise(void *, size_t, int);

#endif
