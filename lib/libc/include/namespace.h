#ifndef _LIBC_NAMESPACE_H_
#define _LIBC_NAMESPACE_H_

// static libc: no hidden second names
#define PROTO_NORMAL(x)
#define PROTO_STD_DEPRECATED(x)
#define PROTO_DEPRECATED(x)
#define DEF_WEAK(x)
#define DEF_STRONG(x)
#define DEF_BUILTIN(x)
#define MAKE_CLONE(dst, src) __typeof__(dst) dst __attribute__((alias(#src)))
#define WRAP(x) x
#ifndef __ASSEMBLER__
int __pledge_open(const char *path, int flags);
#endif

#endif
