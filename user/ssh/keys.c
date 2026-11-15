#include "ssh.h"
#include <fcntl.h>
#include <sys/stat.h>

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int ssh_b64_encode(const unsigned char *in, uint32_t n, char *out, uint32_t max) {
  uint32_t i, o = 0;

  if (max < (n + 2) / 3 * 4 + 1)
    return -1;
  for (i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n)
      v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n)
      v |= in[i + 2];
    out[o++] = b64[(v >> 18) & 63];
    out[o++] = b64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? b64[v & 63] : '=';
  }
  out[o] = 0;
  return (int)o;
}

static int b64_value(int c) {
  const char *p = c ? strchr(b64, c) : 0;
  return p ? (int)(p - b64) : -1;
}

int ssh_b64_decode(const char *in, unsigned char *out, uint32_t max) {
  uint32_t v = 0, o = 0;
  int bits = 0, c;

  for (; *in; in++) {
    if (*in == '=')
      break;
    if ((c = b64_value(*in)) < 0)
      break;
    v = (v << 6) | (uint32_t)c;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= max)
        return -1;
      out[o++] = (unsigned char)(v >> bits);
    }
  }
  return (int)o;
}

void ssh_put_pubkey(struct sbuf *s, const unsigned char pub[32]) {
  sb_put_cstring(s, "ssh-ed25519");
  sb_put_string(s, pub, 32);
}

int ssh_get_pubkey(struct sbuf *s, unsigned char pub[32]) {
  char type[32];
  const unsigned char *p;
  uint32_t n;

  if (sb_get_cstring(s, type, sizeof(type)) < 0 || strcmp(type, "ssh-ed25519") != 0)
    return -1;
  if ((p = sb_get_string(s, &n)) == 0 || n != 32)
    return -1;
  memcpy(pub, p, 32);
  return 0;
}

void ssh_put_sig(struct sbuf *s, const unsigned char sig[64]) {
  sb_put_cstring(s, "ssh-ed25519");
  sb_put_string(s, sig, 64);
}

int ssh_get_sig(struct sbuf *s, unsigned char sig[64]) {
  char type[32];
  const unsigned char *p;
  uint32_t n;

  if (sb_get_cstring(s, type, sizeof(type)) < 0 || strcmp(type, "ssh-ed25519") != 0)
    return -1;
  if ((p = sb_get_string(s, &n)) == 0 || n != 64)
    return -1;
  memcpy(sig, p, 64);
  return 0;
}

int ssh_key_generate(unsigned char pub[32], unsigned char sec[64]) {
  return crypto_sign_ed25519_keypair(pub, sec);
}

// SHA256
int ssh_fingerprint(const unsigned char pub[32], char *out, uint32_t max) {
  struct sbuf blob;
  SHA2_CTX c;
  unsigned char d[32];
  char b[48];
  int n;

  sb_init(&blob);
  ssh_put_pubkey(&blob, pub);
  SHA256Init(&c);
  SHA256Update(&c, blob.b, blob.len);
  SHA256Final(d, &c);
  sb_free(&blob);
  n = ssh_b64_encode(d, 32, b, sizeof(b));
  while (n > 0 && b[n - 1] == '=')
    b[--n] = 0;
  return snprintf(out, max, "SHA256:%s", b);
}

