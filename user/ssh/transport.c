#include "ssh.h"
#include "chacha_x4.h"

#define KEX_ALGS_C "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com"
#define KEX_ALGS_S "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com"
#define HOSTKEY_ALGS "ssh-ed25519"
#define CIPHER_ALGS "chacha20-poly1305@openssh.com"
#define MAC_ALGS "hmac-sha2-256"
#define COMP_ALGS "none"

// BUFFERS

static int memchr_nul(const unsigned char *p, uint32_t n);

void sb_init(struct sbuf *s) { memset(s, 0, sizeof(*s)); }

void sb_free(struct sbuf *s) {
  if (s->b != 0) {
    explicit_bzero(s->b, s->cap);
    free(s->b);
  }
  sb_init(s);
}

void sb_reset(struct sbuf *s) { s->len = s->rd = 0; s->err = 0; }

int sb_reserve(struct sbuf *s, uint32_t n) {
  unsigned char *nb;
  uint32_t want = s->len + n, cap;

  if (want <= s->cap)
    return 0;
  if (want > SSH_PACKET_MAX + 4096) {
    s->err = 1;
    return -1;
  }
  cap = s->cap ? s->cap : 256;
  while (cap < want)
    cap *= 2;
  if ((nb = malloc(cap)) == 0) {
    s->err = 1;
    return -1;
  }
  if (s->len)
    memcpy(nb, s->b, s->len);
  if (s->b) {
    explicit_bzero(s->b, s->cap);
    free(s->b);
  }
  s->b = nb;
  s->cap = cap;
  return 0;
}

void sb_put(struct sbuf *s, const void *p, uint32_t n) {
  if (sb_reserve(s, n) < 0)
    return;
  memcpy(s->b + s->len, p, n);
  s->len += n;
}

void sb_put_u8(struct sbuf *s, int v) {
  unsigned char c = (unsigned char)v;
  sb_put(s, &c, 1);
}

void sb_put_u32(struct sbuf *s, uint32_t v) {
  unsigned char c[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16),
                        (unsigned char)(v >> 8), (unsigned char)v};
  sb_put(s, c, 4);
}

void sb_put_string(struct sbuf *s, const void *p, uint32_t n) {
  sb_put_u32(s, n);
  sb_put(s, p, n);
}

void sb_put_cstring(struct sbuf *s, const char *p) {
  sb_put_string(s, p, (uint32_t)strlen(p));
}

void sb_put_mpint(struct sbuf *s, const unsigned char *p, uint32_t n) {
  while (n > 0 && *p == 0) {
    p++;
    n--;
  }
  if (n > 0 && (p[0] & 0x80)) {
    sb_put_u32(s, n + 1);
    sb_put_u8(s, 0);
  } else
    sb_put_u32(s, n);
  sb_put(s, p, n);
}

void sb_put_sbuf(struct sbuf *s, const struct sbuf *t) {
  sb_put_string(s, t->b, t->len);
}

uint32_t sb_left(const struct sbuf *s) { return s->len - s->rd; }

int sb_get_u8(struct sbuf *s) {
  if (sb_left(s) < 1) {
    s->err = 1;
    return -1;
  }
  return s->b[s->rd++];
}

uint32_t sb_get_u32(struct sbuf *s) {
  uint32_t v;

  if (sb_left(s) < 4) {
    s->err = 1;
    return 0;
  }
  v = ((uint32_t)s->b[s->rd] << 24) | ((uint32_t)s->b[s->rd + 1] << 16) |
      ((uint32_t)s->b[s->rd + 2] << 8) | s->b[s->rd + 3];
  s->rd += 4;
  return v;
}

const unsigned char *sb_get_string(struct sbuf *s, uint32_t *n) {
  uint32_t len = sb_get_u32(s);
  const unsigned char *p;

  if (s->err || len > sb_left(s)) {
    s->err = 1;
    *n = 0;
    return 0;
  }
  p = s->b + s->rd;
  s->rd += len;
  *n = len;
  return p;
}

