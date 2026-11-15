#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vi.h"

static struct sbuf term_sbuf;	/* output buffer if not NULL */
static int rows, cols;		/* number of terminal rows and columns */
static int win_top, win_rows;	/* active window rows */
static int win_left, win_cols;	/* active window columns */

void term_init(void)
{
	int r, c;
	sys_termraw();
	if (getenv("LINES"))
		rows = atoi(getenv("LINES"));
	if (getenv("COLUMNS"))
		cols = atoi(getenv("COLUMNS"));
	if (!sys_termsize(&r, &c)) {
		cols = c;
		rows = r;
	}
	cols = cols ? cols : 80;
	rows = rows ? rows : 25;
	term_str("\33[m");
	term_window(win_top, win_rows > 0 ? win_rows : rows);
}

void term_window(int row, int cnt)
{
	char cmd[64];
	win_top = row;
	win_rows = cnt;
	win_left = 0;
	win_cols = cols;
	if (row == 0 && win_rows == rows) {
		term_str("\33[r");
	} else {
		sprintf(cmd, "\33[%d;%dr", win_top + 1, win_top + win_rows);
		term_str(cmd);
	}
}

void term_done(void)
{
	term_str("\33[r");
	term_pos(rows - 1, 0);
	term_kill();
	term_commit();
	sbuf_free(&term_sbuf);
	sys_termrestore();
}

void term_suspend(void)
{
	term_done();
	sys_stop();
	term_init();
}

void term_commit(void)
{
	sys_write(1, sbuf_buf(&term_sbuf), sbuf_len(&term_sbuf));
	sbuf_cut(&term_sbuf, 0);
}

void term_str(char *s)
{
	sbuf_str(&term_sbuf, s);
}

void term_chr(int ch)
{
	char s[4] = {ch};
	term_str(s);
}

void term_kill(void)
{
	term_str("\33[K");
}

void term_room(int n)
{
	char cmd[16];
	if (n < 0)
		sprintf(cmd, "\33[%dM", -n);
	if (n > 0)
		sprintf(cmd, "\33[%dL", n);
	if (n)
		term_str(cmd);
}

void term_pos(int r, int c)
{
	char buf[32];
	if (c < 0)
		c = 0;
	if (c >= term_cols())
		c = win_cols - 1;
	if (r < 0)
		sprintf(buf, "\33[%dG", win_left + c + 1);
	else
		sprintf(buf, "\33[%d;%dH", win_top + r + 1, win_left + c + 1);
	term_str(buf);
}

int term_rowx(void)
{
	return rows;
}

int term_rows(void)
{
	return win_rows;
}

int term_cols(void)
{
	return win_cols;
}

static char istd[1024];		/* characters read from stdin */
static char ibuf[4096];		/* buffered characters (term_push) */
static char icmd[4096];		/* characters returned since the last term_cmd() */
static int istd_pos, istd_cnt;	/* istd[] position and length */
static int ibuf_pos, ibuf_cnt;	/* ibuf[] position and length */
static int icmd_pos;		/* icmd[] position */

/* read s before reading from the terminal */
void term_push(char *s, int n)
{
	int cur = ibuf_cnt - ibuf_pos;
	n = MIN((size_t)n, sizeof(ibuf) - cur);
	memmove(ibuf + n, ibuf + ibuf_pos, cur);
	memcpy(ibuf, s, n);
	ibuf_pos = 0;
	ibuf_cnt = cur + n;
}

/* drop all characters pushed via term_push() */
void term_pushstop(void)
{
	ibuf_pos = ibuf_cnt;
}

/* return a static buffer containing inputs read since the last term_cmd() */
char *term_cmd(int *n)
{
	*n = icmd_pos;
	icmd_pos = 0;
	return icmd;
}

int term_read(int buffered)
{
	struct sysfd in = {0, SYS_IN, 0};
	int n, c;
	if (!buffered && ibuf_pos >= ibuf_cnt && istd_pos >= istd_cnt) {
		if (sys_wait(&in, 1, -1) <= 0)
			return -1;
		if ((n = sys_read(0, istd, sizeof(istd))) <= 0)
			return -1;
		istd_cnt = n;
		istd_pos = 0;
	}
	if (ibuf_pos < ibuf_cnt)
		c = (unsigned char) ibuf[ibuf_pos++];
	else
		c = istd_pos < istd_cnt ? (unsigned char) istd[istd_pos++] : -1;
	if ((size_t)icmd_pos < sizeof(icmd) && c >= 0)
		icmd[icmd_pos++] = c;
	return c;
}

/* return a static string that changes text attributes from old to att */
char *term_seqattr(int att, int old)
{
	static char buf[128];
	char *s = buf;
	int fg = SYN_FG(att);
	int bg = SYN_BG(att);
	if (att == old)
		return "";
	s += sprintf(s, "\33[");
	if (att & SYN_BD)
		s += sprintf(s, ";1");
	if (att & SYN_IT)
		s += sprintf(s, ";3");
	if (att & SYN_RV)
		s += sprintf(s, ";7");
	if (SYN_FGSET(att)) {
		if ((fg & 0xff) < 8)
			s += sprintf(s, ";%d", 30 + (fg & 0xff));
		else
			s += sprintf(s, ";38;5;%d", (fg & 0xff));
	}
	if (SYN_BGSET(att)) {
		if ((bg & 0xff) < 8)
			s += sprintf(s, ";%d", 40 + (bg & 0xff));
		else
			s += sprintf(s, ";48;5;%d", (bg & 0xff));
	}
	s += sprintf(s, "m");
	return buf;
}

char *term_seqkill(void)
{
	return "\33[K";
}
