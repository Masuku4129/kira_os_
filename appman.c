#include "appman.h"

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

appman_mode_t appman_mode(int trusted, int dev_flag) {
    return (trusted || dev_flag) ? APPMAN_SIGNED : APPMAN_DEV;
}

int appman_parse(const uint8_t* blob, size_t len, uint32_t* serial, uint32_t* count) {
    if (!blob || len < APPMAN_HEADER + APPMAN_ENTRY || len > APPMAN_MAX_LEN) return APPMAN_E_FORMAT;
    for (int i = 0; i < 8; i++) if (blob[i] != (uint8_t)APPMAN_MAGIC[i]) return APPMAN_E_FORMAT;
    uint32_t n = rd32(blob + 12);
    if (n == 0 || n > APPMAN_MAX || len != APPMAN_HEADER + (size_t)n * APPMAN_ENTRY) return APPMAN_E_FORMAT;
    if (rd32(blob + 16) != 0) return APPMAN_E_FORMAT;                       /* неизвестные флаги нельзя игнорировать */
    for (int i = 20; i < 32; i++) if (blob[i]) return APPMAN_E_FORMAT;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = blob + APPMAN_HEADER + (size_t)i * APPMAN_ENTRY;
        int end = -1;
        for (unsigned k = 0; k < APPMAN_NAME_LEN; k++) {
            if (end < 0 && e[k] == 0) end = (int)k;
            else if (end >= 0 && e[k] != 0) return APPMAN_E_FORMAT;           /* после нуля имя обязано кончаться нулями */
            else if (end < 0 && (e[k] < 0x21 || e[k] > 0x7e)) return APPMAN_E_FORMAT;
        }
        if (end <= 0) return APPMAN_E_FORMAT;                                 /* пустое имя или без завершающего нуля */
        for (uint32_t j = 0; j < i; j++) {                                    /* имена не повторяются: иначе «первое совпадение» зависело бы от порядка */
            const uint8_t* f = blob + APPMAN_HEADER + (size_t)j * APPMAN_ENTRY;
            if (same((const char*)e, (const char*)f)) return APPMAN_E_FORMAT;
        }
    }
    if (serial) *serial = rd32(blob + 8);
    if (count) *count = n;
    return APPMAN_OK;
}

trust_verdict_t appman_check(const uint8_t* blob, size_t len, const char* name, const uint8_t digest[32]) {
    uint32_t n;
    if (!name || !digest || appman_parse(blob, len, 0, &n) != APPMAN_OK) return TRUST_UNKNOWN;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = blob + APPMAN_HEADER + (size_t)i * APPMAN_ENTRY;
        if (!same((const char*)e, name)) continue;
        uint8_t diff = 0;                                   /* без раннего выхода: время не зависит от места расхождения */
        for (unsigned k = 0; k < 32; k++) diff |= (uint8_t)(e[APPMAN_NAME_LEN + k] ^ digest[k]);
        return diff ? TRUST_TAMPERED : TRUST_OK;
    }
    return TRUST_UNKNOWN;
}

size_t appman_build(uint32_t serial, const trust_entry_t* entries, unsigned n, uint8_t* out, size_t cap) {
    if (!entries || !out || n == 0 || n > APPMAN_MAX) return 0;
    size_t total = APPMAN_HEADER + (size_t)n * APPMAN_ENTRY;
    if (cap < total) return 0;
    for (size_t i = 0; i < total; i++) out[i] = 0;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)APPMAN_MAGIC[i];
    wr32(out + 8, serial);
    wr32(out + 12, n);
    for (unsigned i = 0; i < n; i++) {
        uint8_t* e = out + APPMAN_HEADER + (size_t)i * APPMAN_ENTRY;
        unsigned k = 0;
        while (entries[i].name[k]) { if (k + 1 >= APPMAN_NAME_LEN) return 0; e[k] = (uint8_t)entries[i].name[k]; k++; }
        for (unsigned q = 0; q < 32; q++) e[APPMAN_NAME_LEN + q] = entries[i].sha[q];
    }
    return appman_parse(out, total, 0, 0) == APPMAN_OK ? total : 0;
}

const char* appman_strerror(int rc) {
    switch (rc) {
    case APPMAN_OK:       return "ok";
    case APPMAN_E_ARGS:   return "bad arguments";
    case APPMAN_E_FORMAT: return "manifest format is invalid";
    default:              return "unknown error";
    }
}