int sb_get_cstring(struct sbuf *s, char *out, uint32_t max) {
  uint32_t n;
  const unsigned char *p = sb_get_string(s, &n);

  if (p == 0 || n >= max || memchr_nul(p, n))
    return -1;
  memcpy(out, p, n);
  out[n] = 0;
  return 0;
}

int sb_skip_string(struct sbuf *s) {
  uint32_t n;
  return sb_get_string(s, &n) ? 0 : -1;
}

static int memchr_nul(const unsigned char *p, uint32_t n) {
  while (n-- > 0)
    if (*p++ == 0)
      return 1;
  return 0;
}

// I/O

int ssh_readn(int fd, void *buf, uint32_t n) {
  unsigned char *p = buf;

  while (n > 0) {
    int r = (int)read(fd, p, (size_t)n);
    if (r <= 0)
      return -1;
    p += r;
    n -= (uint32_t)r;
  }
  return 0;
}

int ssh_writen(int fd, const void *buf, uint32_t n) {
  const unsigned char *p = buf;

  while (n > 0) {
    int r = (int)write(fd, p, (size_t)n);
    if (r <= 0)
      return -1;
    p += r;
    n -= (uint32_t)r;
  }
  return 0;
}

void ssh_log(struct ssh *s, const char *fmt, ...) {
  va_list va;
  char line[256];

  if (!s->debug)
    return;
  va_start(va, fmt);
  vsnprintf(line, sizeof(line), fmt, va);
  va_end(va);
  dprintf(2, "%s: %s\n", s->server ? "sshd" : "ssh", line);
}

void ssh_init(struct ssh *s, int fd, int server) {
  memset(s, 0, sizeof(*s));
  s->fd = fd;
  s->server = server;
  s->debug = getenv("SSH_DEBUG") != 0;
  sb_init(&s->i_local);
  sb_init(&s->i_peer);
  sb_init(&s->rbuf);
  sb_init(&s->obuf);
}

#define READ_AHEAD (64 * 1024)

// need bytes in rbuf, up to READ_AHEAD more
static int rfill(struct ssh *s, uint32_t need) {
  struct sbuf *b = &s->rbuf;
  uint32_t want = need > READ_AHEAD ? need : READ_AHEAD;
  int r;

  while (sb_left(b) < need) {
    if (b->rd > 0) {
      memmove(b->b, b->b + b->rd, sb_left(b));
      b->len -= b->rd;
      b->rd = 0;
    }
    if (sb_reserve(b, want - b->len) < 0)
      return -1;
    if ((r = (int)read(s->fd, b->b + b->len, (size_t)(b->cap - b->len))) <= 0)
      return -1;
    b->len += (uint32_t)r;
  }
  return 0;
}

// already read: skip the select
int ssh_pending(const struct ssh *s) { return sb_left(&s->rbuf) > 0; }

int ssh_versions(struct ssh *s) {
  char line[SSH_VERSION_MAX + 2];
  int n, i, lines = 0;

  snprintf(s->v_local, sizeof(s->v_local), "SSH-2.0-OpenProninx_0.1");
  n = snprintf(line, sizeof(line), "%s\r\n", s->v_local);
  if (ssh_writen(s->fd, line, (uint32_t)n) < 0)
    return -1;

  for (;;) {
    for (i = 0; i < (int)sizeof(line) - 1; i++) {
      if (rfill(s, 1) < 0)
        return -1;
      line[i] = (char)s->rbuf.b[s->rbuf.rd++];
      if (line[i] == '\n')
        break;
    }
    if (i >= (int)sizeof(line) - 1)
      return -1;
    while (i > 0 && (line[i] == '\n' || line[i] == '\r'))
      i--;
    line[i + 1] = 0;
    if (strncmp(line, "SSH-", 4) == 0)
      break;
    if (s->server || ++lines > 64)
      return -1;
  }
  if (strncmp(line, "SSH-2.0-", 8) != 0 && strncmp(line, "SSH-1.99-", 9) != 0)
    return -1;
  strlcpy(s->v_peer, line, sizeof(s->v_peer));
  ssh_log(s, "peer is %s", s->v_peer);
  return 0;
}

// PACKETS

