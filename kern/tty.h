#ifndef PRONINX_TTY_H
#define PRONINX_TTY_H

#include "inc/abi.h"
#include "inc/types.h"
#include "selinfo.h"
#include "mutex.h"

struct uio;

#define TTYQ_SIZE 1024
#define TTY_QUOTE 0x100
#define TTY_CHARMASK 0xff
#define TTY_FE 0x01000000
#define TTY_PE 0x02000000
#define TTY_ERRORMASK (TTY_FE | TTY_PE)

struct ttyq {
  uint16_t c[TTYQ_SIZE];
  int first;
  int cc;
};

#define TS_ISOPEN 0x00001
#define TS_CARR_ON 0x00002
#define TS_BUSY 0x00004
#define TS_ASLEEP 0x00008
#define TS_TTSTOP 0x00010
#define TS_TBLOCK 0x00020
#define TS_ERASE 0x00040
#define TS_LNCH 0x00080
#define TS_TYPEN 0x00100
#define TS_CNTTB 0x00200
#define TS_BKSL 0x00400
#define TS_ZOMBIE 0x00800

struct tty {
  int t_unit;
  struct fnu_ttymode t_mode;
  struct ttyq t_rawq;
  struct ttyq t_canq;
  struct ttyq t_outq;
  int t_state;
  int t_column;
  int t_rocount, t_rocol;
  int t_hiwat, t_lowat;
  int t_gen; // moves on every close
  pid_t t_pgrp;
  pid_t t_session;
  int64_t t_inputat;
  struct fnu_ttysize t_winsize;
  void (*t_oproc)(struct tty *);
  void (*t_stop)(struct tty *, int);
  int (*t_param)(struct tty *, const struct fnu_ttymode *);
  void *t_sc;
  struct selinfo t_rsel, t_wsel;
};

extern struct mutex tty_lock;
extern struct tty *cn_tp; // /dev/console

void tty_init(void);
void tty_attach(struct tty *tp, void (*oproc)(struct tty *),
                void (*stop)(struct tty *, int),
                int (*param)(struct tty *, const struct fnu_ttymode *), void *sc);
void tty_defaults(struct fnu_ttymode *t);
void ttyopen(struct tty *tp);
void ttyclose(struct tty *tp);
void ttylclose(struct tty *tp);
int ttyinput(int c, struct tty *tp);
int ttyoutput(int c, struct tty *tp);
int ttread(struct tty *tp, struct uio *uio, int nonblock);
int ttwrite(struct tty *tp, struct uio *uio, int nonblock);
int ttioctl(struct tty *tp, uint64_t cmd, uint64_t arg);
int ttymodem(struct tty *tp, int carrier);
void ttyflush(struct tty *tp, int rw);
void ttstart(struct tty *tp);
void ttwakeup(struct tty *tp);
void ttwakeupwr(struct tty *tp);
int ttnread(struct tty *tp);
int tty_readable(struct tty *tp);
int tty_writable(struct tty *tp);
pid_t tty_leave_session(struct tty *tp, pid_t sid);
int tty_getc(struct ttyq *q);
int tty_putc(int c, struct ttyq *q);

// FNU_TTYIOCTL_PASS
#define TTY_PASS (-1)

#endif
