#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>

#define LINELENGTH 72

enum { EDIT, IN, OUT, LINE };
enum { CHANGED, POSIX, BSD };

#define SHOWN 1
#define EXTERNAL 2 // set through FNU_TTYEXTERNAL

struct mode {
  const char *name;
  int field;
  uint32_t set, clear;
  int how;
};

#define BIT(n, f, b) {n, f, b, 0, SHOWN}, {"-" n, f, 0, b, 0}
#define ALIAS(n, f, b) {n, f, b, 0, 0}, {"-" n, f, 0, b, 0}

static const struct mode modes[] = {
    BIT("icanon", EDIT, FNU_TTYEDIT_LINES),
    BIT("isig", EDIT, FNU_TTYEDIT_SIGNALS),
    BIT("iexten", EDIT, FNU_TTYEDIT_EXTENDED),
    BIT("echo", EDIT, FNU_TTYEDIT_ECHO),
    BIT("echoe", EDIT, FNU_TTYEDIT_ECHOERASE),
    BIT("echok", EDIT, FNU_TTYEDIT_ECHOKILL),
    BIT("echoke", EDIT, FNU_TTYEDIT_ECHOKILLRUB),
    BIT("echonl", EDIT, FNU_TTYEDIT_ECHONL),
    BIT("echoctl", EDIT, FNU_TTYEDIT_ECHOCTL),
    BIT("echoprt", EDIT, FNU_TTYEDIT_ECHOPRINT),
    BIT("altwerase", EDIT, FNU_TTYEDIT_ALTWERASE),
    BIT("noflsh", EDIT, FNU_TTYEDIT_NOFLUSH),
    BIT("tostop", EDIT, FNU_TTYEDIT_STOPBG),
    BIT("flusho", EDIT, FNU_TTYEDIT_FLUSHOUT),
    BIT("pendin", EDIT, FNU_TTYEDIT_PENDING),
    BIT("nokerninfo", EDIT, FNU_TTYEDIT_NOSTATUS),
    {"extproc", EDIT, FNU_TTYEDIT_EXTERNAL, 0, SHOWN | EXTERNAL},
    {"-extproc", EDIT, 0, FNU_TTYEDIT_EXTERNAL, EXTERNAL},
    BIT("xcase", EDIT, FNU_TTYEDIT_CASEESC),
    ALIAS("crterase", EDIT, FNU_TTYEDIT_ECHOERASE),
    ALIAS("crtbs", EDIT, FNU_TTYEDIT_ECHOERASE),
    ALIAS("crtkill", EDIT, FNU_TTYEDIT_ECHOKILLRUB),
    ALIAS("ctlecho", EDIT, FNU_TTYEDIT_ECHOCTL),
    ALIAS("prterase", EDIT, FNU_TTYEDIT_ECHOPRINT),
    {"kerninfo", EDIT, 0, FNU_TTYEDIT_NOSTATUS, 0},
    {"-kerninfo", EDIT, FNU_TTYEDIT_NOSTATUS, 0, 0},
    {"crt", EDIT, FNU_TTYEDIT_ECHOERASE | FNU_TTYEDIT_ECHOKILLRUB | FNU_TTYEDIT_ECHOCTL,
     FNU_TTYEDIT_ECHOKILL | FNU_TTYEDIT_ECHOPRINT, 0},
    {"-crt", EDIT, FNU_TTYEDIT_ECHOKILL,
     FNU_TTYEDIT_ECHOERASE | FNU_TTYEDIT_ECHOKILLRUB | FNU_TTYEDIT_ECHOCTL, 0},
    {"newcrt", EDIT, FNU_TTYEDIT_ECHOERASE | FNU_TTYEDIT_ECHOKILLRUB | FNU_TTYEDIT_ECHOCTL,
     FNU_TTYEDIT_ECHOKILL | FNU_TTYEDIT_ECHOPRINT, 0},
    {"-newcrt", EDIT, FNU_TTYEDIT_ECHOKILL,
     FNU_TTYEDIT_ECHOERASE | FNU_TTYEDIT_ECHOKILLRUB | FNU_TTYEDIT_ECHOCTL, 0},

    BIT("istrip", IN, FNU_TTYIN_STRIP),
    BIT("icrnl", IN, FNU_TTYIN_CRTONL),
    BIT("inlcr", IN, FNU_TTYIN_NLTOCR),
    BIT("igncr", IN, FNU_TTYIN_DROPCR),
    BIT("iuclc", IN, FNU_TTYIN_TOLOWER),
    BIT("ixon", IN, FNU_TTYIN_FLOWOUT),
    BIT("ixoff", IN, FNU_TTYIN_FLOWIN),
    BIT("ixany", IN, FNU_TTYIN_ANYSTART),
    BIT("imaxbel", IN, FNU_TTYIN_BELLFULL),
    BIT("ignbrk", IN, FNU_TTYIN_IGNBREAK),
    BIT("brkint", IN, FNU_TTYIN_BREAKINT),
    BIT("inpck", IN, FNU_TTYIN_CHECKPARITY),
    BIT("ignpar", IN, FNU_TTYIN_IGNPARITY),
    BIT("parmrk", IN, FNU_TTYIN_MARKPARITY),
    ALIAS("flow", IN, FNU_TTYIN_FLOWOUT),
    ALIAS("tandem", IN, FNU_TTYIN_FLOWIN),
    {"decctlq", IN, 0, FNU_TTYIN_ANYSTART, 0},
    {"-decctlq", IN, FNU_TTYIN_ANYSTART, 0, 0},

    BIT("opost", OUT, FNU_TTYOUT_PROCESS),
    BIT("onlcr", OUT, FNU_TTYOUT_NLTOCRNL),
    BIT("ocrnl", OUT, FNU_TTYOUT_CRTONL),
    BIT("onocr", OUT, FNU_TTYOUT_NOCRCOL0),
    BIT("onlret", OUT, FNU_TTYOUT_NLRETURNS),
    BIT("olcuc", OUT, FNU_TTYOUT_TOUPPER),
    BIT("oxtabs", OUT, FNU_TTYOUT_TABS),
    BIT("onoeot", OUT, FNU_TTYOUT_NOEOT),
    {"litout", OUT, 0, FNU_TTYOUT_PROCESS, 0},
    {"-litout", OUT, FNU_TTYOUT_PROCESS, 0, 0},
    {"tabs", OUT, 0, FNU_TTYOUT_TABS, 0},
    {"-tabs", OUT, FNU_TTYOUT_TABS, 0, 0},

    BIT("cread", LINE, FNU_TTYLINE_RECEIVE),
    BIT("parenb", LINE, FNU_TTYLINE_PARITY),
    BIT("parodd", LINE, FNU_TTYLINE_ODD),
    BIT("hupcl", LINE, FNU_TTYLINE_HANGUP),
    BIT("clocal", LINE, FNU_TTYLINE_LOCAL),
    BIT("cstopb", LINE, FNU_TTYLINE_TWOSTOP),
    BIT("crtscts", LINE, FNU_TTYLINE_HWFLOW),
    BIT("dsrflow", LINE, FNU_TTYLINE_DSRFLOW),
    BIT("mdmbuf", LINE, FNU_TTYLINE_CARRIERFLOW),
    ALIAS("hup", LINE, FNU_TTYLINE_HANGUP),
    ALIAS("ctsflow", LINE, FNU_TTYLINE_CTSFLOW),
    ALIAS("rtsflow", LINE, FNU_TTYLINE_RTSFLOW),
    {"cs5", LINE, FNU_TTYLINE_5BITS, FNU_TTYLINE_BITS, 0},
    {"cs6", LINE, FNU_TTYLINE_6BITS, FNU_TTYLINE_BITS, 0},
    {"cs7", LINE, FNU_TTYLINE_7BITS, FNU_TTYLINE_BITS, 0},
    {"cs8", LINE, FNU_TTYLINE_8BITS, FNU_TTYLINE_BITS, 0},
    {"parity", LINE, FNU_TTYLINE_PARITY | FNU_TTYLINE_7BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_BITS, 0},
    {"-parity", LINE, FNU_TTYLINE_8BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_PARITY | FNU_TTYLINE_BITS, 0},
    {"evenp", LINE, FNU_TTYLINE_PARITY | FNU_TTYLINE_7BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_BITS, 0},
    {"-evenp", LINE, FNU_TTYLINE_8BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_PARITY | FNU_TTYLINE_BITS, 0},
    {"oddp", LINE, FNU_TTYLINE_PARITY | FNU_TTYLINE_7BITS | FNU_TTYLINE_ODD,
     FNU_TTYLINE_BITS, 0},
    {"-oddp", LINE, FNU_TTYLINE_8BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_PARITY | FNU_TTYLINE_BITS, 0},
    {"pass8", LINE, FNU_TTYLINE_8BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_PARITY | FNU_TTYLINE_BITS, 0},
    {"-pass8", LINE, FNU_TTYLINE_PARITY | FNU_TTYLINE_7BITS,
     FNU_TTYLINE_ODD | FNU_TTYLINE_BITS, 0},
    {0, 0, 0, 0, 0}};

