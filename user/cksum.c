// CKSUM
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sha2.h>
#include <err.h>

extern char *__progname;

#define BUFSIZE (64 * 1024)

struct hash {
  const char *name;  // as -a takes it, and as a program
  const char *tag;   // as a line names it
  int digest;        // bytes
  void (*init)(SHA2_CTX *);
  void (*update)(SHA2_CTX *, const uint8_t *, size_t);
  char *(*end)(SHA2_CTX *, char *);
};

static const struct hash hashes[] = {
    {"cksum", "CKSUM", 0, 0, 0, 0},
    {"sha224", "SHA224", SHA224_DIGEST_LENGTH, SHA224Init, SHA224Update, SHA224End},
    {"sha256", "SHA256", SHA256_DIGEST_LENGTH, SHA256Init, SHA256Update, SHA256End},
    {"sha384", "SHA384", SHA384_DIGEST_LENGTH, SHA384Init, SHA384Update, SHA384End},
    {"sha512", "SHA512", SHA512_DIGEST_LENGTH, SHA512Init, SHA512Update, SHA512End},
    {"sha512/256", "SHA512/256", SHA512_256_DIGEST_LENGTH, SHA512_256Init,
     SHA512_256Update, SHA512_256End},
};
#define NHASH (sizeof(hashes) / sizeof(hashes[0]))

static uint32_t crctab[256];
static int quiet, reverse;
static uint8_t *buf;

static void usage(void) {
  if (strcmp(__progname, "cksum") == 0)
    fprintf(stderr, "usage: cksum [-qr] [-a algorithms] [-c] [-s string] [file ...]\n");
  else
    fprintf(stderr, "usage: %s [-qr] [-c] [-s string] [file ...]\n", __progname);
  exit(1);
}

static const struct hash *lookup(const char *name) {
  size_t i;

  for (i = 0; i < NHASH; i++)
    if (strcasecmp(hashes[i].name, name) == 0 || strcasecmp(hashes[i].tag, name) == 0)
      return &hashes[i];
  return 0;
}

// POSIX cksum: CRC-32 0x04c11db7, msb first, then the length
static void crc_init(void) {
  uint32_t c;
  int i, k;

  for (i = 0; i < 256; i++) {
    c = (uint32_t)i << 24;
    for (k = 0; k < 8; k++)
      c = (c & 0x80000000) ? (c << 1) ^ 0x04c11db7 : c << 1;
    crctab[i] = c;
  }
}

static uint32_t crc_add(uint32_t crc, const uint8_t *p, size_t n) {
  while (n-- > 0)
    crc = (crc << 8) ^ crctab[((crc >> 24) ^ *p++) & 0xff];
  return crc;
}

static uint32_t crc_end(uint32_t crc, uint64_t len) {
  uint8_t b;

  for (; len != 0; len >>= 8) {
    b = (uint8_t)len;
    crc = crc_add(crc, &b, 1);
  }
  return ~crc;
}

struct sum {
  SHA2_CTX ctx;
  uint32_t crc;
  uint64_t len;
  char text[2 * SHA512_DIGEST_LENGTH + 1];
};

static void sum_start(const struct hash *h, struct sum *s) {
  s->crc = 0;
  s->len = 0;
  if (h->init != 0)
    h->init(&s->ctx);
}

static void sum_add(const struct hash *h, struct sum *s, const uint8_t *p, size_t n) {
  s->len += n;
  if (h->update != 0)
    h->update(&s->ctx, p, n);
  else
    s->crc = crc_add(s->crc, p, n);
}

static void sum_end(const struct hash *h, struct sum *s) {
  if (h->end != 0)
    h->end(&s->ctx, s->text);
  else
    snprintf(s->text, sizeof(s->text), "%u %llu", crc_end(s->crc, s->len),
             (unsigned long long)s->len);
}

// every algorithm over one pass of the data
static int sum_fd(int fd, const struct hash **hs, int n, struct sum *sums) {
  ssize_t got;
  int i;

  for (i = 0; i < n; i++)
    sum_start(hs[i], &sums[i]);
  while ((got = read(fd, buf, BUFSIZE)) > 0)
    for (i = 0; i < n; i++)
      sum_add(hs[i], &sums[i], buf, (size_t)got);
  if (got < 0)
    return -1;
  for (i = 0; i < n; i++)
    sum_end(hs[i], &sums[i]);
  return 0;
}

static void print(const struct hash *h, struct sum *s, const char *name) {
  if (h->end == 0) {
    if (name != 0)
      printf("%s %s\n", s->text, name);
    else
      printf("%s\n", s->text);
  } else if (quiet || name == 0)
    printf("%s\n", s->text);
  else if (reverse)
    printf("%s %s\n", s->text, name);
  else
    printf("%s (%s) = %s\n", h->tag, name, s->text);
}

