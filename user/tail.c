#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>

#define BLOCK 65536

enum { FROM_END, FROM_START, REVERSE };

struct input {
  const char *name;
  int fd;
  int atwill;
  int byname;
  char *mem;
  int64_t size;
  uint64_t device, ino;
  int follow;
};

static int64_t count = 10;
static int style = FROM_END, bytes, given, follow, reverse, status;
static struct input *inputs, *shown;
static int ninputs;
static char obuf[BLOCK];
static int olen;

static void flush(void) {
  struct fnu_segment seg;
  long r;
  int done = 0;

  while (done < olen) {
    seg.base = obuf + done;
    seg.len = (uint64_t)(olen - done);
    if ((r = fnu_write(1, &seg, 1, FNU_IO_HERE)) == -FNU_ERR_INTERRUPTED)
      continue;
    if (r < 0)
      errx(1, "stdout: %s", fnu_errstr((int)r));
    done += (int)r;
  }
  olen = 0;
}

static void put(const char *p, int64_t n) {
  int64_t k;

  while (n > 0) {
    if (olen == BLOCK)
      flush();
    k = BLOCK - olen < n ? BLOCK - olen : n;
    memcpy(obuf + olen, p, (size_t)k);
    olen += (int)k;
    p += k;
    n -= k;
  }
}

static void header(struct input *in) {
  if (ninputs > 1 && shown != in) {
    put(shown != 0 ? "\n==> " : "==> ", shown != 0 ? 5 : 4);
    put(in->name, (int64_t)strlen(in->name));
    put(" <==\n", 5);
  }
  shown = in;
}

static long readsome(struct input *in, char *buf, long len, int64_t at) {
  struct fnu_segment seg = {buf, (uint64_t)len};
  long r;

  while ((r = fnu_read(in->fd, &seg, 1, at)) == -FNU_ERR_INTERRUPTED)
    ;
  if (r < 0) {
    warnx("%s: %s", in->name, fnu_errstr((int)r));
    status = 1;
  }
  return r;
}

static const char *view(struct input *in, int64_t at, long len, char *buf) {
  long r, got = 0;

  if (!in->atwill)
    return in->mem + at;
  while (got < len && (r = readsome(in, buf + got, len - got, at + got)) > 0)
    got += r;
  if (got < len)
    memset(buf + got, 0, (size_t)(len - got));
  return buf;
}

static int64_t lastpart(struct input *in, int64_t end, char *buf) {
  int64_t pos = end, from, n = 0;
  const char *p;
  long len, i;

  if (bytes)
    return end > count ? end - count : 0;
  if (count == 0)
    return end;
  while (pos > 0) {
    len = pos > BLOCK ? BLOCK : (long)pos;
    from = pos - len;
    p = view(in, from, len, buf);
    for (i = len - 1; i >= 0; i--)
      if (p[i] == '\n' && from + i != end - 1 && ++n == count)
        return from + i + 1;
    pos = from;
  }
  return 0;
}

static int keep(struct input *in, char *buf) {
  int64_t cap = 0, trimmed = 0, cut;
  char *grown;
  long r;

  in->size = 0;
  for (;;) {
    if (cap - in->size < BLOCK) {
      cap = cap == 0 ? 4 * BLOCK : cap * 2;
      if ((grown = realloc(in->mem, (size_t)cap)) == 0)
        errx(1, "%s: out of memory", in->name);
      in->mem = grown;
    }
    if ((r = readsome(in, in->mem + in->size, BLOCK, FNU_IO_HERE)) <= 0)
      return r < 0 ? -1 : 0;
    in->size += r;
    if ((style == REVERSE && !given) || in->size < 2 * trimmed + 16 * BLOCK)
      continue;
    if ((cut = lastpart(in, in->size, buf)) > in->size / 2) {
      memmove(in->mem, in->mem + cut, (size_t)(in->size - cut));
      in->size -= cut;
    }
    trimmed = in->size;
  }
}

static void copyfrom(struct input *in, int64_t at, char *buf) {
  long len;

  for (; at < in->size; at += len) {
    len = in->size - at > BLOCK ? BLOCK : (long)(in->size - at);
    put(view(in, at, len, buf), len);
  }
}

