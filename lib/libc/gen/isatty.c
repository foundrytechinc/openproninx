#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <errno.h>
#include <fnu/sys.h>
#include "posix.h"

// TIOCMGET and kin: modem lines, the system's numbers
_Static_assert(TIOCM_LE == FNU_TTYMODEM_LE && TIOCM_DTR == FNU_TTYMODEM_DTR &&
                   TIOCM_RTS == FNU_TTYMODEM_RTS && TIOCM_ST == FNU_TTYMODEM_ST &&
                   TIOCM_SR == FNU_TTYMODEM_SR && TIOCM_CTS == FNU_TTYMODEM_CTS &&
                   TIOCM_CD == FNU_TTYMODEM_CD && TIOCM_RI == FNU_TTYMODEM_RI &&
                   TIOCM_DSR == FNU_TTYMODEM_DSR,
               "modem lines");

// termios <-> fnu_ttymode, flag by flag. tedious
static const unsigned int imap[][2] = {
    {ISTRIP, FNU_TTYIN_STRIP},   {INLCR, FNU_TTYIN_NLTOCR},
    {IGNCR, FNU_TTYIN_DROPCR},   {ICRNL, FNU_TTYIN_CRTONL},
    {IXON, FNU_TTYIN_FLOWOUT},   {IXOFF, FNU_TTYIN_FLOWIN},
    {IXANY, FNU_TTYIN_ANYSTART}, {IMAXBEL, FNU_TTYIN_BELLFULL},
    {IGNBRK, FNU_TTYIN_IGNBREAK}, {BRKINT, FNU_TTYIN_BREAKINT},
    {IGNPAR, FNU_TTYIN_IGNPARITY}, {PARMRK, FNU_TTYIN_MARKPARITY},
    {INPCK, FNU_TTYIN_CHECKPARITY}, {IUCLC, FNU_TTYIN_TOLOWER},
};
static const unsigned int omap[][2] = {
    {OPOST, FNU_TTYOUT_PROCESS}, {ONLCR, FNU_TTYOUT_NLTOCRNL},
    {OCRNL, FNU_TTYOUT_CRTONL},  {ONLRET, FNU_TTYOUT_NLRETURNS},
    {OXTABS, FNU_TTYOUT_TABS},   {ONOEOT, FNU_TTYOUT_NOEOT},
    {OLCUC, FNU_TTYOUT_TOUPPER}, {ONOCR, FNU_TTYOUT_NOCRCOL0},
};
static const unsigned int cmap[][2] = {
    {CSTOPB, FNU_TTYLINE_TWOSTOP}, {CREAD, FNU_TTYLINE_RECEIVE},
    {PARENB, FNU_TTYLINE_PARITY},  {PARODD, FNU_TTYLINE_ODD},
    {HUPCL, FNU_TTYLINE_HANGUP},   {CLOCAL, FNU_TTYLINE_LOCAL},
    {CCTS_OFLOW, FNU_TTYLINE_CTSFLOW}, {CRTS_IFLOW, FNU_TTYLINE_RTSFLOW},
    {CDSR_OFLOW, FNU_TTYLINE_DSRFLOW}, {CCAR_OFLOW, FNU_TTYLINE_CARRIERFLOW},
    {CIGNORE, FNU_TTYLINE_IGNORE},
};
static const unsigned int lmap[][2] = {
    {ECHOKE, FNU_TTYEDIT_ECHOKILLRUB}, {ECHOE, FNU_TTYEDIT_ECHOERASE},
    {ECHOK, FNU_TTYEDIT_ECHOKILL},     {ECHO, FNU_TTYEDIT_ECHO},
    {ECHONL, FNU_TTYEDIT_ECHONL},      {ECHOCTL, FNU_TTYEDIT_ECHOCTL},
    {ISIG, FNU_TTYEDIT_SIGNALS},       {ICANON, FNU_TTYEDIT_LINES},
    {IEXTEN, FNU_TTYEDIT_EXTENDED},    {TOSTOP, FNU_TTYEDIT_STOPBG},
    {NOFLSH, FNU_TTYEDIT_NOFLUSH},     {ECHOPRT, FNU_TTYEDIT_ECHOPRINT},
    {ALTWERASE, FNU_TTYEDIT_ALTWERASE}, {FLUSHO, FNU_TTYEDIT_FLUSHOUT},
    {PENDIN, FNU_TTYEDIT_PENDING},     {NOKERNINFO, FNU_TTYEDIT_NOSTATUS},
    {EXTPROC, FNU_TTYEDIT_EXTERNAL},   {XCASE, FNU_TTYEDIT_CASEESC},
};
static const unsigned char kmap[][2] = {
    {VEOF, FNU_TTYKEY_EOF},          {VEOL, FNU_TTYKEY_EOL},
    {VEOL2, FNU_TTYKEY_EOL2},        {VERASE, FNU_TTYKEY_ERASE},
    {VWERASE, FNU_TTYKEY_WORDERASE}, {VKILL, FNU_TTYKEY_KILL},
    {VREPRINT, FNU_TTYKEY_REPRINT},  {VERASE2, FNU_TTYKEY_ERASE2},
    {VINTR, FNU_TTYKEY_INTERRUPT},   {VQUIT, FNU_TTYKEY_QUIT},
    {VSUSP, FNU_TTYKEY_SUSPEND},     {VSTART, FNU_TTYKEY_START},
    {VSTOP, FNU_TTYKEY_STOP},        {VLNEXT, FNU_TTYKEY_LITERAL},
    {VDISCARD, FNU_TTYKEY_DISCARD},  {VSTATUS, FNU_TTYKEY_STATUS},
    {VDSUSP, FNU_TTYKEY_DELAYSUSP},
};

