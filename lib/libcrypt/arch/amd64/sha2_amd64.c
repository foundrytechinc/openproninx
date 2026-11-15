// SHA-2 BLOCKS: cpuid for the SHA extensions
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

#include <sha2.h>

static int shani = -1;

static int has_shani(void) {
  uint32_t a, b, c, d;

  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
  if (a < 7)
    return 0;
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
  return (b >> 29) & 1;
}

void __sha256_block(uint32_t state[8], const uint8_t *in, size_t num) {
  if (shani < 0)
    shani = has_shani();
  if (shani) {
    __sha256_block_shani(state, in, num);
    return;
  }
  __sha256_block_generic(state, in, num);
}

void __sha512_block(uint64_t state[8], const uint8_t *in, size_t num) {
  __sha512_block_generic(state, in, num);
}
