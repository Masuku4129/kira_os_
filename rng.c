#include "rng.h"
#include "crypto.h"
#include "spinlock.h"

static spinlock_t lk = SPINLOCK_INIT;
static sha256_ctx_t pool;
static uint8_t key[32];
static uint64_t nonce_ctr, events, events_at_reseed;
static int ready, strong;
static char src[48];

static void src_add(const char* s) {
    unsigned n = 0; while (src[n]) n++;
    if (n && n + 2 < sizeof src) { src[n++] = ','; src[n++] = ' '; }
    while (*s && n + 1 < sizeof src) src[n++] = *s++;
    src[n] = 0;
}

void rng_mix(const void* d, size_t n) {
    uint64_t f = spin_lock_irqsave(&lk);
    sha256_update(&pool, d, n);
    spin_unlock_irqrestore(&lk, f);
}

void rng_event(void) {
    if (!ready) return;
    uint64_t c = arch_cycles();
    uint64_t f = spin_lock_irqsave(&lk);
    sha256_update(&pool, &c, sizeof c);
    events++;
    spin_unlock_irqrestore(&lk, f);
}

/* Под замком: новый ключ = SHA-256(ключ || свёртка пула); пул продолжается от свёртки. */
static void reseed(void) {
    uint8_t d[32], m[64];
    sha256_ctx_t c = pool;
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) { m[i] = key[i]; m[32 + i] = d[i]; }
    sha256(m, 64, key);
    sha256_init(&pool);
    sha256_update(&pool, d, 32);
    events_at_reseed = events;
    crypto_wipe(d, sizeof d); crypto_wipe(m, sizeof m); crypto_wipe(&c, sizeof c);
}

static int hw_words(unsigned n) {                       /* под замком: n слов от процессора в пул */
    int got = 0;
    for (unsigned i = 0; i < n; i++) { uint64_t v; if (arch_hw_random(&v)) { sha256_update(&pool, &v, 8); got++; } }
    return got;
}

void rng_init(void) {
    sha256_init(&pool);
    uint64_t c = arch_cycles();
    sha256_update(&pool, &c, 8);
    const char* hw = arch_hw_random_name();
    if (hw && hw_words(8) == 8) { strong = 1; src_add(hw); }
    uint8_t b[64];
    int n = virtio_rng_read(b, sizeof b);
    if (n >= 32) { sha256_update(&pool, b, (size_t)n); strong = 1; src_add("virtio-rng"); }
    crypto_wipe(b, sizeof b);
    src_add("interrupt timing");
    reseed();
    ready = 1;
}

int rng_get(void* out, size_t n) {
    uint8_t blk[64], nonce[12];
    uint8_t* o = (uint8_t*)out;
    uint64_t f = spin_lock_irqsave(&lk);
    uint64_t c = arch_cycles();
    sha256_update(&pool, &c, 8);
    (void)hw_words(2);
    if (!ready || events - events_at_reseed >= 16 || nonce_ctr % 64 == 0) reseed();
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(i < 8 ? nonce_ctr >> (8 * i) : 0);
    nonce_ctr++;
    uint32_t ctr = 1;
    while (n) {
        chacha20_block(key, ctr++, nonce, blk);
        size_t k = n < 64 ? n : 64;
        for (size_t i = 0; i < k; i++) o[i] = blk[i];
        o += k; n -= k;
    }
    chacha20_block(key, 0, nonce, blk);                 /* блок 0 - новый ключ: выданное назад не восстановить */
    for (int i = 0; i < 32; i++) key[i] = blk[i];
    int s = strong;
    spin_unlock_irqrestore(&lk, f);
    crypto_wipe(blk, sizeof blk);
    return s;
}

const char* rng_sources(void) { return src[0] ? src : "not started"; }