static int sum_files(const struct hash **hs, int n, char **files, int nfiles) {
  struct sum *sums = calloc((size_t)n, sizeof(*sums));
  int i, k, fd, bad = 0;

  if (sums == 0)
    err(1, 0);
  if (nfiles == 0) {
    if (sum_fd(0, hs, n, sums) < 0)
      err(1, "stdin");
    for (k = 0; k < n; k++)
      print(hs[k], &sums[k], 0);
  }
  for (i = 0; i < nfiles; i++) {
    if ((fd = open(files[i], O_RDONLY)) < 0) {
      warn("%s", files[i]);
      bad = 1;
      continue;
    }
    if (sum_fd(fd, hs, n, sums) < 0) {
      warn("%s", files[i]);
      bad = 1;
    } else
      for (k = 0; k < n; k++)
        print(hs[k], &sums[k], files[i]);
    close(fd);
  }
  free(sums);
  return bad;
}

// "TAG (name) = digest", or "digest name" with one algorithm
static int parse_line(char *line, const struct hash *only, const struct hash **h,
                      char **name, char **want) {
  char *p, *q;

  line[strcspn(line, "\r\n")] = 0;
  if ((p = strstr(line, " (")) != 0 && (q = strstr(p, ") = ")) != 0) {
    *p = 0;
    *q = 0;
    if ((*h = lookup(line)) == 0)
      return -1;
    *name = p + 2;
    *want = q + 4;
    return 0;
  }
  if (only == 0 || (p = strpbrk(line, " \t")) == 0)
    return -1;
  *p++ = 0;
  p += strspn(p, " \t*");
  *h = only;
  *want = line;
  *name = p;
  return *p != 0 ? 0 : -1;
}

static int check(const struct hash *only, char **lists, int nlists) {
  char line[4096], *name, *want;
  const struct hash *h;
  struct sum s;
  FILE *fp;
  int i, fd, bad = 0, seen = 0;

  for (i = 0; i < (nlists ? nlists : 1); i++) {
    if (nlists == 0)
      fp = stdin;
    else if ((fp = fopen(lists[i], "r")) == 0) {
      warn("%s", lists[i]);
      bad = 1;
      continue;
    }
    while (fgets(line, sizeof(line), fp) != 0) {
      if (parse_line(line, only, &h, &name, &want) < 0 || (only != 0 && h != only) ||
          h->end == 0)
        continue;
      seen++;
      if ((fd = open(name, O_RDONLY)) < 0) {
        printf("(%s) %s: %s\n", h->tag, name, strerror(errno));
        bad = 1;
        continue;
      }
      if (sum_fd(fd, &h, 1, &s) < 0) {
        printf("(%s) %s: %s\n", h->tag, name, strerror(errno));
        bad = 1;
      } else if (strcasecmp(s.text, want) == 0)
        printf("(%s) %s: OK\n", h->tag, name);
      else {
        printf("(%s) %s: FAILED\n", h->tag, name);
        bad = 1;
      }
      close(fd);
    }
    if (fp != stdin)
      fclose(fp);
  }
  if (seen == 0) {
    warnx("no lines to check");
    bad = 1;
  }
  return bad;
}

int main(int argc, char **argv) {
  const struct hash *hs[NHASH], *h;
  char *list = 0, *one, *string = 0;
  struct sum s;
  int ch, n = 0, checking = 0, named = 0, i;

  crc_init();
  if (strcmp(__progname, "cksum") != 0) {
    if ((h = lookup(__progname)) == 0)
      errx(1, "%s: not an algorithm", __progname);
    hs[n++] = h;
    named = 1;
  }
  while ((ch = getopt(argc, argv, "a:cqrs:")) != -1)
    switch (ch) {
    case 'a':
      if (strcmp(__progname, "cksum") != 0)
        usage();
      list = optarg;
      named = 1;
      break;
    case 'c':
      checking = 1;
      break;
    case 'q':
      quiet = 1;
      break;
    case 'r':
      reverse = 1;
      break;
    case 's':
      string = optarg;
      break;
    default:
      usage();
    }
  argc -= optind;
  argv += optind;
  for (; list != 0 && (one = strsep(&list, ",")) != 0;) {
    if (*one == 0)
      continue;
    if ((h = lookup(one)) == 0)
      errx(1, "%s: no such algorithm", one);
    for (i = 0; i < n && hs[i] != h; i++)
      ;
    if (i == n)
      hs[n++] = h;
  }
  if (n == 0)
    hs[n++] = &hashes[0];
  if (checking) {
    if (string != 0 || n > 1)
      usage();
    if ((buf = malloc(BUFSIZE)) == 0)
      err(1, 0);
    return check(named ? hs[0] : 0, argv, argc);
  }
  if (string != 0) {
    if (argc != 0)
      usage();
    for (i = 0; i < n; i++) {
      sum_start(hs[i], &s);
      sum_add(hs[i], &s, (const uint8_t *)string, strlen(string));
      sum_end(hs[i], &s);
      if (hs[i]->end == 0 || quiet)
        printf("%s\n", s.text);
      else if (reverse)
        printf("%s \"%s\"\n", s.text, string);
      else
        printf("%s (\"%s\") = %s\n", hs[i]->tag, string, s.text);
    }
    return 0;
  }
  if ((buf = malloc(BUFSIZE)) == 0)
    err(1, 0);
  return sum_files(hs, n, argv, argc);
}
