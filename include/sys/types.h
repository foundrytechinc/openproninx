#ifndef _SYS_TYPES_H_
#define _SYS_TYPES_H_

#include "inc/types.h"
#include <machine/_types.h>

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define BYTE_ORDER LITTLE_ENDIAN

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

typedef int64_t off_t;
typedef int64_t quad_t;
typedef int __wchar_t;
typedef int __wint_t;
typedef int __rune_t;
typedef union {
  char __mbstate8[128];
  int64_t __mbstateL;
} __mbstate_t;
typedef uint64_t u_quad_t;
typedef int64_t time_t;
typedef long suseconds_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t mode_t;
typedef uint32_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
typedef int64_t blkcnt_t;
typedef int32_t blksize_t;
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;
typedef uint32_t socklen_t;
typedef uint8_t sa_family_t;
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
typedef unsigned int sigset_t;
typedef int clockid_t;
typedef unsigned int useconds_t;
typedef uint32_t id_t;
typedef long clock_t;

typedef char *caddr_t; // the core address type

// the select types and the byte order come along
#include <sys/select.h>
#include <sys/endian.h>

#endif
