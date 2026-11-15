#ifndef PRONINX_SSH_CRYPTO_H
#define PRONINX_SSH_CRYPTO_H
#include <sys/types.h>
#include <sha2.h>
#include "chacha.h"
#include "poly1305.h"
#include "crypto_api.h"

int crypto_scalarmult_curve25519(unsigned char *, const unsigned char *,
                                 const unsigned char *);
void explicit_bzero(void *, size_t);
int timingsafe_bcmp(const void *, const void *, size_t);
void arc4random_buf(void *, size_t);
uint32_t arc4random(void);

#endif
