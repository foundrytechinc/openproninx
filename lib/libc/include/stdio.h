/* Inside libc the stream is not opaque: this wrapper opens it up. */
#ifndef _LIBC_STDIO_H_
#define _LIBC_STDIO_H_

#ifndef _STDFILES_DECLARED
#define _STDFILES_DECLARED
typedef struct __sFILE FILE;
#endif

#include_next <stdio.h>
#include <_stdio.h>		/* struct __sFILE, std{in,out,err} */

// inside libc the stream is reached straight
#define	__sgetc(p) (--(p)->_r < 0 ? __srget(p) : (int)(*(p)->_p++))
static __inline int __sputc(int _c, FILE *_p) {
	if (--_p->_w >= 0 || (_p->_w >= _p->_lbfsize && (char)_c != '\n'))
		return (*_p->_p++ = _c);
	else
		return (__swbuf(_c, _p));
}

#define	__sfeof(p)	(((p)->_flags & __SEOF) != 0)
#define	__sferror(p)	(((p)->_flags & __SERR) != 0)
#define	__sclearerr(p)	((void)((p)->_flags &= ~(__SERR|__SEOF)))
#define	__sfileno(p)	((p)->_file)

extern int __isthreaded;

#define feof(p)		(!__isthreaded ? __sfeof(p) : (feof)(p))
#define ferror(p)	(!__isthreaded ? __sferror(p) : (ferror)(p))
#define clearerr(p)	(!__isthreaded ? __sclearerr(p) : (clearerr)(p))
#define fileno(p)	(!__isthreaded ? __sfileno(p) : (fileno)(p))
#define getc(fp)	(!__isthreaded ? __sgetc(fp) : (getc)(fp))

/*
 * The macro implementations of putc and putc_unlocked are not fully POSIX
 * compliant; they do not set errno on failure
 */
#define putc(x, fp)	(!__isthreaded ? __sputc(x, fp) : (putc)(x, fp))
#define getc_unlocked(fp)	__sgetc(fp)
#define putc_unlocked(x, fp)	__sputc(x, fp)

#endif /* _LIBC_STDIO_H_ */
