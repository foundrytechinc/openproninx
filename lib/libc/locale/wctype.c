// C locale: below 128 is itself, nothing else has a class
#include <ctype.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

enum { W_ALNUM = 1, W_ALPHA, W_BLANK, W_CNTRL, W_DIGIT, W_GRAPH, W_LOWER,
       W_PRINT, W_PUNCT, W_SPACE, W_UPPER, W_XDIGIT };
enum { T_LOWER = 1, T_UPPER };

static const char *const names[] = {"",      "alnum", "alpha", "blank",
                                    "cntrl", "digit", "graph", "lower",
                                    "print", "punct", "space", "upper",
                                    "xdigit"};

wctype_t wctype(const char *name) {
  int i;

  for (i = 1; i <= W_XDIGIT; i++)
    if (strcmp(name, names[i]) == 0)
      return (wctype_t)i;
  return 0;
}

int iswctype(wint_t wc, wctype_t t) {
  int c;

  if (wc == WEOF || wc < 0 || wc > 127)
    return 0;
  c = (int)wc;
  switch (t) {
  case W_ALNUM: return isalnum(c);
  case W_ALPHA: return isalpha(c);
  case W_BLANK: return isblank(c);
  case W_CNTRL: return iscntrl(c);
  case W_DIGIT: return isdigit(c);
  case W_GRAPH: return isgraph(c);
  case W_LOWER: return islower(c);
  case W_PRINT: return isprint(c);
  case W_PUNCT: return ispunct(c);
  case W_SPACE: return isspace(c);
  case W_UPPER: return isupper(c);
  case W_XDIGIT: return isxdigit(c);
  default: return 0;
  }
}

int iswalnum(wint_t wc) { return iswctype(wc, W_ALNUM); }
int iswalpha(wint_t wc) { return iswctype(wc, W_ALPHA); }
int iswblank(wint_t wc) { return iswctype(wc, W_BLANK); }
int iswcntrl(wint_t wc) { return iswctype(wc, W_CNTRL); }
int iswdigit(wint_t wc) { return iswctype(wc, W_DIGIT); }
int iswgraph(wint_t wc) { return iswctype(wc, W_GRAPH); }
int iswlower(wint_t wc) { return iswctype(wc, W_LOWER); }
int iswprint(wint_t wc) { return iswctype(wc, W_PRINT); }
int iswpunct(wint_t wc) { return iswctype(wc, W_PUNCT); }
int iswspace(wint_t wc) { return iswctype(wc, W_SPACE); }
int iswupper(wint_t wc) { return iswctype(wc, W_UPPER); }
int iswxdigit(wint_t wc) { return iswctype(wc, W_XDIGIT); }

wint_t towlower(wint_t wc) {
  return wc >= 0 && wc <= 127 ? (wint_t)tolower((int)wc) : wc;
}

wint_t towupper(wint_t wc) {
  return wc >= 0 && wc <= 127 ? (wint_t)toupper((int)wc) : wc;
}

wctrans_t wctrans(const char *name) {
  if (strcmp(name, "tolower") == 0)
    return T_LOWER;
  if (strcmp(name, "toupper") == 0)
    return T_UPPER;
  return 0;
}

wint_t towctrans(wint_t wc, wctrans_t t) {
  return t == T_LOWER ? towlower(wc) : t == T_UPPER ? towupper(wc) : wc;
}

// columns: none for nul, -1 for unprintable
int wcwidth(wchar_t wc) {
  if (wc == 0)
    return 0;
  return iswprint((wint_t)wc) ? 1 : -1;
}

int wcswidth(const wchar_t *s, size_t n) {
  int w, total = 0;

  for (; n > 0 && *s != 0; s++, n--) {
    if ((w = wcwidth(*s)) < 0)
      return -1;
    total += w;
  }
  return total;
}