static void poke_u64(unsigned char *p, uint64_t v) {
  int i;
  for (i = 7; i >= 0; i--) {
    p[i] = (unsigned char)v;
    v >>= 8;
  }
}

static int chachapoly_crypt(struct chacha_ctx *main, struct chacha_ctx *header,
                            uint32_t seqnr, unsigned char *dest,
                            const unsigned char *src, uint32_t len,
                            uint32_t aadlen, int do_encrypt) {
  unsigned char seqbuf[8], one[8] = {1, 0, 0, 0, 0, 0, 0, 0};
  unsigned char expected[POLY1305_TAGLEN], poly_key[POLY1305_KEYLEN];
  uint32_t done;
  int r = -1;

  memset(poly_key, 0, sizeof(poly_key));
  poke_u64(seqbuf, seqnr);
  chacha_ivsetup(main, seqbuf, 0);
  chacha_encrypt_bytes(main, poly_key, poly_key, sizeof(poly_key));
  if (!do_encrypt) {
    poly1305_auth(expected, src, aadlen + len, poly_key);
    if (timingsafe_bcmp(expected, src + aadlen + len, POLY1305_TAGLEN) != 0)
      goto out;
  }
  if (aadlen) {
    chacha_ivsetup(header, seqbuf, 0);
    chacha_encrypt_bytes(header, src, dest, aadlen);
  }
  chacha_ivsetup(main, seqbuf, one);
  done = chacha_encrypt_x4(main, src + aadlen, dest + aadlen, len);
  if (len > done)
    chacha_encrypt_bytes(main, src + aadlen + done, dest + aadlen + done, len - done);
  if (do_encrypt)
    poly1305_auth(dest + aadlen + len, dest, aadlen + len, poly_key);
  r = 0;
out:
  explicit_bzero(expected, sizeof(expected));
  explicit_bzero(poly_key, sizeof(poly_key));
  return r;
}

int ssh_send(struct ssh *s, struct sbuf *payload) {
  struct sbuf *pk = &s->obuf;
  unsigned char pad[256];
  uint32_t padlen, total, block = 8;
  int r;

  if (payload->err)
    return -1;
  padlen = block - ((5 + payload->len - (s->crypt_out ? 4 : 0)) % block);
  if (padlen < 4)
    padlen += block;
  total = 1 + payload->len + padlen;
  sb_reset(pk);
  if (sb_reserve(pk, 4 + total + POLY1305_TAGLEN) < 0)
    return -1;
  sb_put_u32(pk, total);
  sb_put_u8(pk, (int)padlen);
  sb_put(pk, payload->b, payload->len);
  arc4random_buf(pad, padlen);
  sb_put(pk, pad, padlen);
  if (s->crypt_out) {
    chachapoly_crypt(&s->main_out, &s->header_out, s->seq_out, pk->b, pk->b,
                     total, 4, 1);
    pk->len += POLY1305_TAGLEN;
  }
  r = ssh_writen(s->fd, pk->b, pk->len);
  s->seq_out++;
  return r;
}

int ssh_send_u8(struct ssh *s, int type) {
  struct sbuf p;
  int r;

  sb_init(&p);
  sb_put_u8(&p, type);
  r = ssh_send(s, &p);
  sb_free(&p);
  return r;
}

static int ssh_recv_raw(struct ssh *s, struct sbuf *payload) {
  unsigned char *head, lenbuf[4], seqbuf[8];
  uint32_t len, padlen, whole;

  if (rfill(s, 4) < 0)
    return -1;
  head = s->rbuf.b + s->rbuf.rd;
  if (s->crypt_in) {
    poke_u64(seqbuf, s->seq_in);
    chacha_ivsetup(&s->header_in, seqbuf, 0);
    chacha_encrypt_bytes(&s->header_in, head, lenbuf, 4);
  } else
    memcpy(lenbuf, head, 4);
  len = ((uint32_t)lenbuf[0] << 24) | ((uint32_t)lenbuf[1] << 16) |
        ((uint32_t)lenbuf[2] << 8) | lenbuf[3];
  if (len < 5 || len > SSH_PACKET_MAX || (!s->crypt_in && (len + 4) % 8 != 0))
    return -1;
  whole = 4 + len + (s->crypt_in ? POLY1305_TAGLEN : 0);
  if (rfill(s, whole) < 0)
    return -1;
  head = s->rbuf.b + s->rbuf.rd;
  if (s->crypt_in &&
      chachapoly_crypt(&s->main_in, &s->header_in, s->seq_in, head, head, len, 4, 0) < 0) {
    ssh_log(s, "bad mac on packet %u", s->seq_in);
    return -1;
  }
  s->seq_in++;
  s->rbuf.rd += whole;
  padlen = head[4];
  if (padlen < 4 || padlen + 1 > len)
    return -1;
  sb_reset(payload);
  sb_put(payload, head + 5, len - 1 - padlen);
  return payload->err ? -1 : 0;
}

