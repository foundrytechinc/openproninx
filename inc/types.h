#ifndef PRONINX_X86_64_TYPES_H
#define PRONINX_X86_64_TYPES_H

#include "inc/cdefs.h"

typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

#ifndef USE_STD

typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
#if defined(_KERNEL) || defined(_STANDALONE)
typedef long int64_t;
typedef unsigned long uint64_t;
#else
typedef long long int64_t;
typedef unsigned long long uint64_t;
#endif
#if __SIZEOF_LONG__ == 4 && __SIZEOF_POINTER__ == 8
// uefi is LLP64
typedef unsigned long long uintptr_t;
typedef unsigned long long size_t;
typedef long long ssize_t;
typedef long long intptr_t;
typedef long long register_t;
#else
typedef unsigned long uintptr_t;
typedef unsigned long size_t;
typedef long ssize_t;
typedef long intptr_t;
typedef long register_t;
#endif

#define NULL ((void *)0)

#endif

typedef uint64_t pte_t;
typedef int32_t pid_t;

typedef uint64_t pointer_t;

struct fnu_timespec {
  int64_t sec;
  int32_t nsec;
  int32_t pad;
};
typedef uint64_t uint64;
typedef uint8_t proc_state_t;
typedef int64_t int64;
typedef int32_t int32;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;

#define major(x) (((uint)(x) >> 8) & 0xff)
#define minor(x) ((uint)(x) & 0xff)
#define makedev(maj, min) ((uint)((((maj) & 0xff) << 8) | ((min) & 0xff)))

#endif
