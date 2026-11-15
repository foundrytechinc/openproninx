#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define F_BLANKS 0x01  // b: leading blanks skipped
#define F_DICT 0x02
#define F_FOLD 0x04
#define F_PRINT 0x08   // i: only what prints
#define F_NUMBER 0x10  // n: an arithmetic value
#define F_REVERSE 0x20 // r: the other way
#define F_ENDBLANKS 0x40

struct key {
  int f1, c1, f2, c2;
  int flags;
};

struct line {
  char *text;
  size_t len;
};

static struct key *keys;
static int nkeys, global, stable, unique, tab = -1;

static void usage(void) {
  fprintf(stderr, "usage: sort [-bCcdfimnrsu] [-k field1[,field2]] [-o output] "
                  "[-t char] [file ...]\n");
  exit(2);
}

static const char *field(const char *s, const char *end, int n, const char **fend) {
  const char *p = s;

  while (--n > 0 && p < end) {
    if (tab >= 0) {
      while (p < end && *p != tab)
        p++;
      if (p < end)
        p++;
    } else {
      while (p < end && isblank((unsigned char)*p))
        p++;
      while (p < end && !isblank((unsigned char)*p))
        p++;
    }
  }
  if (fend != 0) {
    const char *q = p;

    if (tab >= 0)
      while (q < end && *q != tab)
        q++;
    else {
      while (q < end && isblank((unsigned char)*q))
        q++;
      while (q < end && !isblank((unsigned char)*q))
        q++;
    }
    *fend = q;
  }
  return p;
}

static void bounds(const struct line *l, const struct key *k, const char **b,
                   const char **e) {
  const char *end = l->text + l->len, *fe, *p;
  int c;

  p = field(l->text, end, k->f1, &fe);
  if (k->flags & F_BLANKS)
    while (p < fe && isblank((unsigned char)*p))
      p++;
  for (c = 1; c < k->c1 && p < fe; c++)
    p++;
  *b = p;
  if (k->f2 == 0) {
    *e = end;
  } else {
    p = field(l->text, end, k->f2, &fe);
    if (k->c2 == 0) {
      *e = fe;
    } else {
      if (k->flags & F_ENDBLANKS)
        while (p < fe && isblank((unsigned char)*p))
          p++;
      for (c = 0; c < k->c2 && p < fe; c++)
        p++;
      *e = p;
    }
  }
  if (*e < *b)
    *e = *b;
}

static int skipped(int ch, int flags) {
  if ((flags & F_DICT) && !isblank(ch) && !isalnum(ch))
    return 1;
  return (flags & F_PRINT) && !isprint(ch);
}

static int text_cmp(const char *a, const char *ae, const char *b, const char *be,
                    int flags) {
  int ca, cb;

  for (;;) {
    while (a < ae && skipped((unsigned char)*a, flags))
      a++;
    while (b < be && skipped((unsigned char)*b, flags))
      b++;
    if (a == ae || b == be)
      return (a < ae) - (b < be);
    ca = (unsigned char)*a++;
    cb = (unsigned char)*b++;
    if (flags & F_FOLD) {
      ca = toupper(ca);
      cb = toupper(cb);
    }
    if (ca != cb)
      return ca < cb ? -1 : 1;
  }
}

static void number(const char **p, const char *e, int *neg, const char **ip,
                   size_t *ilen, const char **fp, size_t *flen) {
  const char *s = *p;

  while (s < e && isblank((unsigned char)*s))
    s++;
  *neg = s < e && *s == '-';
  if (*neg)
    s++;
  while (s < e && *s == '0')
    s++;
  *ip = s;
  while (s < e && isdigit((unsigned char)*s))
    s++;
  *ilen = (size_t)(s - *ip);
  *fp = s;
  *flen = 0;
  if (s < e && *s == '.') {
    *fp = ++s;
    while (s < e && isdigit((unsigned char)*s))
      s++;
    *flen = (size_t)(s - *fp);
    while (*flen > 0 && (*fp)[*flen - 1] == '0')
      (*flen)--;
  }
  if (*ilen == 0 && *flen == 0)
    *neg = 0;
}

