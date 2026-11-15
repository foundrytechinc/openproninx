// THE TERMINAL LINE DISCIPLINE
#include "defs.h"
#include "inc/abi.h"
#include "inc/error.h"
#include "inc/open.h"
#include "inc/signal.h"
#include "mutex.h"
#include "param.h"
#include "proc.h"
#include "sched.h"
#include "tty.h"
#include "uio.h"

struct mutex tty_lock;
struct tty *cn_tp;

#define TTIPRI 25
#define TTOPRI 26
#define TTYHOG 1024
#define TTMINLOWAT 32
#define TTMAXLOWAT 256
#define TTMINHIWAT 100
#define TTHIWATMINSPACE 200
#define OBUFSIZ 100
#define C(x) ((x) - '@')

#define ISSET(t, f) ((t) & (f))
#define CCEQ(v, c) ((v) != FNU_TTYKEY_OFF && (v) == ((c) & 0xff))

#define E 0x00
#define O 0x80
#define ALPHA 0x40
#define CCLASSMASK 0x3f
#define ORDINARY 0
#define CONTROL 1
#define BACKSPACE 2
#define NEWLINE 3
#define TAB 4
#define VTAB 5
#define RETURN 6
#define CC CONTROL
#define BS BACKSPACE
#define CR RETURN
#define NA (ORDINARY | ALPHA)
#define NL NEWLINE
#define NO ORDINARY
#define TB TAB
#define VT VTAB

static const uchar char_type[256] = {
    E | CC, O | CC, O | CC, E | CC, O | CC, E | CC, E | CC, O | CC,
    O | BS, E | TB, E | NL, O | CC, E | VT, O | CR, O | CC, E | CC,
    O | CC, E | CC, E | CC, O | CC, E | CC, O | CC, O | CC, E | CC,
    E | CC, O | CC, O | CC, E | CC, O | CC, E | CC, E | CC, O | CC,
    O | NO, E | NO, E | NO, O | NO, E | NO, O | NO, O | NO, E | NO,
    E | NO, O | NO, O | NO, E | NO, O | NO, E | NO, E | NO, O | NO,
    E | NA, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA,
    O | NA, E | NA, E | NO, O | NO, E | NO, O | NO, O | NO, E | NO,
    O | NO, E | NA, E | NA, O | NA, E | NA, O | NA, O | NA, E | NA,
    E | NA, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA,
    E | NA, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA,
    O | NA, E | NA, E | NA, O | NO, E | NO, O | NO, O | NO, O | NA,
    E | NO, O | NA, O | NA, E | NA, O | NA, E | NA, E | NA, O | NA,
    O | NA, E | NA, E | NA, O | NA, E | NA, O | NA, O | NA, E | NA,
    O | NA, E | NA, E | NA, O | NA, E | NA, O | NA, O | NA, E | NA,
    E | NA, O | NA, O | NA, E | NO, O | NO, E | NO, E | NO, O | CC,
    [128 ... 255] = NA,
};

#define CCLASS(c) (char_type[(c) & TTY_CHARMASK] & CCLASSMASK)
#define ISALPHA(c) (char_type[(c) & TTY_CHARMASK] & ALPHA)

static int nowake;

void tty_init(void) { mtx_init(&tty_lock, "tty"); }

// THE QUEUES
int tty_putc(int c, struct ttyq *q) {
  if (q->cc >= TTYQ_SIZE)
    return -1;
  q->c[(q->first + q->cc++) % TTYQ_SIZE] = (uint16_t)c;
  return 0;
}

int tty_getc(struct ttyq *q) {
  int c;

  if (q->cc == 0)
    return -1;
  c = q->c[q->first];
  q->first = (q->first + 1) % TTYQ_SIZE;
  q->cc--;
  return c;
}

static int unputc(struct ttyq *q) {
  if (q->cc == 0)
    return -1;
  return q->c[(q->first + --q->cc) % TTYQ_SIZE];
}

static int nthc(struct ttyq *q, int i) { return q->c[(q->first + i) % TTYQ_SIZE]; }

static void flushq(struct ttyq *q) {
  q->first = 0;
  q->cc = 0;
}

static void catq(struct ttyq *from, struct ttyq *to) {
  int c;

  while ((c = tty_getc(from)) >= 0)
    if (tty_putc(c, to) < 0)
      break;
}

void tty_defaults(struct fnu_ttymode *t) {
  static const uint8 keys[FNU_TTYKEY_COUNT] = FNU_TTYDEF_KEYS;

  memset(t, 0, sizeof(*t));
  t->in = FNU_TTYDEF_IN;
  t->out = FNU_TTYDEF_OUT;
  t->line = FNU_TTYDEF_LINE;
  t->edit = FNU_TTYDEF_EDIT;
  memmove(t->key, keys, sizeof(keys));
  t->min = 1;
  t->time = 0;
  t->speed_in = t->speed_out = FNU_TTY_SPEED;
}

static void ttsetwater(struct tty *tp) {
  int cps = (int)(tp->t_mode.speed_out / 10), x;

#define CLAMP(x, h, l) ((x) > (h) ? (h) : (x) < (l) ? (l) : (x))
  tp->t_lowat = x = CLAMP(cps / 2, TTMAXLOWAT, TTMINLOWAT);
  x += cps;
  tp->t_hiwat = CLAMP(x, TTYQ_SIZE - TTHIWATMINSPACE, TTMINHIWAT);
#undef CLAMP
}

void tty_attach(struct tty *tp, void (*oproc)(struct tty *),
                void (*stop)(struct tty *, int),
                int (*param)(struct tty *, const struct fnu_ttymode *), void *sc) {
  memset(tp, 0, sizeof(*tp));
  tty_defaults(&tp->t_mode);
  tp->t_oproc = oproc;
  tp->t_stop = stop;
  tp->t_param = param;
  tp->t_sc = sc;
  tp->t_state = TS_CARR_ON;
  ttsetwater(tp);
}

void ttyopen(struct tty *tp) {
  if (!ISSET(tp->t_state, TS_ISOPEN)) {
    tp->t_state |= TS_ISOPEN;
    tp->t_column = 0;
  }
  tp->t_state &= ~TS_ZOMBIE;
}

