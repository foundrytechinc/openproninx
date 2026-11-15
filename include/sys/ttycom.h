#ifndef _SYS_TTYCOM_H_
#define _SYS_TTYCOM_H_

#include "inc/ioccom.h"

// window size and the terminal calls that take a struct; libc maps them to the system's own
struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

struct termios;
#define TIOCGETA _IOR('t', 19, struct termios)
#define TIOCSETA _IOW('t', 20, struct termios)
#define TIOCSETAW _IOW('t', 21, struct termios)
#define TIOCSETAF _IOW('t', 22, struct termios)
#define TIOCSWINSZ _IOW('t', 103, struct winsize)
#define TIOCGWINSZ _IOR('t', 104, struct winsize)
#define TIOCGETD _IOR('t', 26, int)
#define TIOCSETD _IOW('t', 27, int)
#define TIOCSTART _IO('t', 110)
#define TIOCSTOP _IO('t', 111)
#define TIOCSTAT _IO('t', 101)
#define TIOCEXT _IOW('t', 96, int)

// the one discipline there is; the rest are named so they can be refused
#define TTYDISC 0
#define TABLDISC 3
#define SLIPDISC 4
#define PPPDISC 5
#define STRIPDISC 6
#define NMEADISC 7
#define MSTSDISC 8
#define ENDRUNDISC 9

// calls the system names its own way
#define TIOCFLUSH FNU_TTYFLUSH
#define TIOCSCTTY FNU_TTYCONTROL
#define TIOCGSID FNU_TTYGETSESSION
#define TIOCSPGRP FNU_TTYSETGROUP
#define TIOCGPGRP FNU_TTYGETGROUP
#define TIOCDRAIN FNU_TTYDRAIN
#define TIOCMGET FNU_TTYGETLINES
#define TIOCMBIC FNU_TTYDROPLINES
#define TIOCMBIS FNU_TTYRAISELINES
#define TIOCMSET FNU_TTYSETLINES
#define TIOCOUTQ FNU_TTYOUTQ
#define TIOCCDTR FNU_TTYDROPDTR
#define TIOCSDTR FNU_TTYRAISEDTR
#define TIOCCBRK FNU_TTYENDBREAK
#define TIOCSBRK FNU_TTYBREAK

#endif