static const char *const fieldnames[] = {"lflags", "iflags", "oflags", "cflags"};

#define KEY_MIN (-1)
#define KEY_TIME (-2)

static const struct key {
  const char *name;
  int index;
  int alias;
} keys[] = {{"discard", FNU_TTYKEY_DISCARD, 0},
            {"dsusp", FNU_TTYKEY_DELAYSUSP, 0},
            {"eof", FNU_TTYKEY_EOF, 0},
            {"eol", FNU_TTYKEY_EOL, 0},
            {"eol2", FNU_TTYKEY_EOL2, 0},
            {"erase", FNU_TTYKEY_ERASE, 0},
            {"erase2", FNU_TTYKEY_ERASE2, 0},
            {"intr", FNU_TTYKEY_INTERRUPT, 0},
            {"kill", FNU_TTYKEY_KILL, 0},
            {"lnext", FNU_TTYKEY_LITERAL, 0},
            {"min", KEY_MIN, 0},
            {"quit", FNU_TTYKEY_QUIT, 0},
            {"reprint", FNU_TTYKEY_REPRINT, 0},
            {"start", FNU_TTYKEY_START, 0},
            {"status", FNU_TTYKEY_STATUS, 0},
            {"stop", FNU_TTYKEY_STOP, 0},
            {"susp", FNU_TTYKEY_SUSPEND, 0},
            {"time", KEY_TIME, 0},
            {"werase", FNU_TTYKEY_WORDERASE, 0},
            {"brk", FNU_TTYKEY_EOL, 1},
            {"flush", FNU_TTYKEY_DISCARD, 1},
            {"rprnt", FNU_TTYKEY_REPRINT, 1},
            {0, 0, 0}};

