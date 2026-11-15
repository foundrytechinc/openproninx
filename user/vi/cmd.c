#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

/*
 * Execute a shell command.
 *
 * If ibuf is given, it is passed as standard input to the process.
 * Otherwise, the process reads from the terminal.
 *
 * If oproc is 0, the process writes directly to the terminal.  If it
 * is 1, process' output is saved and returned.  If it is 2, in addition
 * to returning the output, it is written to the terminal.
 */
char *cmd_pipe(char *cmd, char *ibuf, int oproc)
{
	char *shell = getenv("SHELL");
	char *argv[] = {shell && *shell ? shell : "sh", "-c", cmd, NULL};
	struct sysfd fds[4];
	struct sbuf isb = {0};
	struct sbuf osb = {0};
	char buf[512];
	int ifd = -1, ofd = -1;
	long ipos = 0, opos = 0;
	int eof = 0;
	int lines = !ibuf && sys_isterm(0);
	int pid = sys_spawn(argv, &ifd, &ofd, -1, 1);
	if (pid <= 0)
		return NULL;
	if (lines)
		sys_termlines(1);
	memset(fds, 0, sizeof(fds));
	fds[0].fd = ofd;
	fds[1].fd = ifd;
	fds[0].want = SYS_IN;
	fds[2].fd = 0;
	fds[3].fd = oproc != 1 ? 1 : -1;
	fds[2].want = SYS_IN;
	sbuf_str(&isb, ibuf ? ibuf : "");
	while (fds[0].fd >= 0 || fds[1].fd >= 0 || (fds[3].fd >= 0 && opos < sbuf_len(&osb))) {
		if ((ibuf || eof || fds[2].fd < 0) && ipos == sbuf_len(&isb) && fds[1].fd >= 0) {
			sys_close(fds[1].fd);
			fds[1].fd = -1;
		}
		fds[1].want = ipos < sbuf_len(&isb) ? SYS_OUT : 0;
		fds[3].want = opos < sbuf_len(&osb) ? SYS_OUT : 0;
		if (sys_wait(fds, 4, -1) < 0)
			break;
		if (fds[0].got & SYS_IN) {
			long ret = sys_read(fds[0].fd, buf, sizeof(buf));
			if (ret > 0)
				sbuf_mem(&osb, buf, ret);
			if (ret <= 0) {
				sys_close(fds[0].fd);
				fds[0].fd = -1;
			}
		} else if (fds[0].got & SYS_GONE) {
			sys_close(fds[0].fd);
			fds[0].fd = -1;
		}
		if (fds[1].got & SYS_OUT) {
			long ret = sys_write_some(fds[1].fd, sbuf_buf(&isb) + ipos, sbuf_len(&isb) - ipos);
			if (ret > 0)
				ipos += ret;
			if (ret < 0) {
				sys_close(fds[1].fd);
				fds[1].fd = -1;
			}
		} else if (fds[1].got & SYS_GONE) {
			sys_close(fds[1].fd);
			fds[1].fd = -1;
		}
		if (fds[2].got & SYS_IN) {
			long ret = sys_read(fds[2].fd, buf, sizeof(buf));
			if (!ibuf && ret > 0)
				sbuf_mem(&isb, buf, ret);
			if (ret > 0 && memchr(buf, TK_CTL('c'), ret))
				sys_interrupt(pid);
			if (!ret && !ibuf)
				eof = 1;
			if (ret < 0)
				fds[2].fd = -1;
		} else if (fds[2].got & SYS_GONE) {
			fds[2].fd = -1;
		}
		if (fds[3].got & SYS_OUT) {
			long ret = sys_write_some(fds[3].fd, sbuf_buf(&osb) + opos, sbuf_len(&osb) - opos);
			if (ret > 0)
				opos += ret;
			if (ret <= 0)
				fds[3].fd = -1;
		} else if (fds[3].got & SYS_GONE) {
			fds[3].fd = -1;
		}
	}
	sys_close(fds[0].fd);
	sys_close(fds[1].fd);
	sys_reap(pid, 1);
	sbuf_free(&isb);
	if (lines)
		sys_termlines(0);
	if (oproc)
		return sbuf_done(&osb);
	sbuf_free(&osb);
	return NULL;
}

int cmd_exec(char *cmd)
{
	cmd_pipe(cmd, NULL, 0);
	return 0;
}
