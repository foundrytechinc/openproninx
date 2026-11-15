#ifndef _TERMIOS_H_
#define _TERMIOS_H_

#include <sys/types.h>

// struct termios and its numbers; libc turns them into a struct fnu_ttymode
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 20

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_cc[NCCS];
  int c_ispeed;
  int c_ospeed;
};

#define VEOF 0
#define VEOL 1
#define VEOL2 2
#define VERASE 3
#define VWERASE 4
#define VKILL 5
#define VREPRINT 6
#define VERASE2 7
#define VINTR 8
#define VQUIT 9
#define VSUSP 10
#define VDSUSP 11
#define VSTART 12
#define VSTOP 13
#define VLNEXT 14
#define VDISCARD 15
#define VMIN 16
#define VTIME 17
#define VSTATUS 18
#define _POSIX_VDISABLE 0xff
#define VDISABLE _POSIX_VDISABLE

#define IGNBRK 0x00000001
#define BRKINT 0x00000002
#define IGNPAR 0x00000004
#define PARMRK 0x00000008
#define INPCK 0x00000010
#define ISTRIP 0x00000020
#define INLCR 0x00000040
#define IGNCR 0x00000080
#define ICRNL 0x00000100
#define IXON 0x00000200
#define IXOFF 0x00000400
#define IXANY 0x00000800
#define IUCLC 0x00001000
#define IMAXBEL 0x00002000

#define OPOST 0x00000001
#define ONLCR 0x00000002
#define TABDLY 0x00000004
#define TAB0 0x00000000
#define TAB3 0x00000004
#define OXTABS TAB3
#define ONOEOT 0x00000008
#define OCRNL 0x00000010
#define OLCUC 0x00000020
#define ONOCR 0x00000040
#define ONLRET 0x00000080

#define CIGNORE 0x00000001
#define CSIZE 0x00000300
#define CS5 0x00000000
#define CS6 0x00000100
#define CS7 0x00000200
#define CS8 0x00000300
#define CSTOPB 0x00000400
#define CREAD 0x00000800
#define PARENB 0x00001000
#define PARODD 0x00002000
#define HUPCL 0x00004000
#define CLOCAL 0x00008000
#define CCTS_OFLOW 0x00010000
#define CRTS_IFLOW 0x00020000
#define CRTSCTS (CCTS_OFLOW | CRTS_IFLOW)
#define CDSR_OFLOW 0x00080000
#define CCAR_OFLOW 0x00100000
#define MDMBUF CCAR_OFLOW
#define CHWFLOW (MDMBUF | CRTSCTS)

#define ECHOKE 0x00000001
#define ECHOE 0x00000002
#define ECHOK 0x00000004
#define ECHO 0x00000008
#define ECHONL 0x00000010
#define ECHOPRT 0x00000020
#define ECHOCTL 0x00000040
#define ISIG 0x00000080
#define ICANON 0x00000100
#define ALTWERASE 0x00000200
#define IEXTEN 0x00000400
#define EXTPROC 0x00000800
#define TOSTOP 0x00400000
#define FLUSHO 0x00800000
#define XCASE 0x01000000
#define NOKERNINFO 0x02000000
#define PENDIN 0x20000000
#define NOFLSH 0x80000000

#define B0 0
#define B50 50
#define B75 75
#define B110 110
#define B134 134
#define B150 150
#define B200 200
#define B300 300
#define B600 600
#define B1200 1200
#define B1800 1800
#define B2400 2400
#define B4800 4800
#define B9600 9600
#define B19200 19200
#define B38400 38400
#define B57600 57600
#define B115200 115200
#define B230400 230400
#define B460800 460800
#define B921600 921600

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCSASOFT 0x10
#define TCIFLUSH 1
#define TCOFLUSH 2
#define TCIOFLUSH 3

#define TIOCM_LE 0001
#define TIOCM_DTR 0002
#define TIOCM_RTS 0004
#define TIOCM_ST 0010
#define TIOCM_SR 0020
#define TIOCM_CTS 0040
#define TIOCM_CD 0100
#define TIOCM_CAR TIOCM_CD
#define TIOCM_RI 0200
#define TIOCM_RNG TIOCM_RI
#define TIOCM_DSR 0400

int tcsendbreak(int fd, int duration);
int tcdrain(int fd);

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);
int tcflush(int fd, int which);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
uint32_t cfgetispeed(const struct termios *t);
uint32_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, uint32_t speed);
int cfsetospeed(struct termios *t, uint32_t speed);
int cfsetspeed(struct termios *t, uint32_t speed);
void cfmakeraw(struct termios *t);

pid_t tcgetsid(int);
int tcsetsid(int, pid_t);
#include <sys/ttycom.h>
#include <sys/ttydefaults.h>

#endif