static void backwards(struct input *in, int64_t from, int64_t end, char *buf) {
  char *carry = 0, *grown;
  int64_t ccap = 0, clen = 0, pos = end, lines = 0, at;
  const char *p;
  long len, i, stop;

  if (given && !bytes && count == 0)
    return;
  while (pos > from) {
    len = pos - from > BLOCK ? BLOCK : (long)(pos - from);
    at = pos - len;
    p = view(in, at, len, buf);
    stop = len;
    for (i = len - 1; i >= 0; i--) {
      if (p[i] != '\n')
        continue;
      if (at + i != end - 1) {
        put(p + i + 1, stop - i - 1);
        if (clen > 0)
          put(carry + ccap - clen, clen);
        put("\n", 1);
        clen = 0;
        if (given && !bytes && ++lines == count)
          goto out;
      }
      stop = i;
    }
    if (clen + stop > ccap) {
      int64_t want = ccap == 0 ? BLOCK : ccap;

      while (want < clen + stop)
        want *= 2;
      if ((grown = malloc((size_t)want)) == 0)
        errx(1, "%s: out of memory", in->name);
      if (clen > 0)
        memcpy(grown + want - clen, carry + ccap - clen, (size_t)clen);
      free(carry);
      carry = grown;
      ccap = want;
    }
    memcpy(carry + ccap - clen - stop, p, (size_t)stop);
    clen += stop;
    pos = at;
  }
  if (from < end) {
    if (clen > 0)
      put(carry + ccap - clen, clen);
    put("\n", 1);
  }
out:
  free(carry);
}

static void skipping(struct input *in, char *buf) {
  int64_t at = 0, n = 0;
  long r, i, s;

  if (in->atwill && bytes) {
    copyfrom(in, count, buf);
    return;
  }
  while ((r = readsome(in, buf, BLOCK, in->atwill ? at : FNU_IO_HERE)) > 0) {
    at += r;
    s = 0;
    if (bytes && n < count) {
      s = count - n < r ? (long)(count - n) : r;
      n += s;
    }
    for (i = 0; !bytes && n < count && i < r; i++)
      if (buf[i] == '\n' && ++n == count)
        s = i + 1;
    if (n >= count)
      put(buf + s, r - s);
  }
}

static void show(struct input *in, char *buf) {
  struct fnu_stat st;

  if (fnu_getattr(in->fd, 0, 0, &st) < 0)
    memset(&st, 0, sizeof(st));
  in->atwill = st.type == FNU_TYPE_FILE;
  in->device = st.device;
  in->ino = st.ino;
  in->size = in->atwill ? (int64_t)st.size : 0;
  header(in);
  if (style == FROM_START) {
    skipping(in, buf);
    if (in->atwill)
      fnu_seek(in->fd, 0, FNU_SEEK_TAIL);
    return;
  }
  if (!in->atwill && keep(in, buf) < 0) {
    free(in->mem);
    in->mem = 0;
    return;
  }
  if (style == REVERSE)
    backwards(in, given && bytes ? lastpart(in, in->size, buf) : 0, in->size, buf);
  else
    copyfrom(in, lastpart(in, in->size, buf), buf);
  if (in->atwill)
    fnu_seek(in->fd, in->size, FNU_SEEK_START);
  free(in->mem);
  in->mem = 0;
}

static void drain(struct input *in, char *buf) {
  struct fnu_stat st;
  int64_t here;
  long r;

  if (in->atwill && fnu_getattr(in->fd, 0, 0, &st) == 0 &&
      (here = fnu_seek(in->fd, 0, FNU_SEEK_HERE)) > (int64_t)st.size) {
    warnx("%s has been truncated, resetting.", in->name);
    fnu_seek(in->fd, 0, FNU_SEEK_START);
  }
  while ((r = readsome(in, buf, BLOCK, FNU_IO_HERE)) > 0) {
    header(in);
    put(buf, r);
  }
}

static void watch(int eq, struct input *in, int on) {
  struct fnu_event ev[2];
  int n = 1;
  uint16_t flags = on ? FNU_EV_ADD | FNU_EV_ENABLE | FNU_EV_CLEAR : FNU_EV_DELETE;

  memset(ev, 0, sizeof(ev));
  ev[0].ident = (uint64_t)in->fd;
  ev[0].filter = FNU_EVF_READ;
  ev[0].flags = flags;
  ev[0].udata = (uint64_t)(in - inputs);
  if (in->atwill) {
    ev[1] = ev[0];
    ev[1].filter = FNU_EVF_FILE;
    ev[1].fflags = FNU_NOTE_DELETE | FNU_NOTE_RENAME | FNU_NOTE_TRUNCATE;
    n = 2;
  }
  fnu_events(eq, ev, n, 0, 0, 0);
}

static void reopen(int eq, struct input *in, char *buf) {
  struct fnu_stat st;
  int fd;

  if (fnu_getattr(FNU_FD_CWD, in->name, 0, &st) < 0 ||
      (in->fd >= 0 && st.device == in->device && st.ino == in->ino))
    return;
  if ((fd = fnu_open(FNU_FD_CWD, in->name, FNU_OPEN_READ, 0)) < 0)
    return;
  if (in->fd >= 0) {
    drain(in, buf);
    fnu_close(in->fd);
    warnx("%s has been replaced, reopening.", in->name);
  }
  in->fd = fd;
  in->atwill = st.type == FNU_TYPE_FILE;
  in->device = st.device;
  in->ino = st.ino;
  watch(eq, in, 1);
  drain(in, buf);
}