static int fd;
static struct fnu_ttymode t, def;
static struct fnu_ttysize win;
static int set, wset;

static void usage(void) {
  fprintf(stderr, "usage: stty [-a | -e | -g] [-f file] [operands]\n");
  exit(1);
}

static void defaults(struct fnu_ttymode *d) {
  static const uint8 k[FNU_TTYKEY_COUNT] = FNU_TTYDEF_KEYS;

  memset(d, 0, sizeof(*d));
  d->in = FNU_TTYDEF_IN;
  d->out = FNU_TTYDEF_OUT;
  d->line = FNU_TTYDEF_LINE;
  d->edit = FNU_TTYDEF_EDIT;
  memcpy(d->key, k, sizeof(k));
  d->min = 1;
  d->speed_in = d->speed_out = FNU_TTY_SPEED;
}

static uint32_t *field(struct fnu_ttymode *m, int f) {
  return f == EDIT ? &m->edit : f == IN ? &m->in : f == OUT ? &m->out : &m->line;
}

static uint8 *slot(struct fnu_ttymode *m, int index) {
  return index == KEY_MIN ? &m->min : index == KEY_TIME ? &m->time : &m->key[index];
}

static void ctl(uint64_t request, uint64_t arg, const char *what) {
  int r;

  if ((r = fnu_ioctl(fd, request, arg)) < 0)
    errx(1, "%s: %s", what, fnu_errstr(r));
}