#define NMAP(m) (sizeof(m) / sizeof(m[0]))

static unsigned int bits(unsigned int v, const unsigned int (*m)[2], size_t n,
                         int to) {
  unsigned int out = 0;
  size_t i;

  for (i = 0; i < n; i++)
    if (v & m[i][!to])
      out |= m[i][to];
  return out;
}

static void mode_of(const struct termios *t, struct fnu_ttymode *m) {
  size_t i;

  memset(m, 0, sizeof(*m));
  m->in = bits(t->c_iflag, imap, NMAP(imap), 1);
  m->out = bits(t->c_oflag, omap, NMAP(omap), 1);
  m->line = bits(t->c_cflag, cmap, NMAP(cmap), 1) | ((t->c_cflag & CSIZE) >> 8);
  m->edit = bits(t->c_lflag, lmap, NMAP(lmap), 1);
  memset(m->key, FNU_TTYKEY_OFF, sizeof(m->key));
  for (i = 0; i < NMAP(kmap); i++)
    m->key[kmap[i][1]] = t->c_cc[kmap[i][0]];
  m->min = t->c_cc[VMIN];
  m->time = t->c_cc[VTIME];
  m->speed_in = (uint32_t)t->c_ispeed;
  m->speed_out = (uint32_t)t->c_ospeed;
}

static void termios_of(const struct fnu_ttymode *m, struct termios *t) {
  size_t i;

  memset(t, 0, sizeof(*t));
  t->c_iflag = bits(m->in, imap, NMAP(imap), 0);
  t->c_oflag = bits(m->out, omap, NMAP(omap), 0);
  t->c_cflag = bits(m->line, cmap, NMAP(cmap), 0) |
               ((m->line & FNU_TTYLINE_BITS) << 8);
  t->c_lflag = bits(m->edit, lmap, NMAP(lmap), 0);
  memset(t->c_cc, _POSIX_VDISABLE, sizeof(t->c_cc));
  for (i = 0; i < NMAP(kmap); i++)
    t->c_cc[kmap[i][0]] = m->key[kmap[i][1]];
  t->c_cc[VMIN] = m->min;
  t->c_cc[VTIME] = m->time;
  t->c_ispeed = (int)m->speed_in;
  t->c_ospeed = (int)m->speed_out;
}

