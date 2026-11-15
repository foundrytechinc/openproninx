#ifndef PRONINX_CDEFS_H
#define PRONINX_CDEFS_H

#define __dead __attribute__((__noreturn__))
#if __has_attribute(__nonstring__)
#define __nonstring __attribute__((__nonstring__))
#else
#define __nonstring
#endif
#define __packed __attribute__((__packed__))
#define __aligned(x) __attribute__((__aligned__(x)))
#define __unused __attribute__((__unused__))
#define __used __attribute__((__used__))
#define __noinline __attribute__((__noinline__))
#define __malloc __attribute__((__malloc__))
#define __predict_true(exp) __builtin_expect(((exp) != 0), 1)
#define __predict_false(exp) __builtin_expect(((exp) != 0), 0)
#define __offsetof(type, field) __builtin_offsetof(type, field)
#define __unreachable() __builtin_unreachable()

#define EFIAPI __attribute__((__ms_abi__))

#define __DECONST(type, var) ((type)(uintptr_t)(const void *)(var))
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))

#ifndef __P
#define __P(protos) protos
#endif
#ifndef __inline
#define __inline inline
#endif
#ifndef __restrict
#define __restrict restrict
#endif
#ifndef __CONCAT
#define __CONCAT(x, y) x##y
#endif
#ifndef __STRING
#define __STRING(x) #x
#endif
#define __weak_alias(alias, sym) \
  __asm__(".weak " __STRING(alias) " ; " __STRING(alias) " = " __STRING(sym))
#define __strong_alias(alias, sym) \
  __asm__(".global " __STRING(alias) " ; " __STRING(alias) " = " __STRING(sym))
#define __warn_references(sym, msg)
#define __dso_hidden __attribute__((__visibility__("hidden")))
#define __FBSDID(s)
#define __RCSID(s)
#define __SCCSID(s)
#define __BEGIN_HIDDEN_DECLS
#define __END_HIDDEN_DECLS
#define __BEGIN_DECLS
#define __END_DECLS
#define __bounded__(a, b, c)
#define __pure __attribute__((__const__))
#define __GNUC_PREREQ__(ma, mi) \
  ((__GNUC__ > (ma)) || (__GNUC__ == (ma) && __GNUC_MINOR__ >= (mi)))
#define __dead2 __attribute__((__noreturn__))
#define __printf0like(fmtarg, firstvararg) __attribute__((__format__(__printf0__, fmtarg, firstvararg)))
#define __printflike(fmtarg, firstvararg) __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#define __scanflike(fmtarg, firstvararg) __attribute__((__format__(__scanf__, fmtarg, firstvararg)))
#ifndef __nonnull
#define __nonnull(x) __attribute__((__nonnull__(x)))
#endif

#endif
