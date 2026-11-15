#ifndef PRONINX_SSH_H
#define PRONINX_SSH_H
// SSH-2: curve25519-sha256, ssh-ed25519, chacha20-poly1305
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include "crypto.h"

#define SSH_MSG_DISCONNECT 1
#define SSH_MSG_IGNORE 2
#define SSH_MSG_UNIMPLEMENTED 3
#define SSH_MSG_DEBUG 4
#define SSH_MSG_SERVICE_REQUEST 5
#define SSH_MSG_SERVICE_ACCEPT 6
#define SSH_MSG_EXT_INFO 7
#define SSH_MSG_KEXINIT 20
#define SSH_MSG_NEWKEYS 21
#define SSH_MSG_KEX_ECDH_INIT 30
#define SSH_MSG_KEX_ECDH_REPLY 31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_USERAUTH_BANNER 53
#define SSH_MSG_USERAUTH_PK_OK 60
#define SSH_MSG_GLOBAL_REQUEST 80
#define SSH_MSG_REQUEST_SUCCESS 81
#define SSH_MSG_REQUEST_FAILURE 82
#define SSH_MSG_CHANNEL_OPEN 90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA 94
#define SSH_MSG_CHANNEL_EXTENDED_DATA 95
#define SSH_MSG_CHANNEL_EOF 96
#define SSH_MSG_CHANNEL_CLOSE 97
#define SSH_MSG_CHANNEL_REQUEST 98
#define SSH_MSG_CHANNEL_SUCCESS 99
#define SSH_MSG_CHANNEL_FAILURE 100

#define SSH_DISCONNECT_PROTOCOL_ERROR 2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED 3
#define SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE 5
#define SSH_DISCONNECT_BY_APPLICATION 11

#define SSH_PACKET_MAX (256 * 1024)
#define SSH_VERSION_MAX 256

// a growing byte string, read from rd and written at len
struct sbuf {
  unsigned char *b;
  uint32_t len, cap, rd;
  int err;
};

void sb_init(struct sbuf *s);
void sb_free(struct sbuf *s);
void sb_reset(struct sbuf *s);
int sb_reserve(struct sbuf *s, uint32_t n);
void sb_put(struct sbuf *s, const void *p, uint32_t n);
void sb_put_u8(struct sbuf *s, int v);
void sb_put_u32(struct sbuf *s, uint32_t v);
void sb_put_string(struct sbuf *s, const void *p, uint32_t n);
void sb_put_cstring(struct sbuf *s, const char *p);
void sb_put_mpint(struct sbuf *s, const unsigned char *p, uint32_t n);
void sb_put_sbuf(struct sbuf *s, const struct sbuf *t);
uint32_t sb_left(const struct sbuf *s);
int sb_get_u8(struct sbuf *s);
uint32_t sb_get_u32(struct sbuf *s);
const unsigned char *sb_get_string(struct sbuf *s, uint32_t *n);
int sb_get_cstring(struct sbuf *s, char *out, uint32_t max);
int sb_skip_string(struct sbuf *s);

struct ssh {
  int fd;
  int server;
  int debug;
  char v_local[SSH_VERSION_MAX], v_peer[SSH_VERSION_MAX];
  struct sbuf i_local, i_peer;
  uint32_t seq_in, seq_out;
  int crypt_in, crypt_out;
  struct chacha_ctx main_in, header_in, main_out, header_out;
  unsigned char session_id[32];
  int have_session_id;
  int strict, in_kex;
  unsigned char hostkey_pub[32];
  unsigned char hostkey_sec[64];
  int have_hostkey;
  int (*verify_hostkey)(struct ssh *, const unsigned char pub[32]);
  struct sbuf rbuf; // read ahead
  struct sbuf obuf;
  char peer[64];
};

void ssh_init(struct ssh *s, int fd, int server);
int ssh_versions(struct ssh *s);
int ssh_kex(struct ssh *s);
int ssh_send(struct ssh *s, struct sbuf *payload);
int ssh_send_u8(struct ssh *s, int type);
int ssh_recv(struct ssh *s, struct sbuf *payload);
int ssh_pending(const struct ssh *s);
int ssh_disconnect(struct ssh *s, int code, const char *why);
void ssh_log(struct ssh *s, const char *fmt, ...);
int ssh_readn(int fd, void *buf, uint32_t n);
int ssh_writen(int fd, const void *buf, uint32_t n);

// keys and their files
int ssh_key_generate(unsigned char pub[32], unsigned char sec[64]);
int ssh_key_write(const char *path, const unsigned char pub[32],
                  const unsigned char sec[64], const char *comment);
int ssh_key_read(const char *path, unsigned char pub[32], unsigned char sec[64]);
int ssh_pubkey_read(const char *path, unsigned char pub[32]);
void ssh_put_pubkey(struct sbuf *s, const unsigned char pub[32]);
int ssh_get_pubkey(struct sbuf *s, unsigned char pub[32]);
void ssh_put_sig(struct sbuf *s, const unsigned char sig[64]);
int ssh_get_sig(struct sbuf *s, unsigned char sig[64]);
int ssh_b64_encode(const unsigned char *in, uint32_t n, char *out, uint32_t max);
int ssh_b64_decode(const char *in, unsigned char *out, uint32_t max);
int ssh_fingerprint(const unsigned char pub[32], char *out, uint32_t max);
void ssh_user_path(char *out, uint32_t max, const char *name);

// terminal modes, rfc 4254 section 8
void ssh_put_ttymodes(struct sbuf *s, const struct termios *t);
void ssh_get_ttymodes(const unsigned char *p, uint32_t n, struct termios *t);

#endif
