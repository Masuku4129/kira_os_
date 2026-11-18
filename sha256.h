#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

// SHA-256 (потоковый интерфейс и одним вызовом)
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    uint32_t buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* c);
void sha256_update(sha256_ctx_t* c, const void* data, size_t len);
void sha256_final(sha256_ctx_t* c, uint8_t out[32]);
void sha256(const void* data, size_t len, uint8_t out[32]);

#endif
