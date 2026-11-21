// strings — print printable sequences from files (common POSIX tool)
// options: -a (scan whole file), -n len (min length, default 4), -t o|d|x
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int minlen = 4;
static int whole = 0;
static int offset_fmt = 0; // 0 none, 'd' decimal, 'o' octal, 'x' hex

static void usage(void) {
  fprintf(stderr, "usage: strings [-a] [-n len] [-t o|d|x] [file...]\n");
  exit(2);
}

static void emit(unsigned long off, const char *s, size_t n) {
  if ((int)n < minlen)
    return;
  switch (offset_fmt) {
  case 'd':
    printf("%lu ", off);
    break;
  case 'o':
    printf("%lo ", off);
    break;
  case 'x':
    printf("%lx ", off);
    break;
  }
  fwrite(s, 1, n, stdout);
  fputc('\n', stdout);
}

static int scan(FILE *f, const char *name) {
  char buf[4096];
  char run[4096];
  size_t rn = 0;
  unsigned long off = 0;
  size_t n;

  (void)name;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    size_t i;
    for (i = 0; i < n; i++) {
      unsigned char c = (unsigned char)buf[i];
      if (isprint(c) || c == '\t') {
        if (rn < sizeof(run) - 1)
          run[rn++] = (char)c;
        else {
          emit(off - rn + 1, run, rn);
          rn = 0;
        }
      } else {
        if (rn) {
          emit(off - rn, run, rn);
          rn = 0;
        }
      }
      off++;
    }
  }
  if (rn)
    emit(off - rn, run, rn);
  return ferror(f) ? 1 : 0;
}

int main(int argc, char **argv) {
  int i, rc = 0, first_file = 1;

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "-a") == 0) {
        whole = 1;
        continue;
      }
      if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
        minlen = atoi(argv[++i]);
        if (minlen < 1)
          minlen = 1;
        continue;
      }
      if (strncmp(argv[i], "-n", 2) == 0 && argv[i][2]) {
        minlen = atoi(argv[i] + 2);
        if (minlen < 1)
          minlen = 1;
        continue;
      }
      if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
        offset_fmt = argv[++i][0];
        if (!strchr("dox", offset_fmt))
          usage();
        continue;
      }
      usage();
    }
    break;
  }
  (void)whole; // FNU files have no seek-skip; always scan fully
  if (i >= argc)
    return scan(stdin, "<stdin>") ? 1 : 0;

  for (; i < argc; i++) {
    FILE *f;
    first_file = 0;
    if (strcmp(argv[i], "-") == 0)
      f = stdin;
    else {
      f = fopen(argv[i], "rb");
      if (!f) {
        perror(argv[i]);
        rc = 1;
        continue;
      }
    }
    if (scan(f, argv[i]))
      rc = 1;
    if (f != stdin)
      fclose(f);
  }
  (void)first_file;
  return rc;
}
