// RANDOM

#include "defs.h"
#include "devfs.h"
#include "file.h"
#include "mmu.h"
#include "uio.h"
#include "inc/stat.h"
#include "param.h"
#include "proc.h"
#include "mutex.h"
#include "x86.h"

#define CHACHA_ROUNDS 20
#define REKEY_BYTES (1024 * 1024)

static struct {
  struct mutex lock;
  uint32_t state[16];
  uchar buffer[64];
  int have;
  uint64_t since_rekey;
  int seeded;
  int hardware;
} rnd;

static uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

#define QUARTERROUND(a, b, c, d)                                               \
  a += b;                                                                      \
  d = rotl(d ^ a, 16);                                                         \
  c += d;                                                                      \
  b = rotl(b ^ c, 12);                                                         \
  a += b;                                                                      \
  d = rotl(d ^ a, 8);                                                          \
  c += d;                                                                      \
  b = rotl(b ^ c, 7)

static void chacha_block(const uint32_t in[16], uchar out[64]) {
  uint32_t x[16];
  int i;

  for (i = 0; i < 16; i++)
    x[i] = in[i];
  for (i = 0; i < CHACHA_ROUNDS; i += 2) {
    QUARTERROUND(x[0], x[4], x[8], x[12]);
    QUARTERROUND(x[1], x[5], x[9], x[13]);
    QUARTERROUND(x[2], x[6], x[10], x[14]);
    QUARTERROUND(x[3], x[7], x[11], x[15]);
    QUARTERROUND(x[0], x[5], x[10], x[15]);
    QUARTERROUND(x[1], x[6], x[11], x[12]);
    QUARTERROUND(x[2], x[7], x[8], x[13]);
    QUARTERROUND(x[3], x[4], x[9], x[14]);
  }
  for (i = 0; i < 16; i++) {
    uint32_t v = x[i] + in[i];
    out[i * 4] = (uchar)v;
    out[i * 4 + 1] = (uchar)(v >> 8);
    out[i * 4 + 2] = (uchar)(v >> 16);
    out[i * 4 + 3] = (uchar)(v >> 24);
  }
}

static int have_rdseed(void) {
  uint32_t a, b, c, d;

  cpuid_count(0, 0, &a, &b, &c, &d);
  if (a < 7)
    return 0;
  cpuid_count(7, 0, &a, &b, &c, &d);
  return (b & (1u << 18)) != 0;
}

static int have_rdrand(void) {
  uint32_t a, b, c, d;

  cpuid_count(1, 0, &a, &b, &c, &d);
  return (c & (1u << 30)) != 0;
}

static int cpu_random(uint64_t *out, int seed) {
  unsigned char ok = 0;
  uint64_t value = 0;
  int i;

  for (i = 0; i < 10; i++) {
    if (seed)
      __asm __volatile("rdseed %0; setc %1" : "=r"(value), "=qm"(ok));
    else
      __asm __volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
    if (ok) {
      *out = value;
      return 0;
    }
  }
  return -1;
}

static uint64_t jitter(void) {
  uint64_t acc = 0;
  int i, k;

  for (i = 0; i < 64; i++) {
    volatile int spin = 0;
    uint64_t before = rdtsc();

    for (k = 0; k < (int)(before & 0x3f) + 8; k++)
      spin++;
    acc = (acc << 1) | ((rdtsc() - before) & 1);
  }
  return acc;
}

// call with the lock held
static void rekey(const uint64_t *extra, int nextra) {
  static const char sigma[17] = "expand 32-byte k";
  uchar key[64];
  uint64_t word;
  int i, seed = have_rdseed(), rand = have_rdrand();

  for (i = 0; i < 8; i++) {
    if ((seed && cpu_random(&word, 1) == 0) ||
        (rand && cpu_random(&word, 0) == 0))
      rnd.hardware = 1;
    else
      word = jitter();
    if (i < nextra && extra != 0)
      word ^= extra[i];
    word ^= ((uint64_t)rnd.state[4 + i % 8] << 32) ^ rnd.state[8 + i % 8];
    key[i * 8 + 0] = (uchar)word;
    key[i * 8 + 1] = (uchar)(word >> 8);
    key[i * 8 + 2] = (uchar)(word >> 16);
    key[i * 8 + 3] = (uchar)(word >> 24);
    key[i * 8 + 4] = (uchar)(word >> 32);
    key[i * 8 + 5] = (uchar)(word >> 40);
    key[i * 8 + 6] = (uchar)(word >> 48);
    key[i * 8 + 7] = (uchar)(word >> 56);
  }

  for (i = 0; i < 4; i++)
    rnd.state[i] = ((uint32_t)(uchar)sigma[i * 4]) |
                   ((uint32_t)(uchar)sigma[i * 4 + 1] << 8) |
                   ((uint32_t)(uchar)sigma[i * 4 + 2] << 16) |
                   ((uint32_t)(uchar)sigma[i * 4 + 3] << 24);
  for (i = 0; i < 8; i++)
    rnd.state[4 + i] = (uint32_t)key[i * 4] | ((uint32_t)key[i * 4 + 1] << 8) |
                       ((uint32_t)key[i * 4 + 2] << 16) |
                       ((uint32_t)key[i * 4 + 3] << 24);
  rnd.state[12] = 0;
  rnd.state[13] = (uint32_t)rdtsc();
  rnd.state[14] = (uint32_t)key[40] | ((uint32_t)key[41] << 8);
  rnd.state[15] = (uint32_t)key[44] | ((uint32_t)key[45] << 8);
  rnd.have = 0;
  rnd.since_rekey = 0;
  rnd.seeded = 1;
  memset(key, 0, sizeof(key));
}

