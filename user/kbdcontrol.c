// KBDCONTROL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <err.h>
#include <fnu/sys.h>

#define SPECIAL 0x80000000
#define KEYMAP_PATH "/usr/share/vt/keymaps/"

static const char ctrl_names[32][4] = {
    "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel", "bs ", "ht ", "nl ",
    "vt ", "ff ", "cr ", "so ", "si ", "dle", "dc1", "dc2", "dc3", "dc4", "nak",
    "syn", "etb", "can", "em ", "sub", "esc", "fs ", "gs ", "rs ", "us "};

static const char acc_names[FNU_KB_NACCENTS][5] = {"dgra", "dacu", "dcir", "dtil", "dmac",
                                      "dbre", "ddot", "duml", "dsla", "drin",
                                      "dced", "dapo", "ddac", "dogo", "dcar"};

static const char acc_names_u[FNU_KB_NACCENTS][5] = {"DGRA", "DACU", "DCIR", "DTIL", "DMAC",
                                        "DBRE", "DDOT", "DUML", "DSLA", "DRIN",
                                        "DCED", "DAPO", "DDAC", "DOGO", "DCAR"};

static const char fkey_table[FNU_KB_NFKEYS][FNU_KB_FKEYLEN + 1] = {
    "\033[M", "\033[N", "\033[O", "\033[P",  "\033[Q", "\033[R", "\033[S", "\033[T",
    "\033[U", "\033[V", "\033[W", "\033[X",  "\033[Y", "\033[Z", "\033[a", "\033[b",
    "\033[c", "\033[d", "\033[e", "\033[f",  "\033[g", "\033[h", "\033[i", "\033[j",
    "\033[k", "\033[l", "\033[m", "\033[n",  "\033[o", "\033[p", "\033[q", "\033[r",
    "\033[s", "\033[t", "\033[u", "\033[v",  "\033[w", "\033[x", "\033[y", "\033[z",
    "\033[@", "\033[[", "\033[\\", "\033[]", "\033[^", "\033[_", "\033[`", "\033[{",
    "\033[H", "\033[A", "\033[I", "-",       "\033[D", "\033[E", "\033[C", "+",
    "\033[F", "\033[B", "\033[G", "\033[L",  "\177",   "\033[J", "\033[~", "\033[}",
    "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
    "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""};

static const int kbdelays[] = {250, 500, 750, 1000};
static const int kbrates[] = {34,  38,  42,  46,  50,  55,  59,  63,  68,  76,  84,
                              92,  100, 110, 118, 126, 136, 152, 168, 184, 200, 220,
                              236, 252, 272, 304, 336, 368, 400, 440, 472, 504};

static int hex;
static int token;
static int number;
static char letter;

static char **paths;
static int npaths;

static void usage(void) {
  dprintf(2, "usage: kbdcontrol [-dFix] [-r delay.repeat | speed] [-l mapfile]\n"
             "                  [-f # string] [-L mapfile] [-P path]\n");
  exit(1);
}

static void complain(const char *fmt, const char *arg) {
  dprintf(2, "kbdcontrol: ");
  dprintf(2, fmt, arg);
  dprintf(2, "\n");
}

// THE LEXER
enum {
  TNOP = 256, TLSH, TRSH, TCLK, TNLK, TSLK, TLALT, TBTAB, TLCTR, TNEXT, TPREV,
  TRCTR, TRALT, TALK, TASH, TMETA, TRBT, TDBG, TSUSP, TSPSC, TPANIC, TLSHA,
  TRSHA, TLCTRA, TRCTRA, TLALTA, TRALTA, THALT, TPDWN, TPASTE, TACC, TFUNC,
  TSCRN, TLET, TNUM, TFLAG
};

static const struct {
  const char *word;
  int tok;
  int num;
} words[] = {
    {"nop", TNOP, 0},      {"lshift", TLSH, 0},   {"rshift", TRSH, 0},
    {"clock", TCLK, 0},    {"nlock", TNLK, 0},    {"slock", TSLK, 0},
    {"lalt", TLALT, 0},    {"alt", TLALT, 0},     {"btab", TBTAB, 0},
    {"lctrl", TLCTR, 0},   {"ctrl", TLCTR, 0},    {"nscr", TNEXT, 0},
    {"pscr", TPREV, 0},    {"rctrl", TRCTR, 0},   {"ralt", TRALT, 0},
    {"alock", TALK, 0},    {"ashift", TASH, 0},   {"meta", TMETA, 0},
    {"boot", TRBT, 0},     {"debug", TDBG, 0},    {"susp", TSUSP, 0},
    {"saver", TSPSC, 0},   {"panic", TPANIC, 0},  {"lshifta", TLSHA, 0},
    {"shifta", TLSHA, 0},  {"rshifta", TRSHA, 0}, {"lctrla", TLCTRA, 0},
    {"ctrla", TLCTRA, 0},  {"rctrla", TRCTRA, 0}, {"lalta", TLALTA, 0},
    {"alta", TLALTA, 0},   {"ralta", TRALTA, 0},  {"halt", THALT, 0},
    {"pdwn", TPDWN, 0},    {"paste", TPASTE, 0},
    {"nul", TNUM, 0},  {"soh", TNUM, 1},  {"stx", TNUM, 2},  {"etx", TNUM, 3},
    {"eot", TNUM, 4},  {"enq", TNUM, 5},  {"ack", TNUM, 6},  {"bel", TNUM, 7},
    {"bs", TNUM, 8},   {"ht", TNUM, 9},   {"lf", TNUM, 10},  {"nl", TNUM, 10},
    {"vt", TNUM, 11},  {"ff", TNUM, 12},  {"np", TNUM, 12},  {"cr", TNUM, 13},
    {"so", TNUM, 14},  {"si", TNUM, 15},  {"dle", TNUM, 16}, {"dc1", TNUM, 17},
    {"dc2", TNUM, 18}, {"dc3", TNUM, 19}, {"dc4", TNUM, 20}, {"nak", TNUM, 21},
    {"syn", TNUM, 22}, {"etb", TNUM, 23}, {"can", TNUM, 24}, {"em", TNUM, 25},
    {"sub", TNUM, 26}, {"esc", TNUM, 27}, {"fs", TNUM, 28},  {"gs", TNUM, 29},
    {"rs", TNUM, 30},  {"ns", TNUM, 31},  {"us", TNUM, 31},  {"sp", TNUM, 32},
    {"del", TNUM, 127},
};

static FILE *in;
static int pushback = -1;

static int getch(void) {
  int c;

  if (pushback >= 0) {
    c = pushback;
    pushback = -1;
    return c;
  }
  return getc(in);
}

static void ungetch(int c) { pushback = c; }

static int strcaseeq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((uchar)*a) != tolower((uchar)*b))
      return 0;
  return *a == 0 && *b == 0;
}