void ttyclose(struct tty *tp) {
  ttyflush(tp, FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT);
  tp->t_gen++;
  tp->t_pgrp = 0;
  tp->t_session = 0;
  tp->t_state &= TS_CARR_ON;
}

void ttwakeup(struct tty *tp) {
  wakeup(&tp->t_rawq);
  selwakeup(&tp->t_rsel);
}

void ttwakeupwr(struct tty *tp) {
  if (tp->t_outq.cc <= tp->t_lowat) {
    if (ISSET(tp->t_state, TS_ASLEEP)) {
      tp->t_state &= ~TS_ASLEEP;
      wakeup(&tp->t_outq);
    }
    selwakeup(&tp->t_wsel);
  }
}

void ttstart(struct tty *tp) {
  if (tp->t_oproc != 0)
    tp->t_oproc(tp);
}

static void ttstop(struct tty *tp, int rw) {
  if (tp->t_stop != 0)
    tp->t_stop(tp, rw);
}

static void ttyunblock(struct tty *tp);

void ttyflush(struct tty *tp, int rw) {
  if (rw & FNU_FLUSH_INPUT) {
    flushq(&tp->t_canq);
    flushq(&tp->t_rawq);
    tp->t_rocount = 0;
    tp->t_rocol = 0;
    ttyunblock(tp);
    ttwakeup(tp);
    wakeup(&tp->t_canq);
  }
  if (rw & FNU_FLUSH_OUTPUT) {
    tp->t_state &= ~TS_TTSTOP;
    ttstop(tp, rw);
    flushq(&tp->t_outq);
    wakeup(&tp->t_outq);
    selwakeup(&tp->t_wsel);
  }
}

static void ttyblock(struct tty *tp) {
  int total = tp->t_rawq.cc + tp->t_canq.cc;

  if (tp->t_rawq.cc > TTYHOG) {
    ttyflush(tp, FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT);
    tp->t_state &= ~TS_TBLOCK;
  }
  if (((total >= TTYHOG / 2 && !ISSET(tp->t_state, TS_TBLOCK) &&
        !ISSET(tp->t_mode.edit, FNU_TTYEDIT_LINES)) ||
       tp->t_canq.cc > 0) &&
      ISSET(tp->t_mode.in, FNU_TTYIN_FLOWIN) &&
      tp->t_mode.key[FNU_TTYKEY_STOP] != FNU_TTYKEY_OFF &&
      tty_putc(tp->t_mode.key[FNU_TTYKEY_STOP], &tp->t_outq) == 0) {
    tp->t_state |= TS_TBLOCK;
    ttstart(tp);
  }
}

static void ttyunblock(struct tty *tp) {
  if (ISSET(tp->t_state, TS_TBLOCK) && ISSET(tp->t_mode.in, FNU_TTYIN_FLOWIN) &&
      tp->t_mode.key[FNU_TTYKEY_START] != FNU_TTYKEY_OFF &&
      tty_putc(tp->t_mode.key[FNU_TTYKEY_START], &tp->t_outq) == 0) {
    tp->t_state &= ~TS_TBLOCK;
    ttstart(tp);
  }
}

int ttyoutput(int c, struct tty *tp) {
  uint32_t oflag = tp->t_mode.out, lflag = tp->t_mode.edit;
  int col, notout, c2;

  if (!ISSET(oflag, FNU_TTYOUT_PROCESS)) {
    if (!ISSET(lflag, FNU_TTYEDIT_FLUSHOUT) && tty_putc(c & TTY_CHARMASK, &tp->t_outq))
      return c;
    return -1;
  }
  c &= TTY_CHARMASK;
  if (c == '\t' && ISSET(oflag, FNU_TTYOUT_TABS) && !ISSET(lflag, FNU_TTYEDIT_EXTERNAL)) {
    c = 8 - (tp->t_column & 7);
    notout = 0;
    if (!ISSET(lflag, FNU_TTYEDIT_FLUSHOUT))
      for (; c - notout > 0 && tty_putc(' ', &tp->t_outq) == 0; notout++)
        ;
    else
      notout = c;
    tp->t_column += notout;
    return notout < c ? '\t' : -1;
  }
  if (c == C('D') && ISSET(oflag, FNU_TTYOUT_NOEOT))
    return -1;
  if (c == '\n' && ISSET(oflag, FNU_TTYOUT_NLTOCRNL)) {
    if (!ISSET(lflag, FNU_TTYEDIT_FLUSHOUT) && tty_putc('\r', &tp->t_outq))
      return c;
    tp->t_column = 0;
  } else if (c == '\r' && ISSET(oflag, FNU_TTYOUT_CRTONL))
    c = '\n';
  if (ISSET(oflag, FNU_TTYOUT_TOUPPER) && c >= 'a' && c <= 'z')
    c = c - 'a' + 'A';
  else if (ISSET(oflag, FNU_TTYOUT_TOUPPER) && ISSET(lflag, FNU_TTYEDIT_CASEESC)) {
    switch (c) {
    case '`': c2 = '\''; break;
    case '|': c2 = '!'; break;
    case '~': c2 = '^'; break;
    case '{': c2 = '('; break;
    case '}': c2 = ')'; break;
    default: c2 = c;
    }
    if (c == '\\' || (c >= 'A' && c <= 'Z') || c != c2) {
      if (tty_putc('\\', &tp->t_outq))
        return c;
      c = c2;
    }
  }
  if (ISSET(oflag, FNU_TTYOUT_NOCRCOL0) && c == '\r' && tp->t_column == 0)
    return -1;
  if (!ISSET(lflag, FNU_TTYEDIT_FLUSHOUT) && tty_putc(c, &tp->t_outq))
    return c;
  col = tp->t_column;
  switch (CCLASS(c)) {
  case BACKSPACE:
    if (col > 0)
      --col;
    break;
  case CONTROL:
    break;
  case NEWLINE:
    if (ISSET(oflag, FNU_TTYOUT_NLRETURNS | FNU_TTYOUT_CRTONL))
      col = 0;
    break;
  case RETURN:
    col = 0;
    break;
  case ORDINARY:
    ++col;
    break;
  case TAB:
    col = (col + 8) & ~7;
    break;
  }
  tp->t_column = col;
  return -1;
}

