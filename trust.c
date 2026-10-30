#include "trust.h"

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

trust_verdict_t trust_check(const trust_entry_t* tbl, unsigned n, const char* name, const uint8_t digest[32]) {
    for (unsigned i = 0; i < n; i++) {
        if (!same(tbl[i].name, name)) continue;
        uint8_t diff = 0;                                   /* без раннего выхода: время не зависит от места расхождения */
        for (unsigned k = 0; k < 32; k++) diff |= (uint8_t)(tbl[i].sha[k] ^ digest[k]);
        return diff ? TRUST_TAMPERED : TRUST_OK;
    }
    return TRUST_UNKNOWN;
}