static int col;
static const char *label;

static void binit(const char *lb) {
  if (col) {
    printf("\n");
    col = 0;
  }
  label = lb;
}

static void bput(const char *s) {
  if (col == 0)
    col = printf("%s: %s", label, s);
  else if (col + (int)strlen(s) + 1 > LINELENGTH)
    col = printf("\n\t%s", s) + 7;
  else
    col += printf(" %s", s);
}

static const char *ccval(int index, int c) {
  static char buf[8];
  char *p = buf;

  if (index == KEY_MIN || index == KEY_TIME) {
    snprintf(buf, sizeof(buf), "%d", c);
    return buf;
  }
  if (c == FNU_TTYKEY_OFF)
    return "<undef>";
  if (c & 0200) {
    *p++ = 'M';
    *p++ = '-';
    c &= 0177;
  }
  if (c == 0177) {
    *p++ = '^';
    *p++ = '?';
  } else if (c < 040) {
    *p++ = '^';
    *p++ = (char)(c + '@');
  } else
    *p++ = (char)c;
  *p = 0;
  return buf;
}

static void print(int fmt) {
  const struct mode *m;
  const struct key *k;
  char names[LINELENGTH + 8], vals[LINELENGTH + 8], name[16];
  uint32_t v;
  int f, on, n = 0;

  if (t.speed_in != t.speed_out)
    printf("ispeed %u baud; ospeed %u baud;", t.speed_in, t.speed_out);
  else
    printf("speed %u baud;", t.speed_out);
  if (fmt == BSD)
    printf(" %u rows; %u columns;", win.rows, win.cols);
  printf("\n");
  for (f = EDIT; f <= LINE; f++) {
    binit(fieldnames[f]);
    v = *field(&t, f);
    if (f == LINE)
      bput((v & FNU_TTYLINE_BITS) == FNU_TTYLINE_5BITS   ? "cs5"
           : (v & FNU_TTYLINE_BITS) == FNU_TTYLINE_6BITS ? "cs6"
           : (v & FNU_TTYLINE_BITS) == FNU_TTYLINE_7BITS ? "cs7"
                                                         : "cs8");
    for (m = modes; m->name != 0; m++) {
      if (m->field != f || !(m->how & SHOWN))
        continue;
      on = (v & m->set) == m->set;
      if (fmt != CHANGED || on != ((*field(&def, f) & m->set) == m->set)) {
        snprintf(name, sizeof(name), "%s%s", on ? "" : "-", m->name);
        bput(name);
      }
    }
  }
  binit(0);
  if (fmt == POSIX) {
    binit("cchars");
    for (k = keys; k->name != 0; k++)
      if (!k->alias) {
        snprintf(names, sizeof(names), "%s = %s;", k->name,
                 ccval(k->index, *slot(&t, k->index)));
        bput(names);
      }
    binit(0);
    return;
  }
  for (k = keys; k->name != 0; k++) {
    if (k->alias || (fmt == CHANGED && *slot(&t, k->index) == *slot(&def, k->index)))
      continue;
    snprintf(names + n * 8, sizeof(names) - (size_t)n * 8, "%-8s", k->name);
    snprintf(vals + n * 8, sizeof(vals) - (size_t)n * 8, "%-8s",
             ccval(k->index, *slot(&t, k->index)));
    if (++n == LINELENGTH / 8) {
      printf("%s\n%s\n", names, vals);
      n = 0;
    }
  }
  if (n)
    printf("%s\n%s\n", names, vals);
}