static void ttyecho(int c, struct tty *tp) {
  uint32_t lflag = tp->t_mode.edit;

  if (!ISSET(tp->t_state, TS_CNTTB))
    tp->t_mode.edit &= ~FNU_TTYEDIT_FLUSHOUT;
  if ((!ISSET(lflag, FNU_TTYEDIT_ECHO) &&
       (!ISSET(lflag, FNU_TTYEDIT_ECHONL) || c != '\n')) ||
      ISSET(lflag, FNU_TTYEDIT_EXTERNAL))
    return;
  if ((ISSET(lflag, FNU_TTYEDIT_ECHOCTL) && (c & TTY_CHARMASK) <= 037 && c != '\t' &&
       c != '\n') ||
      (c & TTY_CHARMASK) == 0177) {
    (void)ttyoutput('^', tp);
    c &= TTY_CHARMASK;
    c = c == 0177 ? '?' : c + 'A' - 1;
  }
  (void)ttyoutput(c, tp);
}

static void ttyrubo(struct tty *tp, int cnt) {
  while (cnt-- > 0) {
    (void)ttyoutput('\b', tp);
    (void)ttyoutput(' ', tp);
    (void)ttyoutput('\b', tp);
  }
}

static int ttyretype(struct tty *tp) {
  int i;

  if (tp->t_mode.key[FNU_TTYKEY_REPRINT] != FNU_TTYKEY_OFF)
    ttyecho(tp->t_mode.key[FNU_TTYKEY_REPRINT], tp);
  (void)ttyoutput('\n', tp);
  for (i = 0; i < tp->t_canq.cc; i++)
    ttyecho(nthc(&tp->t_canq, i), tp);
  for (i = 0; i < tp->t_rawq.cc; i++)
    ttyecho(nthc(&tp->t_rawq, i), tp);
  tp->t_state &= ~TS_ERASE;
  tp->t_rocount = tp->t_rawq.cc;
  tp->t_rocol = 0;
  return 1;
}

static int ttyrub(int c, struct tty *tp) {
  int savecol, i;

  if (!ISSET(tp->t_mode.edit, FNU_TTYEDIT_ECHO) ||
      ISSET(tp->t_mode.edit, FNU_TTYEDIT_EXTERNAL))
    return 0;
  tp->t_mode.edit &= ~FNU_TTYEDIT_FLUSHOUT;
  if (ISSET(tp->t_mode.edit, FNU_TTYEDIT_ECHOERASE)) {
    if (tp->t_rocount == 0)
      return ttyretype(tp);
    if (c == ('\t' | TTY_QUOTE) || c == ('\n' | TTY_QUOTE))
      ttyrubo(tp, 2);
    else {
      c &= TTY_CHARMASK;
      switch (CCLASS(c)) {
      case ORDINARY:
        ttyrubo(tp, 1);
        break;
      case BACKSPACE:
      case CONTROL:
      case NEWLINE:
      case RETURN:
      case VTAB:
        if (ISSET(tp->t_mode.edit, FNU_TTYEDIT_ECHOCTL))
          ttyrubo(tp, 2);
        break;
      case TAB:
        if (tp->t_rocount < tp->t_rawq.cc)
          return ttyretype(tp);
        savecol = tp->t_column;
        tp->t_state |= TS_CNTTB;
        tp->t_mode.edit |= FNU_TTYEDIT_FLUSHOUT;
        tp->t_column = tp->t_rocol;
        for (i = 0; i < tp->t_rawq.cc; i++)
          ttyecho(nthc(&tp->t_rawq, i), tp);
        tp->t_mode.edit &= ~FNU_TTYEDIT_FLUSHOUT;
        tp->t_state &= ~TS_CNTTB;
        savecol -= tp->t_column;
        tp->t_column += savecol;
        if (savecol > 8)
          savecol = 8;
        while (--savecol >= 0)
          (void)ttyoutput('\b', tp);
        break;
      }
    }
  } else if (ISSET(tp->t_mode.edit, FNU_TTYEDIT_ECHOPRINT)) {
    if (!ISSET(tp->t_state, TS_ERASE)) {
      tp->t_state |= TS_ERASE;
      (void)ttyoutput('\\', tp);
    }
    ttyecho(c, tp);
  } else
    ttyecho(tp->t_mode.key[FNU_TTYKEY_ERASE], tp);
  --tp->t_rocount;
  return 0;
}

static void tty_signal(struct tty *tp, int sig) {
  if (tp->t_pgrp > 0)
    tty_pgsignal(tp->t_pgrp, tp->t_session, sig);
}

static void ttyprintf(struct tty *tp, const char *s) {
  for (; *s != 0; s++)
    (void)ttyoutput(*s, tp);
}

static void ttyinfo(struct tty *tp) {
  char line[160];
  uint32_t la[3];
  int t;

  sched_loadavg(la);
  t = (int)((la[0] * 100 + FNU_FSCALE / 2) / FNU_FSCALE);
  snprintf(line, sizeof(line), "load: %d.%02d ", t / 100, t % 100);
  ttyprintf(tp, line);
  if (tp->t_session == 0)
    ttyprintf(tp, "not a controlling terminal\n");
  else if (tp->t_pgrp == 0)
    ttyprintf(tp, "no foreground process group\n");
  else {
    pgrp_status(tp->t_pgrp, line, sizeof(line));
    ttyprintf(tp, line);
  }
  tp->t_rocount = 0;
}

static void ttypend(struct tty *tp) {
  int n = tp->t_rawq.cc, c;

  tp->t_mode.edit &= ~FNU_TTYEDIT_PENDING;
  tp->t_state |= TS_TYPEN;
  while (n-- > 0 && (c = tty_getc(&tp->t_rawq)) >= 0)
    ttyinput(c, tp);
  tp->t_state &= ~TS_TYPEN;
}

#define TTBREAKC(c, lflag)                                                     \
  ((c) == '\n' || ((CCEQ(cc[FNU_TTYKEY_EOF], c) || CCEQ(cc[FNU_TTYKEY_EOL], c) ||  \
                    (CCEQ(cc[FNU_TTYKEY_EOL2], c) && ISSET(lflag, FNU_TTYEDIT_EXTENDED)))))