int ssh_recv(struct ssh *s, struct sbuf *payload) {
  int type;

  for (;;) {
    if (ssh_recv_raw(s, payload) < 0)
      return -1;
    if (payload->len == 0)
      return -1;
    type = payload->b[0];
    switch (type) {
    case SSH_MSG_IGNORE:
    case SSH_MSG_DEBUG:
      if (s->in_kex && s->strict)
        return -1;
      continue;
    case SSH_MSG_UNIMPLEMENTED:
      continue;
    case SSH_MSG_DISCONNECT: {
      char why[256];
      sb_get_u8(payload);
      sb_get_u32(payload);
      if (sb_get_cstring(payload, why, sizeof(why)) == 0)
        ssh_log(s, "peer disconnected: %s", why);
      return -1;
    }
    case SSH_MSG_KEXINIT:
      if (!s->in_kex) {
        struct sbuf keep;

        sb_init(&keep);
        sb_put(&keep, payload->b, payload->len);
        sb_free(&s->i_peer);
        s->i_peer = keep;
        if (ssh_kex(s) < 0)
          return -1;
        continue;
      }
      return type;
    default:
      return type;
    }
  }
}

int ssh_disconnect(struct ssh *s, int code, const char *why) {
  struct sbuf p;

  sb_init(&p);
  sb_put_u8(&p, SSH_MSG_DISCONNECT);
  sb_put_u32(&p, (uint32_t)code);
  sb_put_cstring(&p, why);
  sb_put_cstring(&p, "");
  ssh_send(s, &p);
  sb_free(&p);
  return -1;
}

// KEY EXCHANGE

static int name_listed(const unsigned char *list, uint32_t n, const char *name) {
  uint32_t i = 0, start = 0, len = (uint32_t)strlen(name);

  for (i = 0; i <= n; i++) {
    if (i == n || list[i] == ',') {
      if (i - start == len && memcmp(list + start, name, len) == 0)
        return 1;
      start = i + 1;
    }
  }
  return 0;
}

static void kexinit_build(struct ssh *s) {
  unsigned char cookie[16];

  arc4random_buf(cookie, sizeof(cookie));
  sb_free(&s->i_local);
  sb_put_u8(&s->i_local, SSH_MSG_KEXINIT);
  sb_put(&s->i_local, cookie, sizeof(cookie));
  sb_put_cstring(&s->i_local, s->server ? KEX_ALGS_S : KEX_ALGS_C);
  sb_put_cstring(&s->i_local, HOSTKEY_ALGS);
  sb_put_cstring(&s->i_local, CIPHER_ALGS);
  sb_put_cstring(&s->i_local, CIPHER_ALGS);
  sb_put_cstring(&s->i_local, MAC_ALGS);
  sb_put_cstring(&s->i_local, MAC_ALGS);
  sb_put_cstring(&s->i_local, COMP_ALGS);
  sb_put_cstring(&s->i_local, COMP_ALGS);
  sb_put_cstring(&s->i_local, "");
  sb_put_cstring(&s->i_local, "");
  sb_put_u8(&s->i_local, 0);
  sb_put_u32(&s->i_local, 0);
}

