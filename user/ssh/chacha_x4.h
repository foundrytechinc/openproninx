#ifndef CHACHA_X4_H
#define CHACHA_X4_H

#include "chacha.h"

// whole 256-byte runs; the rest is chacha_encrypt_bytes's
u_int chacha_encrypt_x4(struct chacha_ctx *ctx, const u_char *m, u_char *c, u_int bytes);

#endif
