#ifndef PRONINX_LWIP_ARCH_CC_H
#define PRONINX_LWIP_ARCH_CC_H

#include "inc/types.h"
#include "kern/defs.h"

#define LWIP_NO_STDDEF_H 1
#define LWIP_NO_STDINT_H 1
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_LIMITS_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define SSIZE_MAX 0x7fffffffffffffffL
#define LWIP_PLATFORM_DIAG(x) do { cprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) panic(x)
#define LWIP_RAND() ((u32_t)ticks)
#define LWIP_UNUSED_ARG(x) (void)(x)

typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uint64_t u64_t;
typedef int64_t s64_t;
typedef uintptr_t mem_ptr_t;
typedef intptr_t ptrdiff_t;

#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define X8_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define BYTE_ORDER LITTLE_ENDIAN
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_FLD_8(x) x
#define PACK_STRUCT_FLD_S8(x) x
#define PACK_STRUCT_FLD_16(x) x
#define PACK_STRUCT_FLD_S16(x) x
#define PACK_STRUCT_FLD_32(x) x
#define PACK_STRUCT_FLD_S32(x) x

#endif