static int num_cmp(const char *a, const char *ae, const char *b, const char *be) {
  const char *ai, *af, *bi, *bf;
  size_t ail, afl, bil, bfl, i;
  int an, bn, r = 0;

  number(&a, ae, &an, &ai, &ail, &af, &afl);
  number(&b, be, &bn, &bi, &bil, &bf, &bfl);
  if (an != bn)
    return an ? -1 : 1;
  if (ail != bil)
    r = ail < bil ? -1 : 1;
  else if ((r = memcmp(ai, bi, ail)) == 0) {
    for (i = 0; r == 0 && (i < afl || i < bfl); i++) {
      int ca = i < afl ? af[i] : '0', cb = i < bfl ? bf[i] : '0';

      r = (ca > cb) - (ca < cb);
    }
  }
  r = (r > 0) - (r < 0);
  return an ? -r : r;
}

static int key_cmp(const struct line *x, const struct line *y) {
  const char *xb, *xe, *yb, *ye;
  int i, r;

  for (i = 0; i < nkeys; i++) {
    const struct key *k = &keys[i];

    bounds(x, k, &xb, &xe);
    bounds(y, k, &yb, &ye);
    r = (k->flags & F_NUMBER) ? num_cmp(xb, xe, yb, ye)
                              : text_cmp(xb, xe, yb, ye, k->flags);
    if (r != 0)
      return (k->flags & F_REVERSE) ? -r : r;
  }
  return 0;
}

static int line_cmp(const struct line *x, const struct line *y) {
  int r = key_cmp(x, y);
  size_t n;

  if (r != 0 || stable || unique)
    return r;
  n = x->len < y->len ? x->len : y->len;
  if ((r = memcmp(x->text, y->text, n)) == 0)
    r = (x->len > y->len) - (x->len < y->len);
  r = (r > 0) - (r < 0);
  return (global & F_REVERSE) ? -r : r;
}

static void msort(struct line *a, struct line *tmp, size_t n) {
  size_t h = n / 2, i = 0, j = h, o = 0;

  if (n < 2)
    return;
  msort(a, tmp, h);
  msort(a + h, tmp, n - h);
  while (i < h && j < n)
    tmp[o++] = line_cmp(&a[j], &a[i]) < 0 ? a[j++] : a[i++];
  while (i < h)
    tmp[o++] = a[i++];
  while (j < n)
    tmp[o++] = a[j++];
  memcpy(a, tmp, n * sizeof(*a));
}

static struct line *lines;
static size_t nlines, cap;

static void slurp(FILE *f, const char *name) {
  char *buf = 0;
  size_t size = 0;
  ssize_t n;

  while ((n = getline(&buf, &size, f)) > 0) {
    if (buf[n - 1] == '\n')
      n--;
    if (nlines == cap) {
      cap = cap ? cap * 2 : 1024;
      if ((lines = reallocarray(lines, cap, sizeof(*lines))) == 0)
        err(2, 0);
    }
    if ((lines[nlines].text = malloc((size_t)n + 1)) == 0)
      err(2, 0);
    memcpy(lines[nlines].text, buf, (size_t)n);
    lines[nlines].text[n] = 0;
    lines[nlines].len = (size_t)n;
    nlines++;
  }
  if (ferror(f))
    err(2, "%s", name);
  free(buf);
}

static int flag_of(int c) {
  switch (c) {
  case 'b': return F_BLANKS | F_ENDBLANKS;
  case 'd': return F_DICT;
  case 'f': return F_FOLD;
  case 'i': return F_PRINT;
  case 'n': return F_NUMBER;
  case 'r': return F_REVERSE;
  default: return 0;
  }
}

