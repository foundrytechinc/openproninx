#ifndef _LIMITS_H_
#define _LIMITS_H_

#include "inc/syslimits.h"

#define CHAR_BIT 8
#define SCHAR_MIN (-0x7f - 1)
#define SCHAR_MAX 0x7f
#define UCHAR_MAX 0xff
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define SHRT_MIN (-0x7fff - 1)
#define SHRT_MAX 0x7fff
#define USHRT_MAX 0xffff
#define INT_MIN (-0x7fffffff - 1)
#define INT_MAX 0x7fffffff
#define UINT_MAX 0xffffffffU
#define UID_MAX UINT_MAX
#define GID_MAX UINT_MAX
#define LONG_MIN (-0x7fffffffffffffffL - 1)
#define LONG_MAX 0x7fffffffffffffffL
#define ULONG_MAX 0xffffffffffffffffUL
#define LLONG_MIN (-0x7fffffffffffffffLL - 1)
#define LLONG_MAX 0x7fffffffffffffffLL
#define ULLONG_MAX 0xffffffffffffffffULL
#define QUAD_MIN LLONG_MIN
#define QUAD_MAX LLONG_MAX
#define UQUAD_MAX ULLONG_MAX

#define PATH_MAX FNU_PATHLEN_MAX
#define IOV_MAX FNU_SEGMENTS_MAX
#define _XOPEN_IOV_MAX 16
#define NAME_MAX FNU_NAMELEN_MAX
#define NGROUPS_MAX 16
#define ARG_MAX (32 * 1024)
#define OPEN_MAX 64
#define LOGIN_NAME_MAX 32
#define HOST_NAME_MAX 255
#define PIPE_BUF 512
#define SSIZE_MAX LONG_MAX
#define SIZE_T_MAX ULONG_MAX
#define OFF_MAX LLONG_MAX
#define OFF_MIN LLONG_MIN
#define MB_LEN_MAX 32
#define NL_TEXTMAX 255
#define NL_ARGMAX 9
#define LINE_MAX 2048
#define RE_DUP_MAX 255
#define _POSIX_RE_DUP_MAX 255
#define _POSIX2_RE_DUP_MAX _POSIX_RE_DUP_MAX
#define _POSIX2_LINE_MAX 2048
#define _POSIX2_BC_BASE_MAX 99
#define _POSIX2_BC_DIM_MAX 2048
#define _POSIX2_BC_SCALE_MAX 99
#define _POSIX2_BC_STRING_MAX 1000
#define _POSIX2_COLL_WEIGHTS_MAX 2
#define _POSIX2_EXPR_NEST_MAX 32

#endif