static void gprint(void) {
  const struct key *k;

  printf("fnutty:in=%x:out=%x:line=%x:edit=%x:ispeed=%u:ospeed=%u", t.in, t.out,
         t.line, t.edit, t.speed_in, t.speed_out);
  for (k = keys; k->name != 0; k++)
    if (!k->alias)
      printf(":%s=%x", k->name, *slot(&t, k->index));
  printf("\n");
}

static void gread(char *s) {
  const struct key *k;
  char *name, *val, *end;
  unsigned long v;

  for (s += strlen("fnutty:"); (name = strsep(&s, ":")) != 0;) {
    if ((val = strchr(name, '=')) == 0)
      errx(1, "illegal gfmt1 entry: %s", name);
    *val++ = 0;
    v = strtoul(val, &end, strncmp(name, "ispeed", 6) == 0 ||
                               strncmp(name, "ospeed", 6) == 0 ? 10 : 16);
    if (*val == 0 || *end != 0)
      errx(1, "illegal value: %s=%s", name, val);
    if (strcmp(name, "in") == 0)
      t.in = (uint32_t)v;
    else if (strcmp(name, "out") == 0)
      t.out = (uint32_t)v;
    else if (strcmp(name, "line") == 0)
      t.line = (uint32_t)v;
    else if (strcmp(name, "edit") == 0)
      t.edit = (uint32_t)v;
    else if (strcmp(name, "ispeed") == 0)
      t.speed_in = (uint32_t)v;
    else if (strcmp(name, "ospeed") == 0)
      t.speed_out = (uint32_t)v;
    else {
      for (k = keys; k->name != 0 && strcmp(k->name, name) != 0; k++)
        ;
      if (k->name == 0 || v > 0xff)
        errx(1, "illegal gfmt1 entry: %s", name);
      *slot(&t, k->index) = (uint8)v;
    }
  }
  set = 1;
}

static unsigned long number(const char *what, const char *s, unsigned long max) {
  char *end;
  unsigned long v;

  if (s == 0)
    errx(1, "option requires an argument -- %s", what);
  v = strtoul(s, &end, 10);
  if (*s < '0' || *s > '9' || *end != 0 || v > max)
    errx(1, "%s %s: invalid", what, s);
  return v;
}

static int cchar(char **argv) {
  const struct key *k;
  const char *v = argv[1];

  for (k = keys; k->name != 0 && strcmp(k->name, argv[0]) != 0; k++)
    ;
  if (k->name == 0)
    return 0;
  if (v == 0)
    errx(1, "option requires an argument -- %s", argv[0]);
  if (k->index == KEY_MIN || k->index == KEY_TIME)
    *slot(&t, k->index) = (uint8)number(k->name, v, 0xff);
  else if (strcmp(v, "undef") == 0 || strcmp(v, "<undef>") == 0 ||
           strcmp(v, "^-") == 0 || v[0] == 0)
    *slot(&t, k->index) = FNU_TTYKEY_OFF;
  else if (v[0] == '^' && v[1] != 0 && v[2] == 0)
    *slot(&t, k->index) = v[1] == '?' ? 0177 : (uint8)(v[1] & 037);
  else if (v[1] == 0)
    *slot(&t, k->index) = (uint8)v[0];
  else
    errx(1, "%s: %s: value must be one character", k->name, v);
  set = 1;
  return 2;
}

static int mode(const char *name) {
  const struct mode *m;
  uint32_t *v;
  int on;

  for (m = modes; m->name != 0 && strcmp(m->name, name) != 0; m++)
    ;
  if (m->name == 0)
    return 0;
  v = field(&t, m->field);
  *v = (*v & ~m->clear) | m->set;
  if (m->how & EXTERNAL) {
    on = m->set != 0;
    ctl(FNU_TTYEXTERNAL, (uint64_t)(uintptr_t)&on, "extproc");
  } else
    set = 1;
  return 1;
}

