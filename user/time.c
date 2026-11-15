/*	$OpenBSD: time.c,v 1.26 2023/10/15 18:20:25 cheloha Exp $	*/
/*	$NetBSD: time.c,v 1.7 1995/06/27 00:34:00 jtc Exp $	*/

/*
 * Copyright (c) 1987, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern char *__progname;

__dead void usage(void);

int
main(int argc, char *argv[])
{
	struct rusage ru;
	struct timespec before, after, during;
	int ch, exitonsig, lflag, portableflag, status;
	pid_t pid;


	exitonsig = lflag = portableflag = 0;

	while ((ch = getopt(argc, argv, "lp")) != -1) {
		switch(ch) {
		case 'l':
			lflag = 1;
			break;
		case 'p':
			portableflag = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage();

	clock_gettime(CLOCK_MONOTONIC, &before);
	switch (pid = fork()) {
	case -1:			/* error */
		warn("fork");
		return 1;
	case 0:				/* child */
		execvp(*argv, argv);
		warn("%s", *argv);
		_exit(errno == ENOENT ? 127 : 126);
	}

	/* parent */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	while (wait3(&status, 0, &ru) != pid)
		continue;
	clock_gettime(CLOCK_MONOTONIC, &after);
	if (WIFSIGNALED(status))
		exitonsig = WTERMSIG(status);
	if (!WIFEXITED(status))
		fprintf(stderr, "Command terminated abnormally.\n");
	timespecsub(&after, &before, &during);

	if (portableflag) {
		fprintf(stderr, "real %9lld.%02ld\n",
		    (long long)during.tv_sec, during.tv_nsec / 10000000);
		fprintf(stderr, "user %9lld.%02ld\n",
		    (long long)ru.ru_utime.tv_sec, ru.ru_utime.tv_usec / 10000);
		fprintf(stderr, "sys  %9lld.%02ld\n",
		    (long long)ru.ru_stime.tv_sec, ru.ru_stime.tv_usec / 10000);
	} else {
		fprintf(stderr, "%9lld.%02ld real ",
		    (long long)during.tv_sec, during.tv_nsec / 10000000);
		fprintf(stderr, "%9lld.%02ld user ",
		    (long long)ru.ru_utime.tv_sec, ru.ru_utime.tv_usec / 10000);
		fprintf(stderr, "%9lld.%02ld sys\n",
		    (long long)ru.ru_stime.tv_sec, ru.ru_stime.tv_usec / 10000);
	}

	if (lflag) {
		fprintf(stderr, "%10ld  %s\n",
		    ru.ru_maxrss, "maximum resident set size");
		fprintf(stderr, "%10ld  %s\n",
		    ru.ru_nsignals, "signals received");
		fprintf(stderr, "%10ld  %s\n",
		    ru.ru_nvcsw, "voluntary context switches");
		fprintf(stderr, "%10ld  %s\n",
		    ru.ru_nivcsw, "involuntary context switches");
	}

	if (exitonsig) {
		if (signal(exitonsig, SIG_DFL) == SIG_ERR)
			return 128 + exitonsig;
		else
			raise(exitonsig);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-lp] utility [argument ...]\n",
	    __progname);
	exit(1);
}
