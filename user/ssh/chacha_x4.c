// CHACHA20, FOUR BLOCKS AT A TIME
// each state word is a vector across four blocks. SSE2
#include "chacha_x4.h"

typedef unsigned int v4 __attribute__((vector_size(16)));
typedef unsigned int v4u __attribute__((vector_size(16), aligned(1)));
typedef unsigned short v8h __attribute__((vector_size(16)));
typedef unsigned char v16b __attribute__((vector_size(16)));

// 16 and 8 move whole bytes
#define ROT(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define ROT16(v) ((v4)__builtin_shufflevector((v8h)(v), (v8h)(v), 1, 0, 3, 2, 5, 4, 7, 6))
#define ROT8(v)                                                                 \
  ((v4)__builtin_shufflevector((v16b)(v), (v16b)(v), 3, 0, 1, 2, 7, 4, 5, 6, 11, 8, 9,  \
                               10, 15, 12, 13, 14))
#define QR(a, b, c, d)                                                          \
  do {                                                                          \
    a += b;                                                                     \
    d = ROT16(d ^ a);                                                           \
    c += d;                                                                     \
    b = ROT(b ^ c, 12);                                                         \
    a += b;                                                                     \
    d = ROT8(d ^ a);                                                            \
    c += d;                                                                     \
    b = ROT(b ^ c, 7);                                                          \
  } while (0)

// words i..i+3 of the four blocks, out to each block
static void put4(v4 a, v4 b, v4 c, v4 d, const unsigned char *m, unsigned char *o) {
  v4 t0 = __builtin_shufflevector(a, b, 0, 4, 1, 5);
  v4 t1 = __builtin_shufflevector(a, b, 2, 6, 3, 7);
  v4 t2 = __builtin_shufflevector(c, d, 0, 4, 1, 5);
  v4 t3 = __builtin_shufflevector(c, d, 2, 6, 3, 7);

  *(v4u *)(o + 0) = *(const v4u *)(m + 0) ^ __builtin_shufflevector(t0, t2, 0, 1, 4, 5);
  *(v4u *)(o + 64) = *(const v4u *)(m + 64) ^ __builtin_shufflevector(t0, t2, 2, 3, 6, 7);
  *(v4u *)(o + 128) = *(const v4u *)(m + 128) ^ __builtin_shufflevector(t1, t3, 0, 1, 4, 5);
  *(v4u *)(o + 192) = *(const v4u *)(m + 192) ^ __builtin_shufflevector(t1, t3, 2, 3, 6, 7);
}

u_int chacha_encrypt_x4(struct chacha_ctx *ctx, const u_char *m, u_char *c, u_int bytes) {
  u_int *in = ctx->input, lo, done;
  v4 x[16], s[16];
  int i;

  for (done = 0; bytes - done >= 256; done += 256, m += 256, c += 256) {
    for (i = 0; i < 16; i++)
      s[i] = (v4){in[i], in[i], in[i], in[i]};
    lo = in[12];
    s[12] = (v4){lo, lo + 1, lo + 2, lo + 3};
    s[13] += (v4){0, lo + 1 < lo, lo + 2 < lo, lo + 3 < lo};
    for (i = 0; i < 16; i++)
      x[i] = s[i];
    for (i = 0; i < 20; i += 2) {
      QR(x[0], x[4], x[8], x[12]);
      QR(x[1], x[5], x[9], x[13]);
      QR(x[2], x[6], x[10], x[14]);
      QR(x[3], x[7], x[11], x[15]);
      QR(x[0], x[5], x[10], x[15]);
      QR(x[1], x[6], x[11], x[12]);
      QR(x[2], x[7], x[8], x[13]);
      QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++)
      x[i] += s[i];
    put4(x[0], x[1], x[2], x[3], m, c);
    put4(x[4], x[5], x[6], x[7], m + 16, c + 16);
    put4(x[8], x[9], x[10], x[11], m + 32, c + 32);
    put4(x[12], x[13], x[14], x[15], m + 48, c + 48);
    if ((in[12] = lo + 4) < lo)
      in[13]++;
  }
  return done;
}