static void forward(char *buf) {
  struct fnu_event ev[8];
  struct fnu_timespec second = {.sec = 1};
  struct input *in;
  int eq, n, i, named = 0;

  if ((eq = fnu_eventq(0)) < 0)
    errx(1, "events: %s", fnu_errstr(eq));
  for (i = 0; i < ninputs; i++) {
    if (inputs[i].follow && inputs[i].fd >= 0)
      watch(eq, &inputs[i], 1);
    named |= inputs[i].follow && inputs[i].byname;
  }
  for (;;) {
    flush();
    n = fnu_events(eq, 0, 0, ev, 8, named ? &second : 0);
    if (n == -FNU_ERR_INTERRUPTED)
      continue;
    if (n < 0)
      errx(1, "events: %s", fnu_errstr(n));
    for (i = 0; i < n; i++) {
      in = &inputs[ev[i].udata];
      if (in->fd < 0 || ev[i].ident != (uint64_t)in->fd)
        continue;
      if (ev[i].filter == FNU_EVF_FILE && (ev[i].fflags & FNU_NOTE_TRUNCATE)) {
        warnx("%s has been truncated, resetting.", in->name);
        fnu_seek(in->fd, 0, FNU_SEEK_START);
      }
      drain(in, buf);
      if (ev[i].filter == FNU_EVF_READ && (ev[i].flags & FNU_EV_EOF) && !in->atwill)
        watch(eq, in, 0);
    }
    for (i = 0; i < ninputs; i++)
      if (inputs[i].follow && inputs[i].byname)
        reopen(eq, &inputs[i], buf);
  }
}

static void usage(void) {
  fputs("usage: tail [-f | -r] [-b number | -c number | -n number | -number]"
        " [file ...]\n", stderr);
  exit(1);
}

static void number(const char *s, int units) {
  char *end;
  int plus = *s == '+';

  if (given)
    usage();
  count = strtoll(s, &end, 10);
  if (*s == 0 || *end != 0 || end == s + (plus || *s == '-'))
    errx(1, "illegal offset -- %s", s);
  if (count < 0)
    count = -count;
  count *= units;
  style = plus ? FROM_START : FROM_END;
  if (plus && count > 0)
    count -= units;
  given = 1;
}

static int old(char *a) {
  char *e = a + 1;
  int units = 1;

  if ((a[0] != '-' && a[0] != '+') || a[1] < '0' || a[1] > '9')
    return 0;
  while (*e >= '0' && *e <= '9')
    e++;
  if (*e == 'b' || *e == 'c' || *e == 'l') {
    units = *e == 'b' ? 512 : 1;
    bytes = *e != 'l';
    *e++ = 0;
  }
  for (; *e != 0; *e++ = 0)
    if (*e == 'f')
      follow = 1;
    else if (*e == 'r')
      reverse = 1;
    else
      errx(1, "illegal option -- %s", a);
  number(a, units);
  return 1;
}

int main(int argc, char **argv) {
  static char buf[BLOCK];
  struct input *in;
  char *a, *v;
  int i;

  if (fnu_pledge("stdio rpath", 0) < 0)
    errx(1, "pledge");
  for (i = 1; i < argc && (a = argv[i])[0] == '-' && a[1] != 0; i++) {
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (old(a))
      continue;
    for (a++; *a != 0; a++) {
      if (*a == 'f' || *a == 'r') {
        *(*a == 'f' ? &follow : &reverse) = 1;
        continue;
      }
      if (*a != 'n' && *a != 'c' && *a != 'b')
        usage();
      if ((v = a[1] != 0 ? a + 1 : argv[++i]) == 0)
        usage();
      bytes = *a != 'n';
      number(v, *a == 'b' ? 512 : 1);
      break;
    }
  }
  if (i < argc && argv[i][0] == '+' && !given && old(argv[i]))
    i++;
  if (reverse) {
    if (follow)
      usage();
    style = REVERSE;
  }
  ninputs = argc - i > 0 ? argc - i : 1;
  if ((inputs = calloc((size_t)ninputs, sizeof(*inputs))) == 0)
    errx(1, "out of memory");
  if (i == argc) {
    struct fnu_stat st;

    inputs[0].name = "stdin";
    inputs[0].follow = follow && fnu_getattr(0, 0, 0, &st) == 0 &&
                       st.type != FNU_TYPE_FIFO && st.type != FNU_TYPE_SOCKET;
    show(&inputs[0], buf);
  } else
    for (in = inputs; i < argc; i++, in++) {
      in->name = argv[i];
      in->byname = 1;
      in->follow = follow;
      if ((in->fd = fnu_open(FNU_FD_CWD, in->name, FNU_OPEN_READ, 0)) < 0) {
        warnx("%s: %s", in->name, fnu_errstr(in->fd));
        status = 1;
        continue;
      }
      show(in, buf);
    }
  for (in = inputs; in < inputs + ninputs && !in->follow; in++)
    ;
  if (in < inputs + ninputs)
    forward(buf);
  flush();
  return status;
}