// -k f1[.c1][flags][,f2[.c2][flags]]
static void add_key(const char *s) {
  struct key k;
  char *end;
  int f;

  memset(&k, 0, sizeof(k));
  k.f1 = (int)strtol(s, &end, 10);
  k.c1 = 1;
  if (end == s || k.f1 < 1)
    errx(2, "invalid key: %s", s);
  if (*end == '.') {
    s = end + 1;
    k.c1 = (int)strtol(s, &end, 10);
    if (end == s || k.c1 < 1)
      errx(2, "invalid key: %s", s);
  }
  for (; (f = flag_of(*end)) != 0; end++)
    k.flags |= *end == 'b' ? F_BLANKS : f;
  if (*end == ',') {
    s = end + 1;
    k.f2 = (int)strtol(s, &end, 10);
    if (end == s || k.f2 < 1)
      errx(2, "invalid key: %s", s);
    if (*end == '.') {
      s = end + 1;
      k.c2 = (int)strtol(s, &end, 10);
      if (end == s || k.c2 < 0)
        errx(2, "invalid key: %s", s);
    }
    for (; (f = flag_of(*end)) != 0; end++)
      k.flags |= *end == 'b' ? F_ENDBLANKS : f;
  }
  if (*end != 0)
    errx(2, "invalid key: %s", s);
  if ((keys = reallocarray(keys, (size_t)nkeys + 1, sizeof(*keys))) == 0)
    err(2, 0);
  keys[nkeys++] = k;
}

int main(int argc, char **argv) {
  const char *output = 0;
  int ch, check = 0, quiet = 0, i;
  struct line *tmp;
  size_t n;
  FILE *f;

  if (pledge("stdio rpath wpath cpath", 0) < 0)
    err(2, "pledge");
  while ((ch = getopt(argc, argv, "bCcdfik:mno:rst:u")) != -1)
    switch (ch) {
    case 'C': check = quiet = 1; break;
    case 'c': check = 1; break;
    case 'k': add_key(optarg); break;
    case 'm': break;
    case 'o': output = optarg; break;
    case 's': stable = 1; break;
    case 't':
      if (optarg[0] == 0 || optarg[1] != 0)
        errx(2, "the separator is one character");
      tab = (unsigned char)optarg[0];
      break;
    case 'u': unique = 1; break;
    default:
      if (flag_of(ch) == 0)
        usage();
      global |= flag_of(ch);
    }
  argc -= optind;
  argv += optind;
  if (nkeys == 0)
    add_key("1");
  for (i = 0; i < nkeys; i++)
    if (keys[i].flags == 0)
      keys[i].flags = global;
  if (check && argc > 1)
    errx(2, "only one file may be checked");

  if (argc == 0)
    slurp(stdin, "stdin");
  for (i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-") == 0) {
      slurp(stdin, "stdin");
      continue;
    }
    if ((f = fopen(argv[i], "r")) == 0)
      err(2, "%s", argv[i]);
    slurp(f, argv[i]);
    fclose(f);
  }

  if (check) {
    for (n = 1; n < nlines; n++) {
      int r = line_cmp(&lines[n - 1], &lines[n]);

      if (r > 0 || (unique && r == 0)) {
        if (!quiet)
          fprintf(stderr, "sort: %s:%zu: disorder: %s\n",
                  argc ? argv[0] : "-", n + 1, lines[n].text);
        return 1;
      }
    }
    return 0;
  }

  if (nlines > 0) {
    if ((tmp = reallocarray(0, nlines, sizeof(*tmp))) == 0)
      err(2, 0);
    msort(lines, tmp, nlines);
    free(tmp);
  }
  if (output != 0 && freopen(output, "w", stdout) == 0)
    err(2, "%s", output);
  for (n = 0; n < nlines; n++) {
    if (unique && n > 0 && key_cmp(&lines[n - 1], &lines[n]) == 0)
      continue;
    fwrite(lines[n].text, 1, lines[n].len, stdout);
    putchar('\n');
  }
  if (fflush(stdout) == EOF || ferror(stdout))
    err(2, "%s", output ? output : "stdout");
  return 0;
}
