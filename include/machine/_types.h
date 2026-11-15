// page geometry and the __ types
#ifndef _MACHINE__TYPES_H_
#define _MACHINE__TYPES_H_

#define _MAX_PAGE_SHIFT		12

typedef signed char		__int8_t;
typedef unsigned char		__uint8_t;
typedef short			__int16_t;
typedef unsigned short		__uint16_t;
typedef int			__int32_t;
typedef unsigned int		__uint32_t;
typedef long long		__int64_t;
typedef unsigned long long	__uint64_t;
typedef long long	__off_t;
typedef double			__double_t;
typedef float			__float_t;
typedef unsigned long	__size_t;
typedef __builtin_va_list	__va_list;

#define _ALIGNBYTES		(sizeof(long) - 1)
#define _STACKALIGNBYTES	15
#define _ALIGN(p)		(((unsigned long)(p) + _ALIGNBYTES) &~_ALIGNBYTES)
#define _ALIGNED_POINTER(p, t)	1

#endif