void randominit(void) {
  mtx_init(&rnd.lock, "random");
  mtx_enter(&rnd.lock);
  rekey(0, 0);
  mtx_leave(&rnd.lock);
  // random and its urandom alias; root writes to stir it
  devfs_add("random", FNU_TYPE_DEVICE, RANDOM, 0, 0644);
  devfs_add("urandom", FNU_TYPE_DEVICE, RANDOM, 0, 0644);
  memdevattach();
  printf("RANDOM: chacha20, seeded from %s\n",
          rnd.hardware ? "the processor" : "timing jitter only");
}

void random_bytes(void *dst, uint n) {
  uchar *out = dst;
  uint take;

  mtx_enter(&rnd.lock);
  if (!rnd.seeded)
    rekey(0, 0);
  while (n > 0) {
    if (rnd.have == 0) {
      rnd.state[12]++;
      chacha_block(rnd.state, rnd.buffer);
      rnd.have = sizeof(rnd.buffer);
    }
    take = (uint)rnd.have < n ? (uint)rnd.have : n;
    memmove(out, rnd.buffer + sizeof(rnd.buffer) - rnd.have, take);
    memset(rnd.buffer + sizeof(rnd.buffer) - rnd.have, 0, take);
    rnd.have -= (int)take;
    out += take;
    n -= take;
    rnd.since_rekey += take;
    if (rnd.since_rekey >= REKEY_BYTES)
      rekey(0, 0);
  }
  mtx_leave(&rnd.lock);
}

static int randomread(struct inode *ip, struct uio *uio) {
  size_t want = uio->uio_resid;
  char *buf;

  (void)ip;
  if ((buf = page_alloc()) == 0)
    return -FNU_ERR_NOMEMORY;
  while (uio->uio_resid > 0) {
    size_t chunk = uio->uio_resid > PGSIZE ? PGSIZE : uio->uio_resid;

    random_bytes(buf, (uint)chunk);
    if (uiomove(buf, chunk, uio) != 0) {
      page_free(buf);
      return -FNU_ERR_FAULT;
    }
  }
  page_free(buf);
  return (int)want;
}

// mixed into the next key, never replaces the pool
void random_add(const void *data, uint n) {
  const uchar *p = data;
  uint64_t extra[8];
  uint take;

  while (n > 0) {
    take = n < sizeof(extra) ? n : (uint)sizeof(extra);
    memset(extra, 0, sizeof(extra));
    memmove(extra, p, take);
    mtx_enter(&rnd.lock);
    rekey(extra, (int)nitems(extra));
    mtx_leave(&rnd.lock);
    p += take;
    n -= take;
  }
  memset(extra, 0, sizeof(extra));
}

// what is written goes into the next key
static int randomwrite(struct inode *ip, struct uio *uio) {
  size_t want = uio->uio_resid;
  uint64_t extra[8];

  (void)ip;
  while (uio->uio_resid > 0) {
    size_t chunk = uio->uio_resid > sizeof(extra) ? sizeof(extra) : uio->uio_resid;

    memset(extra, 0, sizeof(extra));
    if (uiomove(extra, chunk, uio) != 0)
      return -FNU_ERR_FAULT;
    mtx_enter(&rnd.lock);
    rekey(extra, (int)nitems(extra));
    mtx_leave(&rnd.lock);
  }
  memset(extra, 0, sizeof(extra));
  return (int)want;
}

void randomdevinit(void) {
  devsw[RANDOM].read = randomread;
  devsw[RANDOM].write = randomwrite;
}
