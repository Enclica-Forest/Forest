#ifndef FOREST_LIBC_HASH_H
#define FOREST_LIBC_HASH_H

/* Real cryptographic hash/encoding primitives for userspace -- standard
 * textbook algorithms (FIPS 180-1/180-2, RFC 1321, RFC 4648), not
 * approximations. Implemented in userspace/libc/hash.c. Nothing in this
 * kernel had any hash/crypto support before this; git.c's object hashing
 * and openssl.c/gpg.c's real hash subcommands are built on these. */

#include <stddef.h>

void sha1(const void *data, size_t len, unsigned char out[20]);
void sha256(const void *data, size_t len, unsigned char out[32]);
void md5(const void *data, size_t len, unsigned char out[16]);

/* Renders a digest as lowercase hex into out (out must have room for
 * digest_len*2 + 1 bytes). */
void hash_to_hex(const unsigned char *digest, size_t digest_len, char *out);

/* Returns the number of bytes written to out (excluding the NUL
 * terminator, which is still written if out_size allows). Returns -1 if
 * out_size is too small. */
long base64_encode(const void *data, size_t len, char *out, size_t out_size);
long base64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_size);

#endif