static void sane(void) {
  t.line = (FNU_TTYDEF_LINE & ~FNU_TTYLINE_LOCAL) |
           (t.line & (FNU_TTYLINE_LOCAL | FNU_TTYLINE_HWFLOW));
  t.in = FNU_TTYDEF_IN;
  t.edit = FNU_TTYDEF_EDIT |
           (t.edit & (FNU_TTYEDIT_ECHOKILLRUB | FNU_TTYEDIT_ECHOERASE |
                      FNU_TTYEDIT_ECHOKILL | FNU_TTYEDIT_ECHOPRINT | FNU_TTYEDIT_ECHOCTL |
                      FNU_TTYEDIT_ALTWERASE | FNU_TTYEDIT_STOPBG | FNU_TTYEDIT_NOFLUSH));
  t.out = FNU_TTYDEF_OUT;
}

static int keyword(char **argv) {
  const char *a = argv[0];
  int off = a[0] == '-';

  if (off)
    a++;
  if (strcmp(a, "raw") == 0 || strcmp(a, "cbreak") == 0) {
    if (off)
      sane();
    else if (a[0] == 'r') {
      t.in &= ~(FNU_TTYIN_BELLFULL | FNU_TTYIN_IGNBREAK | FNU_TTYIN_BREAKINT |
                FNU_TTYIN_MARKPARITY | FNU_TTYIN_STRIP | FNU_TTYIN_NLTOCR |
                FNU_TTYIN_DROPCR | FNU_TTYIN_CRTONL | FNU_TTYIN_FLOWOUT);
      t.out &= ~FNU_TTYOUT_PROCESS;
      t.edit &= ~(FNU_TTYEDIT_ECHO | FNU_TTYEDIT_ECHONL | FNU_TTYEDIT_LINES |
                  FNU_TTYEDIT_SIGNALS | FNU_TTYEDIT_EXTENDED);
      t.line = (t.line & ~(FNU_TTYLINE_BITS | FNU_TTYLINE_PARITY)) | FNU_TTYLINE_8BITS;
      t.min = 1;
      t.time = 0;
    } else {
      t.in |= FNU_TTYIN_BREAKINT | FNU_TTYIN_FLOWOUT | FNU_TTYIN_BELLFULL;
      t.out |= FNU_TTYOUT_PROCESS;
      t.edit = (t.edit | FNU_TTYEDIT_SIGNALS | FNU_TTYEDIT_EXTENDED) & ~FNU_TTYEDIT_LINES;
    }
  } else if (strcmp(a, "nl") == 0) {
    if (off) {
      t.in |= FNU_TTYIN_CRTONL;
      t.out |= FNU_TTYOUT_NLTOCRNL;
    } else {
      t.in &= ~FNU_TTYIN_CRTONL;
      t.out &= ~FNU_TTYOUT_NLTOCRNL;
    }
  } else if (strcmp(a, "lcase") == 0) {
    if (off) {
      t.in &= ~FNU_TTYIN_TOLOWER;
      t.out &= ~FNU_TTYOUT_TOUPPER;
      t.edit &= ~FNU_TTYEDIT_CASEESC;
    } else {
      t.in |= FNU_TTYIN_TOLOWER;
      t.out |= FNU_TTYOUT_TOUPPER;
      t.edit |= FNU_TTYEDIT_CASEESC;
    }
  } else if (off)
    return 0;
  else if (strcmp(a, "sane") == 0 || strcmp(a, "cooked") == 0)
    sane();
  else if (strcmp(a, "ek") == 0) {
    t.key[FNU_TTYKEY_ERASE] = def.key[FNU_TTYKEY_ERASE];
    t.key[FNU_TTYKEY_KILL] = def.key[FNU_TTYKEY_KILL];
  } else if (strcmp(a, "dec") == 0) {
    t.key[FNU_TTYKEY_ERASE] = 0177;
    t.key[FNU_TTYKEY_KILL] = 'U' & 037;
    t.key[FNU_TTYKEY_INTERRUPT] = 'C' & 037;
    t.edit = (t.edit & ~FNU_TTYEDIT_ECHOPRINT) | FNU_TTYEDIT_ECHOERASE |
             FNU_TTYEDIT_ECHOKILLRUB | FNU_TTYEDIT_ECHOCTL;
    t.in &= ~FNU_TTYIN_ANYSTART;
  } else if (strcmp(a, "speed") == 0) {
    printf("%u\n", t.speed_out);
    return 1;
  } else if (strcmp(a, "ispeed") == 0 || strcmp(a, "ospeed") == 0) {
    *(a[0] == 'i' ? &t.speed_in : &t.speed_out) =
        (uint32_t)number(a, argv[1], 0xffffffffUL);
    set = 1;
    return 2;
  } else if (strcmp(a, "rows") == 0 || strcmp(a, "cols") == 0 ||
             strcmp(a, "columns") == 0) {
    *(a[0] == 'r' ? &win.rows : &win.cols) = (uint16)number(a, argv[1], 0xffff);
    wset = 1;
    return 2;
  } else if (strcmp(a, "size") == 0) {
    printf("%u %u\n", win.rows, win.cols);
    return 1;
  } else if (strcmp(a, "all") == 0 || strcmp(a, "everything") == 0) {
    print(BSD);
    return 1;
  } else if (strcmp(a, "ostart") == 0 || strcmp(a, "ostop") == 0) {
    ctl(a[3] == 'a' ? FNU_TTYSTART : FNU_TTYSTOP, 0, a);
    return 1;
  } else
    return 0;
  set = 1;
  return 1;
}