static int yylex(void) {
  char word[64];
  int c, n, i;

  for (;;) {
    c = getch();
    if (c < 0)
      return 0;
    if (c == '#') {
      while ((c = getch()) >= 0 && c != '\n')
        ;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      continue;
    break;
  }
  if (c == '\'') {
    letter = (char)getch();
    if (getch() != '\'')
      return -1;
    return TLET;
  }
  if (isdigit(c)) {
    n = 0;
    word[n++] = (char)c;
    while ((c = getch()) >= 0 && (isalnum(c)) && n < 63)
      word[n++] = (char)c;
    word[n] = 0;
    if (c >= 0)
      ungetch(c);
    if (word[0] == '0' && (word[1] == 'x' || word[1] == 'X'))
      number = (int)strtol(word + 2, 0, 16);
    else
      number = atoi(word);
    return TNUM;
  }
  if (isalpha(c)) {
    n = 0;
    word[n++] = (char)c;
    while ((c = getch()) >= 0 && (isalnum(c)) && n < 63)
      word[n++] = (char)c;
    word[n] = 0;
    if (c >= 0)
      ungetch(c);
    if (n == 1 && (word[0] == 'O' || word[0] == 'C' || word[0] == 'N' || word[0] == 'B')) {
      number = word[0] == 'O' ? 0 : word[0] == 'C' ? 1 : word[0] == 'N' ? 2 : 3;
      return TFLAG;
    }
    if (strncmp(word, "fkey", 4) == 0 && isdigit(word[4])) {
      number = atoi(word + 4);
      return TFUNC;
    }
    if (strncmp(word, "scr", 3) == 0 && isdigit(word[3])) {
      number = atoi(word + 3);
      return TSCRN;
    }
    for (i = 0; i < FNU_KB_NACCENTS; i++)
      if (strcmp(word, acc_names[i]) == 0 || strcmp(word, acc_names_u[i]) == 0) {
        number = i;
        return TACC;
      }
    for (i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++)
      if (words[i].tok == TNUM ? strcaseeq(word, words[i].word) : strcmp(word, words[i].word) == 0) {
        number = words[i].num;
        return words[i].tok;
      }
    return -1;
  }
  return c;
}

static int get_entry(void) {
  switch ((token = yylex())) {
  case TNOP: return FNU_KEY_NOP | SPECIAL;
  case TLSH: return FNU_KEY_LSH | SPECIAL;
  case TRSH: return FNU_KEY_RSH | SPECIAL;
  case TCLK: return FNU_KEY_CLK | SPECIAL;
  case TNLK: return FNU_KEY_NLK | SPECIAL;
  case TSLK: return FNU_KEY_SLK | SPECIAL;
  case TBTAB: return FNU_KEY_BTAB | SPECIAL;
  case TLALT: return FNU_KEY_LALT | SPECIAL;
  case TLCTR: return FNU_KEY_LCTR | SPECIAL;
  case TNEXT: return FNU_KEY_NEXT | SPECIAL;
  case TPREV: return FNU_KEY_PREV | SPECIAL;
  case TRCTR: return FNU_KEY_RCTR | SPECIAL;
  case TRALT: return FNU_KEY_RALT | SPECIAL;
  case TALK: return FNU_KEY_ALK | SPECIAL;
  case TASH: return FNU_KEY_ASH | SPECIAL;
  case TMETA: return FNU_KEY_META | SPECIAL;
  case TRBT: return FNU_KEY_RBT | SPECIAL;
  case TDBG: return FNU_KEY_DBG | SPECIAL;
  case TSUSP: return FNU_KEY_SUSP | SPECIAL;
  case TSPSC: return FNU_KEY_SPSC | SPECIAL;
  case TPANIC: return FNU_KEY_PNC | SPECIAL;
  case TLSHA: return FNU_KEY_LSHA | SPECIAL;
  case TRSHA: return FNU_KEY_RSHA | SPECIAL;
  case TLCTRA: return FNU_KEY_LCTRA | SPECIAL;
  case TRCTRA: return FNU_KEY_RCTRA | SPECIAL;
  case TLALTA: return FNU_KEY_LALTA | SPECIAL;
  case TRALTA: return FNU_KEY_RALTA | SPECIAL;
  case THALT: return FNU_KEY_HALT | SPECIAL;
  case TPDWN: return FNU_KEY_PDWN | SPECIAL;
  case TPASTE: return FNU_KEY_PASTE | SPECIAL;
  case TACC:
    if (FNU_KEY_ACC(number) > FNU_KEY_L_ACC)
      return -1;
    return FNU_KEY_ACC(number) | SPECIAL;
  case TFUNC:
    if (FNU_KEY_F(number) > FNU_KEY_L_FN)
      return -1;
    return FNU_KEY_F(number) | SPECIAL;
  case TSCRN:
    if (FNU_KEY_S(number) > FNU_KEY_L_SCR)
      return -1;
    return FNU_KEY_S(number) | SPECIAL;
  case TLET:
    return (uchar)letter;
  case TNUM:
    if (number < 0 || number > 0x10ffff)
      return -1;
    return number;
  default:
    return -1;
  }
}

static int get_key_definition_line(struct fnu_keymap *map) {
  int i, def, scancode;

  if (number < 0 || number >= FNU_KB_NKEYS)
    return -1;
  scancode = number;

  map->key[scancode].spcl = 0;
  for (i = 0; i < FNU_KB_NSTATES; i++) {
    if ((def = get_entry()) == -1)
      return -1;
    if (def & SPECIAL)
      map->key[scancode].spcl |= (uchar)(0x80 >> i);
    map->key[scancode].map[i] = (uint32)(def & ~SPECIAL);
  }
  if ((token = yylex()) != TFLAG)
    return -1;
  map->key[scancode].flgs = (uchar)number;
  token = yylex();
  return scancode + 1;
}

static int get_accent_definition_line(struct fnu_accentmap *map) {
  int accent, c1, c2, i;

  if (FNU_KEY_ACC(number) < FNU_KEY_F_ACC || FNU_KEY_ACC(number) > FNU_KEY_L_ACC)
    return -1;
  accent = number;
  if (map->acc[accent].accchar != 0) {
    dprintf(2, "kbdcontrol: duplicated accent key definition\n");
    exit(1);
  }

  switch ((token = yylex())) {
  case TLET:
    map->acc[accent].accchar = (uchar)letter;
    break;
  case TNUM:
    map->acc[accent].accchar = (uint32)number;
    break;
  default:
    return -1;
  }

  for (i = 0; (token = yylex()) == '(';) {
    switch ((token = yylex())) {
    case TLET: c1 = (uchar)letter; break;
    case TNUM: c1 = number; break;
    default: return -1;
    }
    switch ((token = yylex())) {
    case TLET: c2 = (uchar)letter; break;
    case TNUM: c2 = number; break;
    default: return -1;
    }
    if ((token = yylex()) != ')')
      return -1;
    if (i >= FNU_KB_NACCENTCHARS) {
      dprintf(2, "kbdcontrol: too many accented characters, ignored\n");
      continue;
    }
    map->acc[accent].map[i][0] = (uint32)c1;
    map->acc[accent].map[i][1] = (uint32)c2;
    ++i;
  }
  return accent + 1;
}

static int get_definition_line(struct fnu_keymap *keymap, struct fnu_accentmap *accentmap) {
  int c;

  if (token < 0)
    token = yylex();
  switch (token) {
  case TNUM:
    c = get_key_definition_line(keymap);
    if (c < 0) {
      dprintf(2, "kbdcontrol: invalid key definition\n");
      exit(1);
    }
    if (c > keymap->n_keys)
      keymap->n_keys = (uint16)c;
    break;
  case TACC:
    c = get_accent_definition_line(accentmap);
    if (c < 0) {
      dprintf(2, "kbdcontrol: invalid accent key definition\n");
      exit(1);
    }
    if (c > accentmap->n_accs)
      accentmap->n_accs = (uint16)c;
    break;
  case 0:
    return -1;
  default:
    dprintf(2, "kbdcontrol: illegal definition line\n");
    exit(1);
  }
  return c;
}

// THE PRINTERS

static void print_entry(int value) {
  int val = value & ~SPECIAL;

  switch (value) {
  case FNU_KEY_NOP | SPECIAL: printf(" nop   "); break;
  case FNU_KEY_LSH | SPECIAL: printf(" lshift"); break;
  case FNU_KEY_RSH | SPECIAL: printf(" rshift"); break;
  case FNU_KEY_CLK | SPECIAL: printf(" clock "); break;
  case FNU_KEY_NLK | SPECIAL: printf(" nlock "); break;
  case FNU_KEY_SLK | SPECIAL: printf(" slock "); break;
  case FNU_KEY_BTAB | SPECIAL: printf(" btab  "); break;
  case FNU_KEY_LALT | SPECIAL: printf(" lalt  "); break;
  case FNU_KEY_LCTR | SPECIAL: printf(" lctrl "); break;
  case FNU_KEY_NEXT | SPECIAL: printf(" nscr  "); break;
  case FNU_KEY_PREV | SPECIAL: printf(" pscr  "); break;
  case FNU_KEY_RCTR | SPECIAL: printf(" rctrl "); break;
  case FNU_KEY_RALT | SPECIAL: printf(" ralt  "); break;
  case FNU_KEY_ALK | SPECIAL: printf(" alock "); break;
  case FNU_KEY_ASH | SPECIAL: printf(" ashift"); break;
  case FNU_KEY_META | SPECIAL: printf(" meta  "); break;
  case FNU_KEY_RBT | SPECIAL: printf(" boot  "); break;
  case FNU_KEY_DBG | SPECIAL: printf(" debug "); break;
  case FNU_KEY_SUSP | SPECIAL: printf(" susp  "); break;
  case FNU_KEY_SPSC | SPECIAL: printf(" saver "); break;
  case FNU_KEY_PNC | SPECIAL: printf(" panic "); break;
  case FNU_KEY_LSHA | SPECIAL: printf(" lshifta"); break;
  case FNU_KEY_RSHA | SPECIAL: printf(" rshifta"); break;
  case FNU_KEY_LCTRA | SPECIAL: printf(" lctrla"); break;
  case FNU_KEY_RCTRA | SPECIAL: printf(" rctrla"); break;
  case FNU_KEY_LALTA | SPECIAL: printf(" lalta "); break;
  case FNU_KEY_RALTA | SPECIAL: printf(" ralta "); break;
  case FNU_KEY_HALT | SPECIAL: printf(" halt  "); break;
  case FNU_KEY_PDWN | SPECIAL: printf(" pdwn  "); break;
  case FNU_KEY_PASTE | SPECIAL: printf(" paste "); break;
  default:
    if (value & SPECIAL) {
      if (val >= FNU_KEY_F_FN && val <= FNU_KEY_L_FN)
        printf(" fkey%02d", val - FNU_KEY_F_FN + 1);
      else if (val >= FNU_KEY_F_SCR && val <= FNU_KEY_L_SCR)
        printf(" scr%02d ", val - FNU_KEY_F_SCR + 1);
      else if (val >= FNU_KEY_F_ACC && val <= FNU_KEY_L_ACC)
        printf(" %-6s", acc_names[val - FNU_KEY_F_ACC]);
      else if (hex)
        printf(" 0x%02x  ", val);
      else
        printf(" %3d   ", val);
    } else {
      if (val < ' ')
        printf(" %s   ", ctrl_names[val]);
      else if (val == 127)
        printf(" del   ");
      else if (val < 128 && isprint(val))
        printf(" '%c'   ", val);
      else if (hex)
        printf(" 0x%02x  ", val);
      else
        printf(" %3d   ", val);
    }
  }
}

static void print_key_definition_line(int scancode, struct fnu_keyent *key) {
  int i;

  if (hex)
    printf(" 0x%02x  ", scancode);
  else
    printf("  %03d  ", scancode);

  for (i = 0; i < FNU_KB_NSTATES; i++) {
    if (key->spcl & (0x80 >> i))
      print_entry((int)key->map[i] | SPECIAL);
    else
      print_entry((int)key->map[i]);
  }

  switch (key->flgs) {
  case 0: printf("  O\n"); break;
  case 1: printf("  C\n"); break;
  case 2: printf("  N\n"); break;
  case 3: printf("  B\n"); break;
  }
}

static void print_accent_definition_line(int accent, struct fnu_accent *key) {
  int c, i;

  if (key->accchar == 0)
    return;

  printf("  %-6s", acc_names[accent]);
  if (key->accchar < 128 && isprint((int)key->accchar))
    printf("'%c'  ", key->accchar);
  else if (hex)
    printf("0x%02x ", key->accchar);
  else
    printf("%03d  ", key->accchar);

  for (i = 0; i < FNU_KB_NACCENTCHARS; ++i) {
    c = (int)key->map[i][0];
    if (c == 0)
      break;
    if ((i > 0) && ((i % 4) == 0))
      printf("\n             ");
    if (c < 128 && isprint(c))
      printf("( '%c' ", c);
    else if (hex)
      printf("(0x%02x ", c);
    else
      printf("( %03d ", c);
    c = (int)key->map[i][1];
    if (c < 128 && isprint(c))
      printf("'%c' ) ", c);
    else if (hex)
      printf("0x%02x) ", c);
    else
      printf("%03d ) ", c);
  }
  printf("\n");
}

static void c_char(int c, char *buf, size_t n) {
  if (c == '\'' || c == '\\')
    snprintf(buf, n, "'\\%c'", c);
  else if (c < 128 && isprint(c))
    snprintf(buf, n, "'%c'", c);
  else
    snprintf(buf, n, "0x%02x", c);
}

static void dump_entry(int value) {
  int val = value & ~SPECIAL;
  const char *name = 0;
  char buf[24];

  if (value & SPECIAL) {
    if (val >= FNU_KEY_F_FN && val <= FNU_KEY_L_FN)
      snprintf(buf, sizeof(buf), "FNU_KEY_F(%2d)", val - FNU_KEY_F_FN + 1);
    else if (val >= FNU_KEY_F_SCR && val <= FNU_KEY_L_SCR)
      snprintf(buf, sizeof(buf), "FNU_KEY_S(%2d)", val - FNU_KEY_F_SCR + 1);
    else if (val >= FNU_KEY_F_ACC && val <= FNU_KEY_L_ACC)
      name = acc_names_u[val - FNU_KEY_F_ACC];
    else {
      switch (val) {
      case FNU_KEY_NOP: name = "NOP"; break;
      case FNU_KEY_LSH: name = "LSH"; break;
      case FNU_KEY_RSH: name = "RSH"; break;
      case FNU_KEY_CLK: name = "CLK"; break;
      case FNU_KEY_NLK: name = "NLK"; break;
      case FNU_KEY_SLK: name = "SLK"; break;
      case FNU_KEY_BTAB: name = "BTAB"; break;
      case FNU_KEY_LALT: name = "LALT"; break;
      case FNU_KEY_LCTR: name = "LCTR"; break;
      case FNU_KEY_NEXT: name = "NEXT"; break;
      case FNU_KEY_PREV: name = "PREV"; break;
      case FNU_KEY_RCTR: name = "RCTR"; break;
      case FNU_KEY_RALT: name = "RALT"; break;
      case FNU_KEY_ALK: name = "ALK"; break;
      case FNU_KEY_ASH: name = "ASH"; break;
      case FNU_KEY_META: name = "META"; break;
      case FNU_KEY_RBT: name = "RBT"; break;
      case FNU_KEY_DBG: name = "DBG"; break;
      case FNU_KEY_SUSP: name = "SUSP"; break;
      case FNU_KEY_SPSC: name = "SPSC"; break;
      case FNU_KEY_PNC: name = "PNC"; break;
      case FNU_KEY_LSHA: name = "LSHA"; break;
      case FNU_KEY_RSHA: name = "RSHA"; break;
      case FNU_KEY_LCTRA: name = "LCTRA"; break;
      case FNU_KEY_RCTRA: name = "RCTRA"; break;
      case FNU_KEY_LALTA: name = "LALTA"; break;
      case FNU_KEY_RALTA: name = "RALTA"; break;
      case FNU_KEY_HALT: name = "HALT"; break;
      case FNU_KEY_PDWN: name = "PDWN"; break;
      case FNU_KEY_PASTE: name = "PASTE"; break;
      default: snprintf(buf, sizeof(buf), "0x%02x", val); break;
      }
    }
    if (name != 0)
      snprintf(buf, sizeof(buf), "FNU_KEY_%s", name);
  } else
    c_char(val, buf, sizeof(buf));
  printf("%13s, ", buf);
}

static void dump_key_definition(char *name, struct fnu_keymap *keymap) {
  int i, j;

  printf("static const struct fnu_keymap keymap_%s = { 0x%02x, {\n", name, (unsigned)keymap->n_keys);
  for (i = 0; i < keymap->n_keys; i++) {
    printf("{{ ");
    for (j = 0; j < FNU_KB_NSTATES; j++) {
      if (keymap->key[i].spcl & (0x80 >> j))
        dump_entry((int)keymap->key[i].map[j] | SPECIAL);
      else
        dump_entry((int)keymap->key[i].map[j]);
    }
    printf("}, 0x%02x,0x%02x },\n", (unsigned)keymap->key[i].spcl, (unsigned)keymap->key[i].flgs);
  }
  printf("} };\n\n");
}

static void dump_accent_definition(char *name, struct fnu_accentmap *accentmap) {
  int i, j, c;
  char buf[8];

  printf("static const struct fnu_accentmap accentmap_%s = { %d", name, accentmap->n_accs);
  if (accentmap->n_accs <= 0) {
    printf(" };\n\n");
    return;
  }
  printf(", {\n");
  for (i = 0; i < FNU_KB_NACCENTS; i++) {
    printf("    /* %s=%d */\n    {  ", acc_names[i], i);
    c = (int)accentmap->acc[i].accchar;
    if (c == 0) {
      printf("0 },\n");
      continue;
    }
    c_char(c, buf, sizeof(buf));
    printf("%s, {", buf);
    for (j = 0; j < FNU_KB_NACCENTCHARS; j++) {
      c = (int)accentmap->acc[i].map[j][0];
      if (c == 0)
        break;
      if ((j > 0) && ((j % 4) == 0))
        printf("\n\t     ");
      c_char(c, buf, sizeof(buf));
      printf(" { %5s,", buf);
      c_char((int)accentmap->acc[i].map[j][1], buf, sizeof(buf));
      printf("%s },", buf);
    }
    printf(" }, },\n");
  }
  printf("} };\n\n");
}

// THE MAPS

static void add_keymap_path(const char *path) {
  if ((paths = reallocarray(paths, (size_t)npaths + 1, sizeof(*paths))) == 0)
    err(1, 0);
  paths[npaths++] = (char *)path;
}

static int open_keymap(const char *opt, char *found, int size) {
  static const char *postfix[] = {"", ".kbd", 0};
  int i, j, fd;

  for (i = 0; i < npaths; i++)
    for (j = 0; postfix[j]; j++) {
      snprintf(found, size, "%s%s%s", paths[i], opt, postfix[j]);
      if ((fd = open(found, O_RDONLY)) >= 0)
        return fd;
    }
  return -1;
}

static void load_keymap(char *opt, int dumponly) {
  static struct fnu_keymap keymap;
  static struct fnu_accentmap accentmap;
  char name[256], *cp;
  int fd;

  if ((fd = open_keymap(opt, name, sizeof(name))) < 0) {
    complain("keymap file \"%s\" not found", opt);
    return;
  }
  if ((in = fd == 0 ? stdin : fdopen(fd, "r")) == 0) {
    complain("keymap file \"%s\" cannot be read", opt);
    close(fd);
    return;
  }
  pushback = -1;
  memset(&keymap, 0, sizeof(keymap));
  memset(&accentmap, 0, sizeof(accentmap));
  token = -1;
  while (get_definition_line(&keymap, &accentmap) >= 0)
    ;
  if (in != stdin)
    fclose(in);
  if (dumponly) {
    for (cp = opt; *cp; cp++)
      if (!isalpha((uchar)*cp) && !isdigit((uchar)*cp))
        *cp = '_';
    printf("/*\n * Automatically generated from %s.\n * DO NOT EDIT!\n */\n", name);
    dump_key_definition(opt, &keymap);
    dump_accent_definition(opt, &accentmap);
    return;
  }
  if (keymap.n_keys > 0 && ioctl(0, FNU_KBSETMAP, (uint64_t)(uintptr_t)&keymap) < 0) {
    complain("setting keymap%s", "");
    return;
  }
  if (accentmap.n_accs > 0 && ioctl(0, FNU_KBSETACCENTS, (uint64_t)(uintptr_t)&accentmap) < 0)
    complain("setting accentmap%s", "");
}

static void print_keymap(void) {
  static struct fnu_keymap keymap;
  static struct fnu_accentmap accentmap;
  int i;

  if (ioctl(0, FNU_KBGETMAP, (uint64_t)(uintptr_t)&keymap) < 0) {
    complain("getting keymap: not a console%s", "");
    exit(1);
  }
  if (ioctl(0, FNU_KBGETACCENTS, (uint64_t)(uintptr_t)&accentmap) < 0)
    memset(&accentmap, 0, sizeof(accentmap));
  printf("#                                                         alt\n"
         "# scan                       cntrl          alt    alt   cntrl lock\n"
         "# code  base   shift  cntrl  shift  alt    shift  cntrl  shift state\n"
         "# ------------------------------------------------------------------\n");
  for (i = 0; i < keymap.n_keys; i++)
    print_key_definition_line(i, &keymap.key[i]);

  printf("\n");
  for (i = 0; i < FNU_KB_NACCENTS; i++)
    print_accent_definition_line(i, &accentmap.acc[i]);
}

static void load_default_functionkeys(void) {
  struct fnu_fkeyarg fkey;
  int i;

  for (i = 0; i < FNU_KB_NFKEYS; i++) {
    fkey.keynum = (uint16)i;
    fkey.flen = (uchar)strlen(fkey_table[i]);
    memcpy(fkey.keydef, fkey_table[i], fkey.flen);
    if (ioctl(0, FNU_KBSETFKEY, (uint64_t)(uintptr_t)&fkey) < 0)
      complain("setting function key%s", "");
  }
}

static void set_functionkey(char *keynumstr, char *string) {
  struct fnu_fkeyarg fkey;
  size_t len;
  char *end;
  long n;

  if (!strcmp(keynumstr, "load") && !strcmp(string, "default")) {
    load_default_functionkeys();
    return;
  }
  n = strtol(keynumstr, &end, 10);
  if (*keynumstr == 0 || *end != 0 || n < 1 || n > FNU_KB_NFKEYS) {
    dprintf(2, "kbdcontrol: function key number must be between 1 and %d\n", FNU_KB_NFKEYS);
    return;
  }
  if ((len = strlen(string)) > FNU_KB_FKEYLEN) {
    dprintf(2, "kbdcontrol: function key string too long (%zu > %d)\n", len, FNU_KB_FKEYLEN);
    return;
  }
  fkey.keynum = (uint16)(n - 1);
  fkey.flen = (uchar)len;
  memcpy(fkey.keydef, string, len);
  if (ioctl(0, FNU_KBSETFKEY, (uint64_t)(uintptr_t)&fkey) < 0)
    complain("setting function key%s", "");
}

static void set_keyrates(char *opt) {
  int arg[2], repeat, delay, r, d, n;
  char *v1;

  if (!strcmp(opt, "slow")) {
    delay = 1000;
    repeat = 504;
    d = 3;
    r = 31;
  } else if (!strcmp(opt, "normal")) {
    delay = 500;
    repeat = 126;
    d = 1;
    r = 15;
  } else if (!strcmp(opt, "fast")) {
    delay = 0;
    repeat = 0;
    d = 0;
    r = 0;
  } else {
    delay = (int)strtol(opt, &v1, 0);
    if ((delay < 0) || (*v1 != '.'))
      goto badopt;
    opt = ++v1;
    repeat = (int)strtol(opt, &v1, 0);
    if ((repeat < 0) || (*opt == '\0') || (*v1 != '\0')) {
    badopt:
      dprintf(2, "kbdcontrol: argument to -r must be delay.repeat or slow|normal|fast\n");
      return;
    }
    for (n = 0; n < 4 - 1; n++)
      if (delay <= kbdelays[n])
        break;
    d = n;
    for (n = 0; n < 32 - 1; n++)
      if (repeat <= kbrates[n])
        break;
    r = n;
  }

  arg[0] = delay;
  arg[1] = repeat;
  if (ioctl(0, FNU_KBSETREPEAT, (uint64_t)(uintptr_t)arg) < 0) {
    if (ioctl(0, FNU_KBSETRATE, (uint64_t)((d << 5) | r)) < 0)
      complain("setting keyboard rate%s", "");
  }
}

static const char *get_kbd_type_name(int type) {
  switch (type) {
  case FNU_KB_84: return "AT 84";
  case FNU_KB_101: return "AT 101/102";
  case FNU_KB_OTHER: return "generic";
  }
  return "unknown";
}

static void show_kbd_info(void) {
  struct fnu_kbinfo info;

  if (ioctl(0, FNU_KBINFO, (uint64_t)(uintptr_t)&info) < 0) {
    complain("unable to obtain keyboard information%s", "");
    return;
  }
  printf("kbd%d:\n", info.kb_index);
  printf("    %.*s%d, type:%s (%d)\n", (int)sizeof(info.kb_name), info.kb_name, info.kb_unit,
         get_kbd_type_name(info.kb_type), info.kb_type);
}

int main(int argc, char **argv) {
  const char *env;
  int i, did = 0;

  for (i = 1; i < argc; i++)
    if (strcmp(argv[i], "-P") == 0 && i + 1 < argc)
      add_keymap_path(argv[++i]);
  if ((env = getenv("KEYMAP_PATH")) != 0)
    add_keymap_path(env);
  add_keymap_path("");
  add_keymap_path(KEYMAP_PATH);

  for (i = 1; i < argc; i++) {
    const char *p = argv[i];

    if (p[0] != '-' || p[1] == 0)
      usage();
    switch (p[1]) {
    case 'd':
      print_keymap();
      break;
    case 'x':
      hex = 1;
      break;
    case 'F':
      load_default_functionkeys();
      break;
    case 'i':
      show_kbd_info();
      break;
    case 'l':
    case 'L':
    case 'r':
    case 'P':
      if (i + 1 >= argc)
        usage();
      if (p[1] == 'l')
        load_keymap(argv[++i], 0);
      else if (p[1] == 'L')
        load_keymap(argv[++i], 1);
      else if (p[1] == 'r')
        set_keyrates(argv[++i]);
      else
        i++;
      break;
    case 'f':
      if (i + 2 >= argc)
        usage();
      set_functionkey(argv[i + 1], argv[i + 2]);
      i += 2;
      break;
    default:
      usage();
    }
    did = 1;
  }
  if (!did)
    usage();
  exit(0);
}
