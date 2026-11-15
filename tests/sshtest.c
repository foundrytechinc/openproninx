#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include "crypto.h"

static int unhex(const char *s, unsigned char *out, int max) {
  int n = 0;

  for (; s[0] && s[1] && n < max; s += 2) {
    int hi = isdigit(s[0]) ? s[0] - '0' : tolower(s[0]) - 'a' + 10;
    int lo = isdigit(s[1]) ? s[1] - '0' : tolower(s[1]) - 'a' + 10;
    out[n++] = (unsigned char)(hi << 4 | lo);
  }
  return n;
}

static int check(const char *what, const unsigned char *got, const char *hex) {
  unsigned char want[128];
  int n = unhex(hex, want, sizeof(want));

  if (memcmp(got, want, (size_t)n) != 0) {
    printf("FAIL %s\n", what);
    return 1;
  }
  printf("ok   %s\n", what);
  return 0;
}

int main(void) {
  SHA2_CTX ctx;
  unsigned char d[64], a[32], b[32], pk[32], sk[64], sig[64], k[32];
  unsigned char key[32], stream[64];
  unsigned long long siglen;
  int bad = 0;

  SHA256Init(&ctx);
  SHA256Update(&ctx, (const unsigned char *)"abc", 3);
  SHA256Final(d, &ctx);
  bad += check("sha256", d,
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  SHA512Init(&ctx);
  SHA512Update(&ctx, (const unsigned char *)"abc", 3);
  SHA512Final(d, &ctx);
  bad += check("sha512", d,
               "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

  // RFC 7748 6
  unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
  unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
  {
    static const unsigned char base[32] = {9};
    crypto_scalarmult_curve25519(pk, a, base);
    bad += check("x25519 public", pk,
                 "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    crypto_scalarmult_curve25519(pk, b, base);
    crypto_scalarmult_curve25519(k, a, pk);
    bad += check("x25519 shared", k,
                 "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  }

  // RFC 8032 7
  unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", a, 32);
  crypto_sign_ed25519_seed_keypair(pk, sk, a);
  bad += check("ed25519 public", pk,
               "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  crypto_sign_ed25519_detached(sig, &siglen, (const unsigned char *)"", 0, sk);
  bad += check("ed25519 sign", sig,
               "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
               "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  if (crypto_sign_ed25519_verify_detached(sig, (const unsigned char *)"", 0,
                                          pk) != 0) {
    printf("FAIL ed25519 verify\n");
    bad++;
  } else
    printf("ok   ed25519 verify\n");
  sig[0] ^= 1;
  if (crypto_sign_ed25519_verify_detached(sig, (const unsigned char *)"", 0,
                                          pk) == 0) {
    printf("FAIL ed25519 verify accepts a bad signature\n");
    bad++;
  } else
    printf("ok   ed25519 refuses a bad signature\n");

  // RFC 8439 2
  {
    struct chacha_ctx cc;
    unsigned char nonce[8], ctr[8];
    static const char *plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you o";

    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    memset(nonce, 0, 8);
    nonce[3] = 0x4a;
    memset(ctr, 0, 8);
    ctr[0] = 1;
    chacha_keysetup(&cc, key, 256);
    chacha_ivsetup(&cc, nonce, ctr);
    chacha_encrypt_bytes(&cc, (const unsigned char *)plain, stream, 64);
    bad += check("chacha20", stream,
                 "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
                 "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8");
  }
  // RFC 8439 2
  {
    unsigned char tag[16];
    static const char *msg = "Cryptographic Forum Research Group";

    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    poly1305_auth(tag, (const unsigned char *)msg, strlen(msg), key);
    bad += check("poly1305", tag, "a8061dc1305136c6c22b8baf0c0127a9");
  }
  printf(bad ? "sshtest: %d FAILED\n" : "sshtest: all ok\n", bad);
  exit(bad != 0);
}
