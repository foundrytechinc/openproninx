// SHA-2 INSIDE LIBCRYPT: the block functions
#ifndef _LIBCRYPT_SHA2_H
#define _LIBCRYPT_SHA2_H

#include_next <sha2.h>

void __sha256_block(uint32_t state[8], const uint8_t *in, size_t num);
void __sha256_block_generic(uint32_t state[8], const uint8_t *in, size_t num);
void __sha256_block_shani(uint32_t state[8], const uint8_t *in, size_t num);
void __sha512_block(uint64_t state[8], const uint8_t *in, size_t num);
void __sha512_block_generic(uint64_t state[8], const uint8_t *in, size_t num);

#endif