static int write_all(const char *path, const void *p, uint32_t n, int mode) {
  int fd;

  unlink(path);
  if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
    return -1;
  if (ssh_writen(fd, p, n) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  chmod(path, mode);
  return 0;
}

// PROTOCOL
int ssh_key_write(const char *path, const unsigned char pub[32],
                  const unsigned char sec[64], const char *comment) {
  struct sbuf f, k, priv;
  char pubpath[256], *text = 0, line[256];
  unsigned char check[4];
  uint32_t i, n;
  int r = -1;

  sb_init(&f);
  sb_init(&k);
  sb_init(&priv);
  ssh_put_pubkey(&k, pub);

  arc4random_buf(check, 4);
  sb_put(&priv, check, 4);
  sb_put(&priv, check, 4);
  ssh_put_pubkey(&priv, pub);
  sb_put_string(&priv, sec, 64);
  sb_put_cstring(&priv, comment);
  for (i = 1; priv.len % 8 != 0; i++)
    sb_put_u8(&priv, (int)i);

  sb_put(&f, "openssh-key-v1", 15);
  sb_put_cstring(&f, "none");
  sb_put_cstring(&f, "none");
  sb_put_cstring(&f, "");
  sb_put_u32(&f, 1);
  sb_put_sbuf(&f, &k);
  sb_put_sbuf(&f, &priv);

  n = (f.len + 2) / 3 * 4 + 1;
  if ((text = malloc(n + n / 70 + 128)) == 0)
    goto out;
  {
    char *b = malloc(n);
    uint32_t o = 0, at = 0, len;

    if (b == 0)
      goto out;
    len = (uint32_t)ssh_b64_encode(f.b, f.len, b, n);
    o = (uint32_t)snprintf(text, 64, "-----BEGIN OPENSSH PRIVATE KEY-----\n");
    while (at < len) {
      uint32_t take = len - at > 70 ? 70 : len - at;
      memcpy(text + o, b + at, take);
      o += take;
      text[o++] = '\n';
      at += take;
    }
    o += (uint32_t)snprintf(text + o, 64, "-----END OPENSSH PRIVATE KEY-----\n");
    explicit_bzero(b, n);
    free(b);
    if (write_all(path, text, o, 0600) < 0)
      goto out;
    explicit_bzero(text, o);
  }

  snprintf(pubpath, sizeof(pubpath), "%s.pub", path);
  {
    char b[128];
    ssh_b64_encode(k.b, k.len, b, sizeof(b));
    n = (uint32_t)snprintf(line, sizeof(line), "ssh-ed25519 %s %s\n", b, comment);
  }
  r = write_all(pubpath, line, n, 0644);
out:
  if (text)
    free(text);
  sb_free(&f);
  sb_free(&k);
  sb_free(&priv);
  return r;
}

static int read_all(const char *path, char **out, uint32_t *len) {
  int fd, n;
  char *p;
  uint32_t cap = 8192, have = 0;

  if ((fd = open(path, O_RDONLY)) < 0)
    return -1;
  if ((p = malloc(cap)) == 0) {
    close(fd);
    return -1;
  }
  while ((n = (int)read(fd, p + have, cap - have - 1)) > 0) {
    have += (uint32_t)n;
    if (have >= cap - 1) {
      close(fd);
      free(p);
      return -1;
    }
  }
  close(fd);
  p[have] = 0;
  *out = p;
  *len = have;
  return 0;
}

int ssh_key_read(const char *path, unsigned char pub[32], unsigned char sec[64]) {
  char *text, *p, *q, *b;
  unsigned char *raw;
  struct sbuf f;
  uint32_t len, n, o = 0, pn;
  const unsigned char *s;
  int r = -1, rn;
  char kind[32];

  if (read_all(path, &text, &len) < 0)
    return -1;
  if ((p = strstr(text, "-----BEGIN OPENSSH PRIVATE KEY-----")) == 0 ||
      (q = strstr(text, "-----END OPENSSH PRIVATE KEY-----")) == 0) {
    explicit_bzero(text, len);
    free(text);
    return -1;
  }
  p += 35;
  if ((b = malloc(len)) == 0 || (raw = malloc(len)) == 0) {
    free(b);
    explicit_bzero(text, len);
    free(text);
    return -1;
  }
  for (; p < q; p++)
    if (*p != '\n' && *p != '\r' && *p != ' ')
      b[o++] = *p;
  b[o] = 0;
  if ((rn = ssh_b64_decode(b, raw, len)) < 0)
    goto out;
  sb_init(&f);
  f.b = raw;
  f.len = (uint32_t)rn;
  f.cap = len;
  if (f.len < 15 || memcmp(f.b, "openssh-key-v1", 15) != 0)
    goto out;
  f.rd = 15;
  if (sb_get_cstring(&f, kind, sizeof(kind)) < 0 || strcmp(kind, "none") != 0) {
    dprintf(2, "%s: only unencrypted keys are read here\n", path);
    goto out;
  }
  if (sb_get_cstring(&f, kind, sizeof(kind)) < 0 || sb_skip_string(&f) < 0)
    goto out;
  if (sb_get_u32(&f) != 1 || sb_skip_string(&f) < 0)
    goto out;
  if ((s = sb_get_string(&f, &pn)) == 0)
    goto out;
  {
    struct sbuf priv;
    sb_init(&priv);
    priv.b = (unsigned char *)s;
    priv.len = pn;
    if (sb_get_u32(&priv) != sb_get_u32(&priv))
      goto out;
    if (ssh_get_pubkey(&priv, pub) < 0)
      goto out;
    if ((s = sb_get_string(&priv, &n)) == 0 || n != 64)
      goto out;
    memcpy(sec, s, 64);
  }
  r = 0;
out:
  explicit_bzero(raw, len);
  explicit_bzero(b, len);
  explicit_bzero(text, len);
  free(raw);
  free(b);
  free(text);
  return r;
}

int ssh_pubkey_read(const char *path, unsigned char pub[32]) {
  char *text, *line;
  uint32_t len;
  int r = -1;

  if (read_all(path, &text, &len) < 0)
    return -1;
  for (line = text; line && *line; ) {
    char *next = strchr(line, '\n');
    unsigned char raw[128];
    struct sbuf b;
    int n;

    if (next)
      *next++ = 0;
    if (strncmp(line, "ssh-ed25519 ", 12) == 0 &&
        (n = ssh_b64_decode(line + 12, raw, sizeof(raw))) > 0) {
      sb_init(&b);
      b.b = raw;
      b.len = (uint32_t)n;
      if (ssh_get_pubkey(&b, pub) == 0) {
        r = 0;
        break;
      }
    }
    line = next;
  }
  free(text);
  return r;
}

// TERMINAL MODES

static const struct {
  int op, idx;
} ttychars[] = {{1, VINTR},   {2, VQUIT},   {3, VERASE},  {4, VKILL},
                {5, VEOF},    {6, VEOL},    {7, VEOL2},   {8, VSTART},
                {9, VSTOP},   {10, VSUSP},  {12, VREPRINT}, {13, VWERASE},
                {14, VLNEXT}, {18, VDISCARD}, {0, 0}};

static const struct {
  int op, which;
  uint32_t bit;
} ttyflags[] = {{33, 0, ISTRIP},  {34, 0, INLCR},  {35, 0, IGNCR},
                {36, 0, ICRNL},   {38, 0, IXON},   {39, 0, IXANY},
                {41, 0, IMAXBEL}, {50, 3, ISIG},   {51, 3, ICANON},
                {53, 3, ECHO},    {54, 3, ECHOE},  {55, 3, ECHOK},
                {56, 3, ECHONL},  {57, 3, NOFLSH}, {58, 3, TOSTOP},
                {59, 3, IEXTEN},  {60, 3, ECHOCTL}, {61, 3, ECHOKE},
                {70, 1, OPOST},   {72, 1, ONLCR},  {73, 1, OCRNL},
                {75, 1, ONLRET},  {0, 0, 0}};

static uint32_t *flag_word(struct termios *t, int which) {
  switch (which) {
  case 0: return &t->c_iflag;
  case 1: return &t->c_oflag;
  case 2: return &t->c_cflag;
  default: return &t->c_lflag;
  }
}

void ssh_put_ttymodes(struct sbuf *s, const struct termios *t) {
  struct sbuf m;
  int i;

  sb_init(&m);
  for (i = 0; ttychars[i].op; i++) {
    sb_put_u8(&m, ttychars[i].op);
    sb_put_u32(&m, t->c_cc[ttychars[i].idx]);
  }
  for (i = 0; ttyflags[i].op; i++) {
    sb_put_u8(&m, ttyflags[i].op);
    sb_put_u32(&m, (*flag_word((struct termios *)t, ttyflags[i].which) &
                    ttyflags[i].bit) != 0);
  }
  sb_put_u8(&m, 128);
  sb_put_u32(&m, t->c_ispeed != 0 ? t->c_ispeed : TTYDEF_SPEED);
  sb_put_u8(&m, 129);
  sb_put_u32(&m, t->c_ospeed != 0 ? t->c_ospeed : TTYDEF_SPEED);
  sb_put_u8(&m, 0);
  sb_put_sbuf(s, &m);
  sb_free(&m);
}

void ssh_get_ttymodes(const unsigned char *p, uint32_t n, struct termios *t) {
  struct sbuf m;
  int op, i;
  uint32_t v;

  sb_init(&m);
  m.b = (unsigned char *)p;
  m.len = n;
  while ((op = sb_get_u8(&m)) > 0 && op < 160 && !m.err) {
    v = sb_get_u32(&m);
    if (m.err)
      break;
    if (op == 128)
      t->c_ispeed = v;
    else if (op == 129)
      t->c_ospeed = v;
    for (i = 0; ttychars[i].op; i++)
      if (ttychars[i].op == op)
        t->c_cc[ttychars[i].idx] = (uchar)(v > 0xff ? VDISABLE : v);
    for (i = 0; ttyflags[i].op; i++)
      if (ttyflags[i].op == op) {
        uint32_t *w = flag_word(t, ttyflags[i].which);
        if (v)
          *w |= ttyflags[i].bit;
        else
          *w &= ~ttyflags[i].bit;
      }
  }
}

void ssh_user_path(char *out, uint32_t max, const char *name) {
  const char *home = getenv("HOME");

  if (home == 0 || home[0] == 0)
    home = "/";
  snprintf(out, max, "%s%s.ssh/%s", home, home[strlen(home) - 1] == '/' ? "" : "/", name);
}
