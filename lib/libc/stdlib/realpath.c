/*	$OpenBSD: realpath.c,v 1.28 2023/05/18 16:11:10 guenther Exp $ */
/*
 * Copyright (c) 2019 Bob Beck <beck@openbsd.org>
 * Copyright (c) 2019 Theo de Raadt <deraadt@openbsd.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fnu/sys.h>
#include <stdlib.h>
#include "posix.h"
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <syslog.h>
#include <stdarg.h>

/*
 * wrapper for the kernel's pathof
 */

char *
realpath(const char *path, char *resolved)
{
	char rbuf[PATH_MAX];

	int error = fnu_pathof(FNU_FD_CWD, path, rbuf, sizeof(rbuf));

	if (error < 0) {
		errno = __errno_of(-error);
		return NULL;
	}
	if (resolved == NULL)
		return (strdup(rbuf));
	strlcpy(resolved, rbuf, PATH_MAX);
	return (resolved);
}