// tty_lock held
int ttyinput(int c, struct tty *tp) {
  uint32_t iflag = tp->t_mode.in, lflag = tp->t_mode.edit;
  uchar *cc = tp->t_mode.key;
  int i, error, ret = 0, alt, ctype;

  if (!ISSET(tp->t_mode.line, FNU_TTYLINE_RECEIVE))
    return 0;
  tp->t_inputat = uptime_ns();
  if (ISSET(lflag, FNU_TTYEDIT_PENDING))
    ttypend(tp);

  if ((error = c & TTY_ERRORMASK) != 0) {
    c &= ~TTY_ERRORMASK;
    if (ISSET(error, TTY_FE) && c == 0) {
      if (ISSET(iflag, FNU_TTYIN_IGNBREAK))
        return 0;
      ttyflush(tp, FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT);
      if (ISSET(iflag, FNU_TTYIN_BREAKINT)) {
        tty_signal(tp, FNU_SIG_INTERRUPT);
        goto endcase;
      } else if (ISSET(iflag, FNU_TTYIN_MARKPARITY))
        goto parmrk;
    } else if ((ISSET(error, TTY_PE) && ISSET(iflag, FNU_TTYIN_CHECKPARITY)) ||
               ISSET(error, TTY_FE)) {
      if (ISSET(iflag, FNU_TTYIN_IGNPARITY))
        goto endcase;
      else if (ISSET(iflag, FNU_TTYIN_MARKPARITY)) {
parmrk:
        (void)tty_putc(0377 | TTY_QUOTE, &tp->t_rawq);
        if (ISSET(iflag, FNU_TTYIN_STRIP) || c != 0377)
          (void)tty_putc(0 | TTY_QUOTE, &tp->t_rawq);
        (void)tty_putc(c | TTY_QUOTE, &tp->t_rawq);
        goto endcase;
      } else
        c = 0;
    }
  }
  if (c == 0377 && !ISSET(iflag, FNU_TTYIN_STRIP) && ISSET(iflag, FNU_TTYIN_MARKPARITY))
    goto parmrk;

  if (ISSET(iflag, FNU_TTYIN_FLOWIN))
    ttyblock(tp);
  if (!ISSET(tp->t_state, TS_TYPEN) && ISSET(iflag, FNU_TTYIN_STRIP))
    c &= ~0x80;
  if (!ISSET(lflag, FNU_TTYEDIT_EXTERNAL)) {
    if (ISSET(tp->t_state, TS_LNCH)) {
      c |= TTY_QUOTE;
      tp->t_state &= ~TS_LNCH;
    }
    if (ISSET(lflag, FNU_TTYEDIT_EXTENDED)) {
      if (CCEQ(cc[FNU_TTYKEY_LITERAL], c)) {
        if (ISSET(lflag, FNU_TTYEDIT_ECHO)) {
          if (ISSET(lflag, FNU_TTYEDIT_ECHOERASE)) {
            (void)ttyoutput('^', tp);
            (void)ttyoutput('\b', tp);
          } else
            ttyecho(c, tp);
        }
        tp->t_state |= TS_LNCH;
        goto endcase;
      }
      if (CCEQ(cc[FNU_TTYKEY_DISCARD], c)) {
        if (ISSET(lflag, FNU_TTYEDIT_FLUSHOUT))
          tp->t_mode.edit &= ~FNU_TTYEDIT_FLUSHOUT;
        else {
          ttyflush(tp, FNU_FLUSH_OUTPUT);
          ttyecho(c, tp);
          if (tp->t_rawq.cc + tp->t_canq.cc)
            ret = ttyretype(tp);
          tp->t_mode.edit |= FNU_TTYEDIT_FLUSHOUT;
        }
        goto startoutput;
      }
    }
    if (ISSET(lflag, FNU_TTYEDIT_SIGNALS)) {
      if (CCEQ(cc[FNU_TTYKEY_INTERRUPT], c) || CCEQ(cc[FNU_TTYKEY_QUIT], c)) {
        if (!ISSET(lflag, FNU_TTYEDIT_NOFLUSH))
          ttyflush(tp, FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT);
        ttyecho(c, tp);
        tty_signal(tp, CCEQ(cc[FNU_TTYKEY_INTERRUPT], c) ? FNU_SIG_INTERRUPT
                                                          : FNU_SIG_QUIT);
        goto endcase;
      }
      if (CCEQ(cc[FNU_TTYKEY_SUSPEND], c)) {
        if (!ISSET(lflag, FNU_TTYEDIT_NOFLUSH))
          ttyflush(tp, FNU_FLUSH_INPUT);
        ttyecho(c, tp);
        tty_signal(tp, FNU_SIG_TTYSTOP);
        goto endcase;
      }
    }
    if (ISSET(iflag, FNU_TTYIN_FLOWOUT)) {
      if (CCEQ(cc[FNU_TTYKEY_STOP], c)) {
        if (!ISSET(tp->t_state, TS_TTSTOP)) {
          tp->t_state |= TS_TTSTOP;
          ttstop(tp, 0);
          return 0;
        }
        if (!CCEQ(cc[FNU_TTYKEY_START], c))
          return 0;
        goto endcase;
      }
      if (CCEQ(cc[FNU_TTYKEY_START], c))
        goto restartoutput;
    }
    if (c == '\r') {
      if (ISSET(iflag, FNU_TTYIN_DROPCR))
        goto endcase;
      else if (ISSET(iflag, FNU_TTYIN_CRTONL))
        c = '\n';
    } else if (c == '\n' && ISSET(iflag, FNU_TTYIN_NLTOCR))
      c = '\r';
  }
  if (!ISSET(tp->t_mode.edit, FNU_TTYEDIT_EXTERNAL) && ISSET(lflag, FNU_TTYEDIT_LINES)) {
    if (ISSET(lflag, FNU_TTYEDIT_CASEESC) && ISSET(iflag, FNU_TTYIN_TOLOWER)) {
      if (ISSET(tp->t_state, TS_BKSL)) {
        tp->t_state &= ~TS_BKSL;
        switch (c) {
        case '\'': c = '`'; break;
        case '!': c = '|'; break;
        case '^': c = '~'; break;
        case '(': c = '{'; break;
        case ')': c = '}'; break;
        }
      } else if (c == '\\') {
        tp->t_state |= TS_BKSL;
        goto endcase;
      } else if (c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';
    } else if (ISSET(iflag, FNU_TTYIN_TOLOWER) && c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
    if (CCEQ(cc[FNU_TTYKEY_ERASE], c) || CCEQ(cc[FNU_TTYKEY_ERASE2], c)) {
      if (tp->t_rawq.cc)
        ret = ttyrub(unputc(&tp->t_rawq), tp);
      goto endcase;
    }
    if (CCEQ(cc[FNU_TTYKEY_KILL], c)) {
      if (ISSET(lflag, FNU_TTYEDIT_ECHOKILLRUB) && tp->t_rawq.cc == tp->t_rocount &&
          !ISSET(lflag, FNU_TTYEDIT_ECHOPRINT)) {
        while (tp->t_rawq.cc)
          if (ttyrub(unputc(&tp->t_rawq), tp))
            ret = 1;
      } else {
        ttyecho(c, tp);
        if (ISSET(lflag, FNU_TTYEDIT_ECHOKILL | FNU_TTYEDIT_ECHOKILLRUB))
          ttyecho('\n', tp);
        flushq(&tp->t_rawq);
        tp->t_rocount = 0;
      }
      goto endcase;
    }
    if (CCEQ(cc[FNU_TTYKEY_WORDERASE], c) && ISSET(lflag, FNU_TTYEDIT_EXTENDED)) {
      alt = ISSET(lflag, FNU_TTYEDIT_ALTWERASE);
      while ((c = unputc(&tp->t_rawq)) == ' ' || c == '\t')
        if (ttyrub(c, tp))
          ret = 1;
      if (c == -1)
        goto endcase;
      if (ttyrub(c, tp))
        ret = 1;
      if ((c = unputc(&tp->t_rawq)) == -1)
        goto endcase;
      if (c == ' ' || c == '\t') {
        (void)tty_putc(c, &tp->t_rawq);
        goto endcase;
      }
      ctype = ISALPHA(c);
      do {
        if (ttyrub(c, tp))
          ret = 1;
        if ((c = unputc(&tp->t_rawq)) == -1)
          goto endcase;
      } while (c != ' ' && c != '\t' && (alt == 0 || ISALPHA(c) == ctype));
      (void)tty_putc(c, &tp->t_rawq);
      goto endcase;
    }
    if (CCEQ(cc[FNU_TTYKEY_REPRINT], c) && ISSET(lflag, FNU_TTYEDIT_EXTENDED)) {
      ret = ttyretype(tp);
      goto endcase;
    }
    if (CCEQ(cc[FNU_TTYKEY_STATUS], c) && ISSET(lflag, FNU_TTYEDIT_EXTENDED)) {
      if (ISSET(lflag, FNU_TTYEDIT_SIGNALS))
        tty_signal(tp, FNU_SIG_INFO);
      if (!ISSET(lflag, FNU_TTYEDIT_NOSTATUS))
        ttyinfo(tp);
      goto endcase;
    }
  }
  if (tp->t_rawq.cc + tp->t_canq.cc >= TTYHOG) {
    if (ISSET(iflag, FNU_TTYIN_BELLFULL)) {
      if (tp->t_outq.cc < tp->t_hiwat)
        (void)ttyoutput(C('G'), tp);
    } else
      ttyflush(tp, FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT);
    goto endcase;
  }
  if (tty_putc(c, &tp->t_rawq) >= 0) {
    if (!ISSET(lflag, FNU_TTYEDIT_LINES)) {
      ttwakeup(tp);
      ttyecho(c, tp);
      goto endcase;
    }
    if (TTBREAKC(c, lflag)) {
      tp->t_rocount = 0;
      catq(&tp->t_rawq, &tp->t_canq);
      ttwakeup(tp);
    } else if (tp->t_rocount++ == 0)
      tp->t_rocol = tp->t_column;
    if (ISSET(tp->t_state, TS_ERASE)) {
      tp->t_state &= ~TS_ERASE;
      (void)ttyoutput('/', tp);
    }
    i = tp->t_column;
    ttyecho(c, tp);
    if (CCEQ(cc[FNU_TTYKEY_EOF], c) && ISSET(lflag, FNU_TTYEDIT_ECHO)) {
      i = tp->t_column - i < 2 ? tp->t_column - i : 2;
      while (i-- > 0)
        (void)ttyoutput('\b', tp);
    }
  }
endcase:
  if (ISSET(tp->t_state, TS_TTSTOP) && !ISSET(iflag, FNU_TTYIN_ANYSTART) &&
      cc[FNU_TTYKEY_START] != cc[FNU_TTYKEY_STOP])
    return ret;
restartoutput:
  tp->t_mode.edit &= ~FNU_TTYEDIT_FLUSHOUT;
  tp->t_state &= ~TS_TTSTOP;
startoutput:
  ttstart(tp);
  return ret;
}

int ttnread(struct tty *tp) {
  int nread;

  if (ISSET(tp->t_mode.edit, FNU_TTYEDIT_PENDING))
    ttypend(tp);
  nread = tp->t_canq.cc;
  if (!ISSET(tp->t_mode.edit, FNU_TTYEDIT_LINES)) {
    nread += tp->t_rawq.cc;
    if (nread < tp->t_mode.min && !tp->t_mode.time)
      nread = 0;
  }
  return nread;
}

int tty_readable(struct tty *tp) {
  int r;

  mtx_enter(&tty_lock);
  r = ttnread(tp) > 0 || ISSET(tp->t_state, TS_ZOMBIE) ||
      (!ISSET(tp->t_state, TS_CARR_ON) && !ISSET(tp->t_mode.line, FNU_TTYLINE_LOCAL));
  if (!r)
    selrecord(&tp->t_rsel);
  mtx_leave(&tty_lock);
  return r;
}

int tty_writable(struct tty *tp) {
  int r;

  mtx_enter(&tty_lock);
  r = tp->t_outq.cc <= tp->t_lowat || ISSET(tp->t_state, TS_ZOMBIE);
  if (!r)
    selrecord(&tp->t_wsel);
  mtx_leave(&tty_lock);
  return r;
}

static int isbackground(struct process *pr, struct tty *tp) {
  return tp->t_session != 0 && pr->sid == tp->t_session && pr->pgid != tp->t_pgrp;
}

static int background_stop(struct tty *tp __unused, int sig, int eio_if_masked) {
  struct proc *p = curproc;
  int masked = proc_sig_masked(p, sig), error;

  if (masked && !eio_if_masked)
    return -1;
  if (masked || proc_pgrp_orphaned(p->p_p))
    return FNU_ERR_IO;
  mtx_leave(&tty_lock);
  tty_pgsignal(p->p_p->pgid, p->p_p->sid, sig);
  error = tsleep(&nowake, TTIPRI | PCATCH, "ttybg", HZ);
  mtx_enter(&tty_lock);
  return error == FNU_ERR_WOULDBLOCK ? 0 : error;
}

int ttread(struct tty *tp, struct uio *uio, int nonblock) {
  struct proc *p = curproc;
  uchar buf[OBUFSIZ];
  uchar *cc = tp->t_mode.key;
  uint64_t due = 0;
  int n, c, first = 1, error = 0, vmin, vtime, last = 0, gen = tp->t_gen;
  int stop, again = 0;
  struct ttyq *qp;
  uint32_t lflag;

  mtx_enter(&tty_lock);
  for (;;) {
    if (tp->t_gen != gen) {
      error = FNU_ERR_IO;
      break;
    }
    lflag = tp->t_mode.edit;
    if (ISSET(lflag, FNU_TTYEDIT_PENDING))
      ttypend(tp);
    if (p != 0 && isbackground(p->p_p, tp)) {
      if ((error = background_stop(tp, FNU_SIG_TTYIN, 1)) != 0)
        break;
      continue;
    }
    if (!ISSET(lflag, FNU_TTYEDIT_LINES)) {
      vmin = tp->t_mode.min;
      vtime = tp->t_mode.time;
      qp = &tp->t_rawq;
      if (vtime == 0) {
        if (qp->cc < vmin)
          goto sleep;
        goto read;
      }
      // VTIME
      if (vmin > 0) {
        if (qp->cc <= 0)
          goto sleep;
        if (qp->cc >= vmin)
          goto read;
        if (due == 0 || qp->cc > last)
          due = ticks + (uint64_t)vtime * HZ / 10;
      } else {
        if (qp->cc > 0)
          goto read;
        if (due == 0)
          due = ticks + (uint64_t)vtime * HZ / 10;
      }
      last = qp->cc;
      if (ticks >= due)
        goto read;
      goto sleep;
    }
    qp = &tp->t_canq;
    if (qp->cc > 0)
      goto read;
sleep:
    if (ISSET(tp->t_state, TS_ZOMBIE) ||
        (!ISSET(tp->t_state, TS_CARR_ON) && !ISSET(tp->t_mode.line, FNU_TTYLINE_LOCAL) &&
         ISSET(tp->t_state, TS_ISOPEN)))
      break;
    if (nonblock) {
      error = FNU_ERR_WOULDBLOCK;
      break;
    }
    error = msleep(&tp->t_rawq, &tty_lock, TTIPRI | PCATCH, "ttyin",
                   due != 0 ? (due > ticks ? (int)(due - ticks) : 1) : 0);
    if (error == FNU_ERR_WOULDBLOCK) {
      error = 0;
      if (!ISSET(tp->t_mode.edit, FNU_TTYEDIT_LINES) && tp->t_rawq.cc == 0)
        break;
      if (!ISSET(tp->t_mode.edit, FNU_TTYEDIT_LINES))
        goto read;
    }
    if (error != 0)
      break;
    continue;
read:
    stop = 0;
    for (n = 0; n < (int)sizeof(buf) && (size_t)n < uio->uio_resid;) {
      if ((c = tty_getc(qp)) < 0) {
        stop = 1;
        break;
      }
      if (CCEQ(cc[FNU_TTYKEY_DELAYSUSP], c) &&
          ISSET(lflag, FNU_TTYEDIT_EXTENDED | FNU_TTYEDIT_SIGNALS) ==
              (FNU_TTYEDIT_EXTENDED | FNU_TTYEDIT_SIGNALS)) {
        tty_signal(tp, FNU_SIG_TTYSTOP);
        stop = 1;
        if (first && n == 0) {
          error = msleep(&nowake, &tty_lock, TTIPRI | PCATCH, "ttybg", HZ);
          stop = error != FNU_ERR_WOULDBLOCK && error != 0;
          if (!stop)
            error = 0;
          again = !stop;
        }
        break;
      }
      if (CCEQ(cc[FNU_TTYKEY_EOF], c) && ISSET(lflag, FNU_TTYEDIT_LINES)) {
        stop = 1;
        break;
      }
      buf[n++] = (uchar)c;
      if (ISSET(lflag, FNU_TTYEDIT_LINES) && TTBREAKC(c, lflag)) {
        stop = 1;
        break;
      }
    }
    if (tp->t_rawq.cc < TTYHOG / 5)
      ttyunblock(tp);
    wakeup(&tp->t_canq);
    mtx_leave(&tty_lock);
    if (n > 0 && uiomove(buf, (size_t)n, uio) != 0)
      error = FNU_ERR_FAULT;
    mtx_enter(&tty_lock);
    if (again) {
      again = 0;
      continue;
    }
    if (error != 0 || stop || uio->uio_resid == 0)
      break;
    first = 0;
  }
  mtx_leave(&tty_lock);
  return error;
}

int ttwrite(struct tty *tp, struct uio *uio, int nonblock) {
  struct proc *p = curproc;
  uchar obuf[OBUFSIZ];
  size_t start = uio->uio_resid;
  int cc = 0, i = 0, error = 0, r, gen = tp->t_gen;

  mtx_enter(&tty_lock);
  for (;;) {
    if (tp->t_gen != gen || ISSET(tp->t_state, TS_ZOMBIE) ||
        (!ISSET(tp->t_state, TS_CARR_ON) && !ISSET(tp->t_mode.line, FNU_TTYLINE_LOCAL))) {
      error = FNU_ERR_IO;
      break;
    }
    if (p != 0 && isbackground(p->p_p, tp) &&
        ISSET(tp->t_mode.edit, FNU_TTYEDIT_STOPBG)) {
      if ((r = background_stop(tp, FNU_SIG_TTYOUT, 0)) > 0) {
        error = r;
        break;
      }
      if (r == 0)
        continue;
    }
    if (ISSET(tp->t_mode.edit, FNU_TTYEDIT_FLUSHOUT)) {
      uio->uio_resid = 0;
      cc = i = 0;
      break;
    }
    if (cc == i) {
      if (uio->uio_resid == 0)
        break;
      cc = uio->uio_resid < sizeof(obuf) ? (int)uio->uio_resid : (int)sizeof(obuf);
      i = 0;
      mtx_leave(&tty_lock);
      error = uiomove(obuf, (size_t)cc, uio) != 0 ? FNU_ERR_FAULT : 0;
      mtx_enter(&tty_lock);
      if (error != 0) {
        cc = i = 0;
        break;
      }
      if (tp == cn_tp && tp->t_gen == 0)
        consbuf_put((const char *)obuf, cc);
    }
    for (; i < cc && tp->t_outq.cc <= tp->t_hiwat; i++) {
      tp->t_rocount = 0;
      if (ttyoutput(obuf[i], tp) >= 0)
        break;
      if (ISSET(tp->t_mode.edit, FNU_TTYEDIT_FLUSHOUT))
        break;
    }
    ttstart(tp);
    if (i < cc && tp->t_outq.cc > tp->t_hiwat) {
      if (nonblock) {
        error = uio->uio_resid + (size_t)(cc - i) == start ? FNU_ERR_WOULDBLOCK : 0;
        break;
      }
      tp->t_state |= TS_ASLEEP;
      if ((error = msleep(&tp->t_outq, &tty_lock, TTOPRI | PCATCH, "ttyout", 0)) != 0)
        break;
    }
  }
  uio->uio_resid += (size_t)(cc - i);
  mtx_leave(&tty_lock);
  return error;
}

static int ttywait_for(struct tty *tp, int limit) {
  int error = 0;

  while ((tp->t_outq.cc || ISSET(tp->t_state, TS_BUSY)) &&
         (ISSET(tp->t_state, TS_CARR_ON) || ISSET(tp->t_mode.line, FNU_TTYLINE_LOCAL)) &&
         tp->t_oproc != 0 && !ISSET(tp->t_state, TS_ZOMBIE)) {
    ttstart(tp);
    if (!(tp->t_outq.cc || ISSET(tp->t_state, TS_BUSY)))
      break;
    tp->t_state |= TS_ASLEEP;
    if ((error = msleep(&tp->t_outq, &tty_lock, TTOPRI | PCATCH, "ttyout", limit)) != 0)
      break;
  }
  return error;
}

static int ttywait(struct tty *tp) { return ttywait_for(tp, 0); }

// tty_lock held
void ttylclose(struct tty *tp) {
  int error = ttywait_for(tp, 5 * HZ);

  if (error == 0 || error == FNU_ERR_WOULDBLOCK)
    ttyflush(tp, FNU_FLUSH_INPUT);
}

int ttymodem(struct tty *tp, int carrier) {
  int r = 1;

  mtx_enter(&tty_lock);
  if (ISSET(tp->t_mode.line, FNU_TTYLINE_CARRIERFLOW)) {
    if (carrier) {
      tp->t_state &= ~TS_TTSTOP;
      ttstart(tp);
    } else if (!ISSET(tp->t_state, TS_TTSTOP)) {
      tp->t_state |= TS_TTSTOP;
      ttstop(tp, 0);
    }
  } else if (!carrier) {
    tp->t_state &= ~TS_CARR_ON;
    if (ISSET(tp->t_state, TS_ISOPEN) && !ISSET(tp->t_mode.line, FNU_TTYLINE_LOCAL)) {
      if (tp->t_session != 0)
        kill(tp->t_session, FNU_SIG_HANGUP);
      ttyflush(tp, FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT);
      r = 0;
    }
  } else {
    tp->t_state |= TS_CARR_ON;
    ttwakeup(tp);
  }
  mtx_leave(&tty_lock);
  return r;
}

pid_t tty_leave_session(struct tty *tp, pid_t sid) {
  pid_t fg = 0;

  mtx_enter(&tty_lock);
  if (tp->t_session == sid) {
    fg = tp->t_pgrp;
    tp->t_session = 0;
    tp->t_pgrp = 0;
  }
  mtx_leave(&tty_lock);
  return fg;
}

static int modifying(uint64_t cmd) {
  switch (cmd) {
  case FNU_TTYFLUSH:
  case FNU_TTYDRAIN:
  case FNU_TTYBREAK:
  case FNU_TTYENDBREAK:
  case FNU_TTYSETMODE:
  case FNU_TTYSETMODE_DRAIN:
  case FNU_TTYSETMODE_FLUSH:
  case FNU_TTYSETGROUP:
  case FNU_TTYSTATUS:
  case FNU_TTYSETSIZE:
    return 1;
  }
  return 0;
}

int ttioctl(struct tty *tp, uint64_t cmd, uint64_t arg) {
  struct proc *p = curproc;
  struct process *pr = p->p_p;
  struct fnu_ttymode t;
  struct fnu_ttysize ws;
  pid_t pid;
  int v, error = 0, r;

  mtx_enter(&tty_lock);
  while (modifying(cmd) && isbackground(pr, tp)) {
    if ((r = background_stop(tp, FNU_SIG_TTYOUT, 0)) < 0)
      break;
    if (r > 0) {
      mtx_leave(&tty_lock);
      return r;
    }
  }
  switch (cmd) {
  case FNU_TTYGETMODE:
    t = tp->t_mode;
    mtx_leave(&tty_lock);
    return copyout(pr->pgdir, (uintptr_t)arg, &t, sizeof(t)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_TTYSETMODE:
  case FNU_TTYSETMODE_DRAIN:
  case FNU_TTYSETMODE_FLUSH:
    mtx_leave(&tty_lock);
    if (copyin(pr->pgdir, &t, (uintptr_t)arg, sizeof(t)) < 0)
      return FNU_ERR_FAULT;
    mtx_enter(&tty_lock);
    if (cmd != FNU_TTYSETMODE) {
      if ((error = ttywait(tp)) != 0)
        break;
      if (cmd == FNU_TTYSETMODE_FLUSH)
        ttyflush(tp, FNU_FLUSH_INPUT);
    }
    if (!ISSET(t.line, FNU_TTYLINE_IGNORE)) {
      if (tp->t_param != 0 && (error = tp->t_param(tp, &t)) != 0)
        break;
      if (!ISSET(tp->t_state, TS_CARR_ON) && ISSET(tp->t_mode.line, FNU_TTYLINE_LOCAL) &&
          !ISSET(t.line, FNU_TTYLINE_LOCAL))
        ttwakeup(tp);
      tp->t_mode.line = t.line;
      tp->t_mode.speed_in = t.speed_in;
      tp->t_mode.speed_out = t.speed_out;
      if (t.speed_out == 0 && tp->t_session != 0)
        kill(tp->t_session, FNU_SIG_HANGUP);
      ttsetwater(tp);
    }
    if (cmd != FNU_TTYSETMODE_FLUSH &&
        ISSET(t.edit, FNU_TTYEDIT_LINES) != ISSET(tp->t_mode.edit, FNU_TTYEDIT_LINES)) {
      if (ISSET(t.edit, FNU_TTYEDIT_LINES)) {
        tp->t_mode.edit |= FNU_TTYEDIT_PENDING;
        ttwakeup(tp);
      } else {
        catq(&tp->t_rawq, &tp->t_canq);
        catq(&tp->t_canq, &tp->t_rawq);
        tp->t_mode.edit &= ~FNU_TTYEDIT_PENDING;
      }
    }
    tp->t_mode.in = t.in;
    tp->t_mode.out = t.out;
    // EXTERNAL
    t.edit &= ~(FNU_TTYEDIT_EXTERNAL | FNU_TTYEDIT_PENDING);
    tp->t_mode.edit = t.edit | (tp->t_mode.edit & (FNU_TTYEDIT_EXTERNAL | FNU_TTYEDIT_PENDING));
    memmove(tp->t_mode.key, t.key, sizeof(t.key));
    tp->t_mode.min = t.min;
    tp->t_mode.time = t.time;
    if (!ISSET(t.in, FNU_TTYIN_FLOWOUT) && ISSET(tp->t_state, TS_TTSTOP)) {
      tp->t_state &= ~TS_TTSTOP;
      ttstart(tp);
    }
    break;
  case FNU_TTYFLUSH:
    mtx_leave(&tty_lock);
    if (copyin(pr->pgdir, &v, (uintptr_t)arg, sizeof(v)) < 0)
      return FNU_ERR_FAULT;
    if (v == 0)
      v = FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT;
    if (v & ~(FNU_FLUSH_INPUT | FNU_FLUSH_OUTPUT))
      return FNU_ERR_INVALID;
    mtx_enter(&tty_lock);
    ttyflush(tp, v);
    break;
  case FNU_TTYDRAIN:
    error = ttywait(tp);
    break;
  case FNU_IOWAITING:
    v = ttnread(tp);
    mtx_leave(&tty_lock);
    return copyout(pr->pgdir, (uintptr_t)arg, &v, sizeof(v)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_TTYOUTQ:
    v = tp->t_outq.cc;
    mtx_leave(&tty_lock);
    return copyout(pr->pgdir, (uintptr_t)arg, &v, sizeof(v)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_TTYGETSIZE:
    ws = tp->t_winsize;
    mtx_leave(&tty_lock);
    return copyout(pr->pgdir, (uintptr_t)arg, &ws, sizeof(ws)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_TTYSETSIZE:
    mtx_leave(&tty_lock);
    if (copyin(pr->pgdir, &ws, (uintptr_t)arg, sizeof(ws)) < 0)
      return FNU_ERR_FAULT;
    mtx_enter(&tty_lock);
    if (memcmp(&ws, &tp->t_winsize, sizeof(ws)) != 0) {
      tp->t_winsize = ws;
      tty_signal(tp, FNU_SIG_WINDOW);
    }
    break;
  case FNU_TTYGETGROUP:
    if (tp->t_session != pr->sid && pr->uid != 0) {
      error = FNU_ERR_BADREQUEST;
      break;
    }
    pid = tp->t_pgrp;
    mtx_leave(&tty_lock);
    return copyout(pr->pgdir, (uintptr_t)arg, &pid, sizeof(pid)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_TTYGETSESSION:
    if (tp->t_session == 0 || tp->t_session != pr->sid) {
      error = FNU_ERR_BADREQUEST;
      break;
    }
    pid = tp->t_session;
    mtx_leave(&tty_lock);
    return copyout(pr->pgdir, (uintptr_t)arg, &pid, sizeof(pid)) < 0 ? FNU_ERR_FAULT : 0;
  case FNU_TTYSETGROUP:
    mtx_leave(&tty_lock);
    if (copyin(pr->pgdir, &pid, (uintptr_t)arg, sizeof(pid)) < 0)
      return FNU_ERR_FAULT;
    if (pid <= 0 || pgrp_session(pid) == 0)
      return FNU_ERR_INVALID;
    if (pgrp_session(pid) != pr->sid)
      return FNU_ERR_NOTPERMITTED;
    mtx_enter(&tty_lock);
    if (tp->t_session != pr->sid)
      error = FNU_ERR_BADREQUEST;
    else
      tp->t_pgrp = pid;
    break;
  case FNU_TTYCONTROL:
    if (pr->sid != pr->pid || (tp->t_session != 0 && tp->t_session != pr->sid) ||
        session_take_tty(pr, tp->t_unit) != 0) {
      error = FNU_ERR_NOTPERMITTED;
      break;
    }
    tp->t_session = pr->sid;
    tp->t_pgrp = pr->pgid;
    break;
  case FNU_TTYSTART:
    if (ISSET(tp->t_state, TS_TTSTOP) || ISSET(tp->t_mode.edit, FNU_TTYEDIT_FLUSHOUT)) {
      tp->t_mode.edit &= ~FNU_TTYEDIT_FLUSHOUT;
      tp->t_state &= ~TS_TTSTOP;
      ttstart(tp);
    }
    break;
  case FNU_TTYSTOP:
    if (!ISSET(tp->t_state, TS_TTSTOP)) {
      tp->t_state |= TS_TTSTOP;
      ttstop(tp, 0);
    }
    break;
  case FNU_TTYSTATUS:
    ttyinfo(tp);
    ttstart(tp);
    break;
  default:
    error = TTY_PASS;
  }
  mtx_leave(&tty_lock);
  return error;
}
