// POLY1305
// three limbs: 44, 44, 42
#include "poly1305.h"

typedef unsigned long long u64;
typedef unsigned __int128 u128;

#define M44 0xfffffffffffULL
#define M42 0x3ffffffffffULL

// x86 is little-endian
static u64 le64(const u_char *p) {
  u64 v;

  __builtin_memcpy(&v, p, sizeof(v));
  return v;
}

static void put64(u_char *p, u64 v) {
  int i;

  for (i = 0; i < 8; i++, v >>= 8)
    p[i] = (u_char)v;
}

void poly1305_auth(u_char out[POLY1305_TAGLEN], const u_char *m, size_t len,
                   const u_char key[POLY1305_KEYLEN]) {
  u64 t0 = le64(key), t1 = le64(key + 8);
  u64 r0 = t0 & 0xffc0fffffffULL;
  u64 r1 = ((t0 >> 44) | (t1 << 20)) & 0xfffffc0ffffULL;
  u64 r2 = (t1 >> 24) & 0x00ffffffc0fULL;
  u64 s1 = r1 * (5 << 2), s2 = r2 * (5 << 2);
  u64 h0 = 0, h1 = 0, h2 = 0, g0, g1, g2, c, hibit = 1ULL << 40;
  u128 d0, d1, d2;
  u_char last[16];
  size_t i;

  while (len > 0) {
    if (len < 16) {
      for (i = 0; i < len; i++)
        last[i] = m[i];
      last[i++] = 1;
      for (; i < 16; i++)
        last[i] = 0;
      m = last;
      len = 16;
      hibit = 0;
    }
    t0 = le64(m);
    t1 = le64(m + 8);
    h0 += t0 & M44;
    h1 += ((t0 >> 44) | (t1 << 20)) & M44;
    h2 += ((t1 >> 24) & M42) | hibit;
    d0 = (u128)h0 * r0 + (u128)h1 * s2 + (u128)h2 * s1;
    d1 = (u128)h0 * r1 + (u128)h1 * r0 + (u128)h2 * s2;
    d2 = (u128)h0 * r2 + (u128)h1 * r1 + (u128)h2 * r0;
    c = (u64)(d0 >> 44);
    h0 = (u64)d0 & M44;
    d1 += c;
    c = (u64)(d1 >> 44);
    h1 = (u64)d1 & M44;
    d2 += c;
    c = (u64)(d2 >> 42);
    h2 = (u64)d2 & M42;
    h0 += c * 5;
    c = h0 >> 44;
    h0 &= M44;
    h1 += c;
    m += 16;
    len -= 16;
  }

  c = h1 >> 44;
  h1 &= M44;
  h2 += c;
  c = h2 >> 42;
  h2 &= M42;
  h0 += c * 5;
  c = h0 >> 44;
  h0 &= M44;
  h1 += c;
  c = h1 >> 44;
  h1 &= M44;
  h2 += c;
  c = h2 >> 42;
  h2 &= M42;
  h0 += c * 5;
  c = h0 >> 44;
  h0 &= M44;
  h1 += c;

  // h - p, kept where it did not borrow
  g0 = h0 + 5;
  c = g0 >> 44;
  g0 &= M44;
  g1 = h1 + c;
  c = g1 >> 44;
  g1 &= M44;
  g2 = h2 + c - (1ULL << 42);
  c = (g2 >> 63) - 1;
  h0 = (h0 & ~c) | (g0 & c);
  h1 = (h1 & ~c) | (g1 & c);
  h2 = (h2 & ~c) | (g2 & c);

  t0 = le64(key + 16);
  t1 = le64(key + 24);
  h0 += t0 & M44;
  c = h0 >> 44;
  h0 &= M44;
  h1 += (((t0 >> 44) | (t1 << 20)) & M44) + c;
  c = h1 >> 44;
  h1 &= M44;
  h2 += ((t1 >> 24) & M42) + c;
  h2 &= M42;
  put64(out, h0 | h1 << 44);
  put64(out + 8, h1 >> 20 | h2 << 24);
}