static int kexinit_check(struct ssh *s) {
  struct sbuf p = s->i_peer;
  const unsigned char *l;
  uint32_t n;
  int i;
  static const char *want[8] = {0, HOSTKEY_ALGS, CIPHER_ALGS, CIPHER_ALGS,
                                MAC_ALGS, MAC_ALGS, COMP_ALGS, COMP_ALGS};

  p.rd = 0;
  sb_get_u8(&p);
  p.rd += 16;
  l = sb_get_string(&p, &n);
  if (l == 0 || !name_listed(l, n, "curve25519-sha256"))
    if (l == 0 || !name_listed(l, n, "curve25519-sha256@libssh.org"))
      return -1;
  if (!s->have_session_id)
    s->strict = name_listed(l, n, s->server ? "kex-strict-c-v00@openssh.com"
                                            : "kex-strict-s-v00@openssh.com");
  for (i = 1; i < 8; i++) {
    l = sb_get_string(&p, &n);
    if (l == 0 || !name_listed(l, n, want[i]))
      return -1;
  }
  return p.err ? -1 : 0;
}

static void hash_string(SHA2_CTX *c, const void *p, uint32_t n) {
  unsigned char len[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
                          (unsigned char)(n >> 8), (unsigned char)n};
  SHA256Update(c, len, 4);
  SHA256Update(c, p, n);
}

static void derive(const unsigned char *K, const unsigned char *H,
                   const unsigned char *sid, char x, unsigned char *out,
                   uint32_t need) {
  SHA2_CTX c;
  struct sbuf kb;
  unsigned char d[32];
  uint32_t have = 0;

  sb_init(&kb);
  sb_put_mpint(&kb, K, 32);
  while (have < need) {
    SHA256Init(&c);
    SHA256Update(&c, kb.b, kb.len);
    SHA256Update(&c, H, 32);
    if (have == 0) {
      SHA256Update(&c, (const unsigned char *)&x, 1);
      SHA256Update(&c, sid, 32);
    } else
      SHA256Update(&c, out, have);
    SHA256Final(d, &c);
    memcpy(out + have, d, have + 32 <= need ? 32 : need - have);
    have += 32;
  }
  sb_free(&kb);
  explicit_bzero(d, sizeof(d));
}

