/* prismd: the few primitives Nexus Link and the sense channel need, with no
 * library behind them: SHA-256, HMAC-SHA256 (frame signatures, key derivation),
 * SHA-1 (the WebSocket accept key only), base64, hex, a constant-time compare and
 * random bytes from the kernel. */
#ifndef PRISM_CRYPTO_H
#define PRISM_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]);
void sha1(const void *data, size_t len, uint8_t out[20]);

/* out needs 2*len+1 bytes; lowercase. */
void hex_encode(const uint8_t *in, size_t len, char *out);
/* Decode exactly 2*len lowercase-or-uppercase hex chars; 0 on success, -1 on bad input. */
int hex_decode(const char *in, size_t len, uint8_t *out);
/* out needs 4*ceil(len/3)+1 bytes. */
void base64_encode(const uint8_t *in, size_t len, char *out);
/* 1 when equal, 0 otherwise; runs in time independent of where they differ. */
int ct_equal(const void *a, const void *b, size_t len);
/* Fill from getrandom()/urandom; 0 on success. */
int random_bytes(uint8_t *out, size_t len);
#endif
