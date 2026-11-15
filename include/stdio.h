#ifndef _STDIO_H_
#define _STDIO_H_

#include <sys/types.h>
#include <stdarg.h>
#include <sys/cdefs.h>

#define	_FSTDIO			/* Define for new stdio with functions. */

typedef off_t fpos_t;		/* stdio file position type */

/*
 * NB: to fit things in six character monocase externals, the stdio
 * code uses the prefix `__s' for stdio objects, typically followed
 * by a three-character attempt at a mnemonic.
 */

/*
 * stdio state variables.
 *
 * The following always hold:
 *
 *	if (_flags&(__SLBF|__SWR)) == (__SLBF|__SWR),
 *		_lbfsize is -_bf._size, else _lbfsize is 0
 *	if _flags&__SRD, _w is 0
 *	if _flags&__SWR, _r is 0
 *
 * This ensures that the getc and putc macros (or inline functions) never
 * try to write or read from a file that is in `read' or `write' mode.
 * (Moreover, they can, and do, automatically switch from read mode to
 * write mode, and back, on "r+" and "w+" files.)
 *
 * _lbfsize is used only to make the inline line-buffered output stream
 * code as compact as possible.
 *
 * _ub, _up, and _ur are used when ungetc() pushes back more characters
 * than fit in the current _bf, or when ungetc() pushes back a character
 * that does not match the previous one in _bf.  When this happens,
 * _ub._base becomes non-nil (i.e., a stream has ungetc() data iff
 * _ub._base!=NULL) and _up and _ur save the current values of _p and _r.
 */
#ifndef	_STDFILES_DECLARED
#define	_STDFILES_DECLARED
typedef	struct __sFILE FILE;
struct __sFstub { long _stub; };

__BEGIN_DECLS
extern struct __sFstub __stdin[];
extern struct __sFstub __stdout[];
extern struct __sFstub __stderr[];
__END_DECLS
#endif

#define	__SLBF	0x0001		/* line buffered */
#define	__SNBF	0x0002		/* unbuffered */
#define	__SRD	0x0004		/* OK to read */
#define	__SWR	0x0008		/* OK to write */
	/* RD and WR are never simultaneously asserted */
#define	__SRW	0x0010		/* open for reading & writing */
#define	__SEOF	0x0020		/* found EOF */
#define	__SERR	0x0040		/* found error */
#define	__SMBF	0x0080		/* _buf is from malloc */
#define	__SAPP	0x0100		/* fdopen()ed in append mode */
#define	__SSTR	0x0200		/* this is an sprintf/snprintf string */
#define	__SOPT	0x0400		/* do fseek() optimisation */
#define	__SNPT	0x0800		/* do not do fseek() optimisation */
#define	__SOFF	0x1000		/* set iff _offset is in fact correct */
#define	__SMOD	0x2000		/* true => fgetln modified _p text */
#define	__SALC	0x4000		/* allocate string space dynamically */

/*
 * The following three definitions are for ANSI C, which took them
 * from System V, which brilliantly took internal interface macros and
 * made them official arguments to setvbuf(), without renaming them.
 * Hence, these ugly _IOxxx names are *supposed* to appear in user code.
 *
 * Although numbered as their counterparts above, the implementation
 * does not rely on this.
 */
#define	_IOFBF	0		/* setvbuf should set fully buffered */
#define	_IOLBF	1		/* setvbuf should set line buffered */
#define	_IONBF	2		/* setvbuf should set unbuffered */

#define	BUFSIZ	1024		/* size of buffer used by setbuf */

#define	EOF	(-1)

/*
 * FOPEN_MAX is a minimum maximum, and should be the number of descriptors
 * that the kernel can provide without allocation of a resource that can
 * fail without the process sleeping.  Do not use this for anything.
 */
#define	FOPEN_MAX	20	/* must be <= OPEN_MAX <sys/syslimits.h> */
#define	FILENAME_MAX	1024	/* must be <= PATH_MAX <sys/syslimits.h> */

/* System V/ANSI C; this is the wrong way to do this, do *not* use these. */
#if __BSD_VISIBLE || __XPG_VISIBLE
#define	P_tmpdir	"/tmp/"
#endif
#define	L_tmpnam	1024	/* XXX must be == PATH_MAX */
#define	TMP_MAX		308915776

#ifndef SEEK_SET
#define	SEEK_SET	0	/* set file offset to offset */
#endif
#ifndef SEEK_CUR
#define	SEEK_CUR	1	/* current plus offset */
#endif
#ifndef SEEK_END
#define	SEEK_END	2	/* EOF plus offset */
#endif

#define	stdin	((struct __sFILE *)__stdin)
#define	stdout	((struct __sFILE *)__stdout)
#define	stderr	((struct __sFILE *)__stderr)

/*
 * Functions defined in ANSI C standard.
 */
__BEGIN_DECLS
void	 clearerr(FILE *);
int	 fclose(FILE *);
int	 feof(FILE *);
int	 ferror(FILE *);
int	 fflush(FILE *);
int	 fpurge(FILE *);
int	 fgetc(FILE *);
int	 fgetpos(FILE *, fpos_t *);
char	*fgets(char *, int, FILE *);
FILE	*fopen(const char *, const char *);
int	 fprintf(FILE *, const char *, ...)
		__printflike(2, 3);
