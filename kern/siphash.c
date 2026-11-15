// SIPHASH
// keyed PRF, 2-4
#include "defs.h"

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

static uint64_t le64(const uchar *p) {
  uint64_t v = 0;
  int i;

  for (i = 7; i >= 0; i--)
    v = v << 8 | p[i];
  return v;
}

static void sipround(uint64_t v[4]) {
  v[0] += v[1];
  v[1] = ROTL(v[1], 13);
  v[1] ^= v[0];
  v[0] = ROTL(v[0], 32);
  v[2] += v[3];
  v[3] = ROTL(v[3], 16);
  v[3] ^= v[2];
  v[0] += v[3];
  v[3] = ROTL(v[3], 21);
  v[3] ^= v[0];
  v[2] += v[1];
  v[1] = ROTL(v[1], 17);
  v[1] ^= v[2];
  v[2] = ROTL(v[2], 32);
}

static void compress(uint64_t v[4], uint64_t m) {
  v[3] ^= m;
  sipround(v);
  sipround(v);
  v[0] ^= m;
}

uint64_t siphash24(const uchar key[16], const void *data, uint len) {
  const uchar *p = data;
  uint64_t k0 = le64(key), k1 = le64(key + 8), v[4], last;
  uint i, left = len & 7;

  v[0] = k0 ^ 0x736f6d6570736575ull;
  v[1] = k1 ^ 0x646f72616e646f6dull;
  v[2] = k0 ^ 0x6c7967656e657261ull;
  v[3] = k1 ^ 0x7465646279746573ull;
  for (i = 0; i + 8 <= len; i += 8)
    compress(v, le64(p + i));
  last = (uint64_t)(len & 0xff) << 56;
  while (left-- > 0)
    last |= (uint64_t)p[i + left] << (8 * left);
  compress(v, last);
  v[2] ^= 0xff;
  for (i = 0; i < 4; i++)
    sipround(v);
  return v[0] ^ v[1] ^ v[2] ^ v[3];
}