int ssh_kex(struct ssh *s) {
  struct sbuf p, blob, sig;
  unsigned char my_sec[32], my_pub[32], peer_pub[32], K[32], H[32];
  unsigned char keys[64];
  const unsigned char *q;
  uint32_t n;
  int type, r = -1;

  sb_init(&p);
  sb_init(&blob);
  sb_init(&sig);
  s->in_kex = 1;
  kexinit_build(s);
  if (ssh_send(s, &s->i_local) < 0)
    goto out;
  if (s->i_peer.len == 0) {
    if ((type = ssh_recv(s, &p)) != SSH_MSG_KEXINIT)
      goto out;
    sb_free(&s->i_peer);
    sb_put(&s->i_peer, p.b, p.len);
  }
  if (kexinit_check(s) < 0) {
    ssh_disconnect(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                   "no common algorithm");
    goto out;
  }

  arc4random_buf(my_sec, sizeof(my_sec));
  {
    static const unsigned char base[32] = {9};
    crypto_scalarmult_curve25519(my_pub, my_sec, base);
  }
  ssh_put_pubkey(&blob, s->hostkey_pub);

  if (s->server) {
    if ((type = ssh_recv(s, &p)) != SSH_MSG_KEX_ECDH_INIT)
      goto out;
    sb_get_u8(&p);
    if ((q = sb_get_string(&p, &n)) == 0 || n != 32)
      goto out;
    memcpy(peer_pub, q, 32);
  } else {
    sb_reset(&p);
    sb_put_u8(&p, SSH_MSG_KEX_ECDH_INIT);
    sb_put_string(&p, my_pub, 32);
    if (ssh_send(s, &p) < 0)
      goto out;
    if ((type = ssh_recv(s, &p)) != SSH_MSG_KEX_ECDH_REPLY)
      goto out;
    sb_get_u8(&p);
    if ((q = sb_get_string(&p, &n)) == 0)
      goto out;
    sb_reset(&blob);
    sb_put(&blob, q, n);
    if (ssh_get_pubkey(&blob, s->hostkey_pub) < 0)
      goto out;
    if ((q = sb_get_string(&p, &n)) == 0 || n != 32)
      goto out;
    memcpy(peer_pub, q, 32);
    if ((q = sb_get_string(&p, &n)) == 0)
      goto out;
    sb_reset(&sig);
    sb_put(&sig, q, n);
  }

  crypto_scalarmult_curve25519(K, my_sec, peer_pub);
  {
    unsigned char zero[32] = {0};
    if (timingsafe_bcmp(K, zero, 32) == 0)
      goto out;
  }

  {
    SHA2_CTX c;
    struct sbuf kb;

    SHA256Init(&c);
    hash_string(&c, s->server ? s->v_peer : s->v_local,
                (uint32_t)strlen(s->server ? s->v_peer : s->v_local));
    hash_string(&c, s->server ? s->v_local : s->v_peer,
                (uint32_t)strlen(s->server ? s->v_local : s->v_peer));
    hash_string(&c, s->server ? s->i_peer.b : s->i_local.b,
                s->server ? s->i_peer.len : s->i_local.len);
    hash_string(&c, s->server ? s->i_local.b : s->i_peer.b,
                s->server ? s->i_local.len : s->i_peer.len);
    hash_string(&c, blob.b, blob.len);
    hash_string(&c, s->server ? peer_pub : my_pub, 32);
    hash_string(&c, s->server ? my_pub : peer_pub, 32);
    sb_init(&kb);
    sb_put_mpint(&kb, K, 32);
    SHA256Update(&c, kb.b, kb.len);
    sb_free(&kb);
    SHA256Final(H, &c);
  }
  if (!s->have_session_id) {
    memcpy(s->session_id, H, 32);
    s->have_session_id = 1;
  }

  if (s->server) {
    unsigned char raw[64];
    unsigned long long siglen;

    crypto_sign_ed25519_detached(raw, &siglen, H, 32, s->hostkey_sec);
    sb_reset(&sig);
    ssh_put_sig(&sig, raw);
    sb_reset(&p);
    sb_put_u8(&p, SSH_MSG_KEX_ECDH_REPLY);
    sb_put_sbuf(&p, &blob);
    sb_put_string(&p, my_pub, 32);
    sb_put_sbuf(&p, &sig);
    if (ssh_send(s, &p) < 0)
      goto out;
  } else {
    unsigned char raw[64];

    sig.rd = 0;
    if (ssh_get_sig(&sig, raw) < 0 ||
        crypto_sign_ed25519_verify_detached(raw, H, 32, s->hostkey_pub) != 0) {
      ssh_disconnect(s, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                     "the host key signature does not verify");
      goto out;
    }
    if (s->verify_hostkey != 0 && s->verify_hostkey(s, s->hostkey_pub) < 0) {
      ssh_disconnect(s, SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE,
                     "host key refused");
      goto out;
    }
  }

  if (ssh_send_u8(s, SSH_MSG_NEWKEYS) < 0)
    goto out;
  if ((type = ssh_recv(s, &p)) != SSH_MSG_NEWKEYS)
    goto out;

  derive(K, H, s->session_id, s->server ? 'D' : 'C', keys, 64);
  chacha_keysetup(&s->main_out, keys, 256);
  chacha_keysetup(&s->header_out, keys + 32, 256);
  derive(K, H, s->session_id, s->server ? 'C' : 'D', keys, 64);
  chacha_keysetup(&s->main_in, keys, 256);
  chacha_keysetup(&s->header_in, keys + 32, 256);
  s->crypt_in = s->crypt_out = 1;
  if (s->strict)
    s->seq_in = s->seq_out = 0;
  ssh_log(s, "new keys%s", s->strict ? ", strict kex" : "");
  r = 0;
out:
  explicit_bzero(my_sec, sizeof(my_sec));
  explicit_bzero(K, sizeof(K));
  explicit_bzero(keys, sizeof(keys));
  sb_free(&p);
  sb_free(&blob);
  sb_free(&sig);
  sb_free(&s->i_peer);
  s->in_kex = 0;
  return r;
}