int	 fputc(int, FILE *);
int	 fputs(const char *, FILE *);
size_t	 fread(void *, size_t, size_t, FILE *);
FILE	*freopen(const char *, const char *, FILE *);
int	 fscanf(FILE *, const char *, ...)
		__scanflike(2, 3);
int	 fseek(FILE *, long, int);
int	 fseeko(FILE *, off_t, int);
int	 fsetpos(FILE *, const fpos_t *);
long	 ftell(FILE *);
off_t	 ftello(FILE *);
size_t	 fwrite(const void *, size_t, size_t, FILE *);
int	 getc(FILE *);
int	 getchar(void);
char	*gets(char *);
#if __BSD_VISIBLE && !defined(__SYS_ERRLIST)
#define __SYS_ERRLIST

extern int sys_nerr;			/* perror(3) external variables */
extern char *sys_errlist[];
#endif
void	 perror(const char *);
int	 printf(const char *, ...)
		__printflike(1, 2);
int	 putc(int, FILE *);
int	 putchar(int);
int	 puts(const char *);
int	 remove(const char *);
int	 rename(const char *, const char *);
int	 renameat(int, const char *, int, const char *);
void	 rewind(FILE *);
int	 scanf(const char *, ...)
		__scanflike(1, 2);
void	 setbuf(FILE *, char *);
int	 setvbuf(FILE *, char *, int, size_t);
int	 sprintf(char *, const char *, ...)
		__printflike(2, 3);
int	 sscanf(const char *, const char *, ...)
		__scanflike(2, 3);
FILE	*tmpfile(void);
char	*tmpnam(char *);
int	 ungetc(int, FILE *);
int	 vfprintf(FILE *, const char *, va_list)
		__printflike(2, 0);
int	 vprintf(const char *, va_list)
		__printflike(1, 0);
int	 vsprintf(char *, const char *, va_list)
		__printflike(2, 0);

#if __ISO_C_VISIBLE >= 1999 || __BSD_VISIBLE
int	 snprintf(char *, size_t, const char *, ...)
		__printflike(3, 4)
		__nonnull(3);
int	 vfscanf(FILE *, const char *, va_list)
		__scanflike(2, 0)
		__nonnull(2);
int	 vscanf(const char *, va_list)
		__scanflike(1, 0)
		__nonnull(1);
int	 vsnprintf(char *, size_t, const char *, va_list)
		__printflike(3, 0)
		__nonnull(3);
int	 vsscanf(const char *, const char *, va_list)
		__scanflike(2, 0)
		__nonnull(2);
#endif /* __ISO_C_VISIBLE >= 1999 || __BSD_VISIBLE */

__END_DECLS


/*
 * Functions defined in POSIX 1003.1.
 */
#if __BSD_VISIBLE || __POSIX_VISIBLE || __XPG_VISIBLE
#define	L_ctermid	1024	/* size for ctermid(); PATH_MAX */
#define L_cuserid	9	/* size for cuserid(); UT_NAMESIZE + 1 */

__BEGIN_DECLS
char	*ctermid(char *);
char	*cuserid(char *);
FILE	*fdopen(int, const char *);
int	 fileno(FILE *);

#if __POSIX_VISIBLE >= 199209
int	 pclose(FILE *);
FILE	*popen(const char *, const char *);
#endif

#if __POSIX_VISIBLE >= 199506

/*
 * These are normally used through macros as defined below, but POSIX
 * requires functions as well.
 */
int	 getc_unlocked(FILE *);
int	 getchar_unlocked(void);
int	 putc_unlocked(int, FILE *);
int	 putchar_unlocked(int);
#endif /* __POSIX_VISIBLE >= 199506 */

#if __XPG_VISIBLE
char	*tempnam(const char *, const char *);
#endif
__END_DECLS

#endif /* __BSD_VISIBLE || __POSIX_VISIBLE || __XPG_VISIBLE */

/*
 * Routines that are purely local.
 */
#if __BSD_VISIBLE
__BEGIN_DECLS
int	 asprintf(char **, const char *, ...)
		__printflike(2, 3)
		__nonnull(2);
int	 vasprintf(char **, const char *, va_list);
int	 dprintf(int, const char *, ...)
		__printflike(2, 3);
int	 vdprintf(int, const char *, va_list)
		__printflike(2, 0);
ssize_t	 getdelim(char **, size_t *, int, FILE *);
ssize_t	 getline(char **, size_t *, FILE *);
char	*fgetln(FILE *, size_t *);
int	 setlinebuf(FILE *);
__END_DECLS

/*
 * Stdio function-access interface.
 */
__BEGIN_DECLS
FILE	*funopen(const void *,
		int (*)(void *, char *, int),
		int (*)(void *, const char *, int),
		fpos_t (*)(void *, fpos_t, int),
		int (*)(void *));
__END_DECLS
#define	fropen(cookie, fn) funopen(cookie, fn, 0, 0, 0)
#define	fwopen(cookie, fn) funopen(cookie, 0, fn, 0, 0)
#endif /* __BSD_VISIBLE */

/*
 * Functions internal to the implementation.
 */
__BEGIN_DECLS
int	__srget(FILE *);
int	__swbuf(int, FILE *);
__END_DECLS

/*
 * getc, putc, feof and their kin are functions: the stream is opaque, as it
 * is in the libc these came from
 */
#define	getchar_unlocked()	getc_unlocked(stdin)
#define	putchar_unlocked(c)	putc_unlocked(c, stdout)

#endif /* _STDIO_H_ */