// terminal calls with a struct; -2 for the rest
int __tty_ioctl(int fd, unsigned long req, void *arg) {
  struct fnu_ttymode m;
  struct fnu_ttysize z;
  struct winsize *w = arg;
  unsigned long n;

  switch (req) {
  case TIOCGETA:
    if (__posix(fnu_ioctl(fd, FNU_TTYGETMODE, (uint64_t)(uintptr_t)&m)) < 0)
      return -1;
    termios_of(&m, arg);
    return 0;
  case TIOCSETA:
  case TIOCSETAW:
  case TIOCSETAF:
    n = req == TIOCSETA    ? FNU_TTYSETMODE
        : req == TIOCSETAW ? FNU_TTYSETMODE_DRAIN
                           : FNU_TTYSETMODE_FLUSH;
    mode_of(arg, &m);
    return (int)__posix(fnu_ioctl(fd, n, (uint64_t)(uintptr_t)&m));
  case TIOCGWINSZ:
    if (__posix(fnu_ioctl(fd, FNU_TTYGETSIZE, (uint64_t)(uintptr_t)&z)) < 0)
      return -1;
    w->ws_row = z.rows;
    w->ws_col = z.cols;
    w->ws_xpixel = z.xpixels;
    w->ws_ypixel = z.ypixels;
    return 0;
  case TIOCSWINSZ:
    z.rows = w->ws_row;
    z.cols = w->ws_col;
    z.xpixels = w->ws_xpixel;
    z.ypixels = w->ws_ypixel;
    return (int)__posix(fnu_ioctl(fd, FNU_TTYSETSIZE, (uint64_t)(uintptr_t)&z));
  // one discipline. no other may be set
  case TIOCGETD:
  case TIOCSETD:
    if (__posix(fnu_ioctl(fd, FNU_TTYGETMODE, (uint64_t)(uintptr_t)&m)) < 0)
      return -1;
    if (req == TIOCGETD) {
      *(int *)arg = TTYDISC;
      return 0;
    }
    if (*(int *)arg != TTYDISC) {
      errno = ENXIO;
      return -1;
    }
    return 0;
  case TIOCSTART:
    return (int)__posix(fnu_ioctl(fd, FNU_TTYSTART, 0));
  case TIOCSTOP:
    return (int)__posix(fnu_ioctl(fd, FNU_TTYSTOP, 0));
  case TIOCSTAT:
    return (int)__posix(fnu_ioctl(fd, FNU_TTYSTATUS, 0));
  case TIOCEXT:
    return (int)__posix(fnu_ioctl(fd, FNU_TTYEXTERNAL, (uint64_t)(uintptr_t)arg));
  default:
    return -2;
  }
}

int isatty(int fd) {
  struct termios t;

  return ioctl(fd, TIOCGETA, (uint64_t)&t) == 0;
}

// the terminal's name in /dev, by the device it is
int ttyname_r(int fd, char *buf, size_t len) {
  struct stat st;
  char *dev;

  if (!isatty(fd))
    return errno;
  if (fstat(fd, &st) < 0)
    return errno;
  if ((dev = devname(st.st_rdev, S_IFCHR)) == 0 || strcmp(dev, "??") == 0)
    return ENOTTY;
  if ((size_t)snprintf(buf, len, "/dev/%s", dev) >= len)
    return ERANGE;
  return 0;
}

char *ttyname(int fd) {
  static char name[sizeof("/dev/") + NAME_MAX];
  int r;

  if ((r = ttyname_r(fd, name, sizeof(name))) != 0) {
    errno = r;
    return 0;
  }
  return name;
}

int tcgetattr(int fd, struct termios *t) {
  return ioctl(fd, TIOCGETA, (uint64_t)t);
}

int tcsetattr(int fd, int action, const struct termios *t) {
  unsigned long cmd = action == TCSAFLUSH ? TIOCSETAF
                      : action == TCSADRAIN ? TIOCSETAW
                                            : TIOCSETA;

  return ioctl(fd, cmd, (uint64_t)t);
}

int tcflush(int fd, int which) {
  int what = (which & TCIFLUSH ? FNU_FLUSH_INPUT : 0) | (which & TCOFLUSH ? FNU_FLUSH_OUTPUT : 0);

  return ioctl(fd, TIOCFLUSH, (uint64_t)&what);
}

int tcdrain(int fd) { return ioctl(fd, TIOCDRAIN, 0); }

// break length; 0 is at least a quarter second
int tcsendbreak(int fd, int duration) {
  if (ioctl(fd, TIOCSBRK, 0) < 0)
    return -1;
  usleep(duration > 0 ? (unsigned)duration * 1000 : 400000);
  return ioctl(fd, TIOCCBRK, 0);
}

uint32_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
uint32_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }

int cfsetispeed(struct termios *t, uint32_t speed) {
  t->c_ispeed = speed;
  return 0;
}

int cfsetospeed(struct termios *t, uint32_t speed) {
  t->c_ospeed = speed;
  return 0;
}

int cfsetspeed(struct termios *t, uint32_t speed) {
  t->c_ispeed = t->c_ospeed = speed;
  return 0;
}

void cfmakeraw(struct termios *t) {
  t->c_iflag &= ~(IMAXBEL | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                  ICRNL | IXON);
  t->c_oflag &= ~OPOST;
  t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t->c_cflag &= ~(CSIZE | PARENB);
  t->c_cflag |= CS8;
  t->c_cc[VMIN] = 1;
  t->c_cc[VTIME] = 0;
}
