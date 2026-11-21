#include <stdio.h>
#include <stdlib.h>

#include <openssl/md5.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>

static char *
hex_format(unsigned char *digest, int len, char *buf)
{
	static char static_buf[129];

	if (buf == NULL)
		buf = static_buf;

	for (int i = 0; i < len; i++)
		sprintf(buf + (i * 2), "%02x", digest[i]);

	return (buf);
}

#define GEN_FILE_FUNC(func_name, ctx_type, init_fn, update_fn, final_fn, digest_len) \
char *func_name(const char *filename, char *buf) \
{ \
	unsigned char digest[digest_len]; \
	unsigned char buffer[4096]; \
	ctx_type ctx; \
	size_t n; \
	FILE *f = fopen(filename, "rb"); \
	if (!f) return (NULL); \
	init_fn(&ctx); \
	while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) \
		update_fn(&ctx, buffer, n); \
	final_fn(digest, &ctx); \
	fclose(f); \
	return (hex_format(digest, digest_len, buf)); \
}

GEN_FILE_FUNC(MD5File, MD5_CTX, MD5_Init, MD5_Update, MD5_Final, MD5_DIGEST_LENGTH)
GEN_FILE_FUNC(RMD160File, RIPEMD160_CTX, RIPEMD160_Init, RIPEMD160_Update, RIPEMD160_Final, RIPEMD160_DIGEST_LENGTH)
GEN_FILE_FUNC(SHA1File, SHA_CTX, SHA1_Init, SHA1_Update, SHA1_Final, SHA_DIGEST_LENGTH)
GEN_FILE_FUNC(SHA256_File, SHA256_CTX, SHA256_Init, SHA256_Update, SHA256_Final, SHA256_DIGEST_LENGTH)
GEN_FILE_FUNC(SHA384_File, SHA512_CTX, SHA384_Init, SHA384_Update, SHA384_Final, SHA384_DIGEST_LENGTH)
GEN_FILE_FUNC(SHA512_File, SHA512_CTX, SHA512_Init, SHA512_Update, SHA512_Final, SHA512_DIGEST_LENGTH)