int main(int argc, char **argv) {
  int fmt = CHANGED, g = 0, i, n, r;
  char *a;

  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0 &&
              strspn(argv[i], "-aefg") == strlen(argv[i]);
       i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    for (a = argv[i] + 1; *a != 0; a++)
      if (*a == 'a')
        fmt = POSIX;
      else if (*a == 'e')
        fmt = BSD;
      else if (*a == 'g')
        g = 1;
      else if (*a == 'f') {
        if (++i >= argc)
          usage();
        if ((fd = fnu_open(FNU_FD_CWD, argv[i], FNU_OPEN_READ | FNU_OPEN_NOWAIT, 0)) < 0)
          errx(1, "%s: %s", argv[i], fnu_errstr(fd));
        break;
      } else
        usage();
  }
  if ((r = fnu_ioctl(fd, FNU_TTYGETMODE, (uint64_t)(uintptr_t)&t)) < 0)
    errx(1, "not a terminal: %s", fnu_errstr(r));
  fnu_ioctl(fd, FNU_TTYGETSIZE, (uint64_t)(uintptr_t)&win);
  defaults(&def);
  if (i == argc) {
    if (g)
      gprint();
    else
      print(fmt);
    return 0;
  }
  if (g || fmt != CHANGED)
    usage();
  for (; i < argc; i += n) {
    if ((n = keyword(argv + i)) || (n = mode(argv[i])) || (n = cchar(argv + i)))
      continue;
    if (argv[i][0] >= '0' && argv[i][0] <= '9') {
      t.speed_in = t.speed_out = (uint32_t)number("speed", argv[i], 0xffffffffUL);
      set = 1;
      n = 1;
    } else if (strncmp(argv[i], "fnutty:", 7) == 0) {
      gread(argv[i]);
      n = 1;
    } else {
      warnx("illegal option -- %s", argv[i]);
      usage();
    }
  }
  if (set)
    ctl(FNU_TTYSETMODE, (uint64_t)(uintptr_t)&t, "set the mode");
  if (wset)
    ctl(FNU_TTYSETSIZE, (uint64_t)(uintptr_t)&win, "set the size");
  return 0;
}
