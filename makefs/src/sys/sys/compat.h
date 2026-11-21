/*
 * Copyright (c) 2022 Tomohiro Kusumi <tkusumi@netbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _MAKEFS_COMPAT_H
#define _MAKEFS_COMPAT_H

#include <sys/types.h>

/* sys/sys/cdefs.h */
#ifndef __printflike
#define __printflike(fmtarg, firstvararg)
#endif

#ifndef __predict_false
#define __predict_false(exp)	(exp)
#endif

#ifndef __offsetof
#define __offsetof(type, field)	offsetof(type, field)
#endif

#ifndef __restrict
#define __restrict
#endif

#ifndef __RCSID
#define __RCSID(s)	struct __hack
#endif

/*
 * Note that some Linux headers have a struct field named __unused.
 * e.g. <linux/sysctl.h> included by <sys/sysctl.h>.
 * e.g. <bits/stat.h> included by <sys/stat.h>.
 * <sys/compat.h> should be included after such headers that cause conflicts.
 */
#ifndef __unused
#define __unused	__attribute__((unused))
#endif

#ifndef __dead
#define __dead		__attribute__((__noreturn__))
#endif

#ifndef __packed
#define __packed	__attribute__((__packed__))
#endif

#ifndef __noinline
#define __noinline	__attribute__ ((__noinline__))
#endif

/* sys/sys/limits.h */
#ifndef GID_MAX
#define	GID_MAX		UINT_MAX	/* max value for a gid_t */
#endif

#ifndef UID_MAX
#define	UID_MAX		UINT_MAX	/* max value for a uid_t */
#endif

/* sys/sys/stat.h */
#ifndef UF_SETTABLE
#define	UF_SETTABLE	0x0000ffff	/* mask of owner changeable flags */
#define	UF_NODUMP	0x00000001	/* do not dump file */
#define	UF_IMMUTABLE	0x00000002	/* file may not be changed */
#define	UF_APPEND	0x00000004	/* writes to file may only append */
#define	UF_OPAQUE	0x00000008	/* directory is opaque wrt. union */
#define	UF_NOUNLINK	0x00000010	/* file may not be removed or renamed */

#define	UF_SYSTEM	0x00000080	/* Windows system file bit */
#define	UF_SPARSE	0x00000100	/* sparse file */
#define	UF_OFFLINE	0x00000200	/* file is offline */
#define	UF_REPARSE	0x00000400	/* Windows reparse point file bit */
#define	UF_ARCHIVE	0x00000800	/* file needs to be archived */
#define	UF_READONLY	0x00001000	/* Windows readonly file bit */
#define	UF_HIDDEN	0x00008000	/* file is hidden */
#endif

#ifndef SF_SETTABLE
#define	SF_SETTABLE	0xffff0000	/* mask of superuser changeable flags */
#define	SF_ARCHIVED	0x00010000	/* file is archived */
#define	SF_IMMUTABLE	0x00020000	/* file may not be changed */
#define	SF_APPEND	0x00040000	/* writes to file may only append */
#define	SF_NOUNLINK	0x00100000	/* file may not be removed or renamed */
#define	SF_SNAPSHOT	0x00200000	/* snapshot inode */
#endif

/* sys/sys/param.h */
#ifndef MAXPHYS
#define MAXPHYS		(1024 * 1024)
#endif

#ifndef MAXBSIZE
#define MAXBSIZE	65536	/* must be power of 2 */
#endif

#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

#ifndef __arraycount
#define __arraycount(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

#ifndef roundup
#define roundup(x, y)	((((x)+((y)-1))/(y))*(y))  /* to any y */
#endif

#ifndef roundup2
#define roundup2	roundup
#endif

#ifndef rounddown
#define rounddown(x, y)	(((x)/(y))*(y))
#endif

#ifndef rounddown2
#define rounddown2	rounddown
#endif

#if defined __linux__ || defined __CYGWIN__
#include <stddef.h> /* size_t */
#include <sys/stat.h> /* S_ISVTX / uid_t / gid_t */

#define S_ISTXT		S_ISVTX

/* include/pwd.h */
const char	*user_from_uid(uid_t, int);
int		 uid_from_user(const char *, uid_t *);

/* include/grp.h */
const char	*group_from_gid(gid_t, int);
int		 gid_from_group(const char *, gid_t *);

/* include/stdlib.h */
const char *getprogname(void);
void setprogname(const char *);

/* include/string.h */
size_t strlcat(char * __restrict, const char * __restrict, size_t);
size_t strlcpy(char * __restrict, const char * __restrict, size_t);
#endif

#if defined __linux__ || defined __CYGWIN__ || defined __OpenBSD__
#include <pwd.h>
#include <grp.h>

/* include/pwd.h */
static inline int
pwcache_userdb(
	int		(*a_setpassent)(int),
	void		(*a_endpwent)(void),
	struct passwd *	(*a_getpwnam)(const char *),
	struct passwd *	(*a_getpwuid)(uid_t))
{
	return (0);
}

/* include/grp.h */
static inline int
pwcache_groupdb(
	int		(*a_setgroupent)(int),
	void		(*a_endgrent)(void),
	struct group *	(*a_getgrnam)(const char *),
	struct group *	(*a_getgrgid)(gid_t))
{
	return (0);
}
#endif

/* include/unistd.h */
#ifdef __OpenBSD__
char	*fflagstostr(u_int32_t);
int	 strtofflags(char **, u_int32_t *, u_int32_t *);
#else
char	*fflagstostr(u_long);
int	 strtofflags(char **, u_long *, u_long *);
#endif
mode_t	 getmode(const void *, mode_t);
void	*setmode(const char *);

/* lib/libnetbsd/stdlib.h */
long long strsuftoll(const char *, const char *, long long, long long);

#endif /* _MAKEFS_COMPAT_H */
