#include "bootsig.h"
#include "pk.h"
#include "sha256.h"

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put(uint8_t* d, const void* s, size_t n) { const uint8_t* x = (const uint8_t*)s; for (size_t i = 0; i < n; i++) d[i] = x[i]; }
static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

size_t bootsig_message(const bootsig_t* s, uint8_t* out, size_t cap) {
    if (!s || !out || s->count == 0 || s->count > BOOTSIG_MAX_ENTRIES) return 0;
    size_t n = BOOTSIG_DOMAIN_LEN + BOOTSIG_HEADER_LEN + (size_t)s->count * BOOTSIG_ENTRY_LEN;
    if (cap < n) return 0;
    for (size_t i = 0; i < n; i++) out[i] = 0;
    put(out, BOOTSIG_DOMAIN, BOOTSIG_DOMAIN_LEN);
    uint8_t* b = out + BOOTSIG_DOMAIN_LEN;
    put(b, BOOTSIG_MAGIC, 8);
    wr32(b + 8, s->image_version);
    wr32(b + 16, s->count);
    for (uint32_t i = 0; i < s->count; i++) {
        uint8_t* e = b + BOOTSIG_HEADER_LEN + (size_t)i * BOOTSIG_ENTRY_LEN;
        for (int k = 0; k < 16; k++) e[k] = (uint8_t)s->entry[i].name[k];
        wr64(e + 16, s->entry[i].size);
        put(e + 24, s->entry[i].sha256, 32);
    }
    return n;
}

int bootsig_parse_verify(const uint8_t* blob, size_t len, const uint8_t pub[32], uint32_t min_version, bootsig_t* out) {
    if (!out) return BOOTSIG_E_ARGS;
    uint8_t* z = (uint8_t*)out;
    for (size_t i = 0; i < sizeof(*out); i++) z[i] = 0;
    if (!blob || !pub) return BOOTSIG_E_ARGS;
    if (len < BOOTSIG_HEADER_LEN + BOOTSIG_ENTRY_LEN + BOOTSIG_SIG_LEN || len > BOOTSIG_MAX_LEN) return BOOTSIG_E_FORMAT;
    for (int i = 0; i < 8; i++) if (blob[i] != (uint8_t)BOOTSIG_MAGIC[i]) return BOOTSIG_E_FORMAT;
    uint32_t count = rd32(blob + 16);
    if (count == 0 || count > BOOTSIG_MAX_ENTRIES) return BOOTSIG_E_FORMAT;
    if (len != BOOTSIG_HEADER_LEN + (size_t)count * BOOTSIG_ENTRY_LEN + BOOTSIG_SIG_LEN) return BOOTSIG_E_FORMAT;

    /* Подпись - раньше всего остального: до неё ни одному полю файла нельзя верить, в том числе флагам и резерву. */
    uint8_t msg[BOOTSIG_DOMAIN_LEN + BOOTSIG_MAX_LEN];
    size_t body = len - BOOTSIG_SIG_LEN;
    put(msg, BOOTSIG_DOMAIN, BOOTSIG_DOMAIN_LEN);
    put(msg + BOOTSIG_DOMAIN_LEN, blob, body);
    if (ed25519_verify(pub, msg, BOOTSIG_DOMAIN_LEN + body, blob + body) != 0) return BOOTSIG_E_SIGNATURE;

    if (rd32(blob + 12) != 0) return BOOTSIG_E_FORMAT;                       /* неизвестные флаги */
    for (int i = 20; i < 32; i++) if (blob[i]) return BOOTSIG_E_FORMAT;
    bootsig_t t;
    z = (uint8_t*)&t;
    for (size_t i = 0; i < sizeof(t); i++) z[i] = 0;
    t.image_version = rd32(blob + 8);
    t.count = count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = blob + BOOTSIG_HEADER_LEN + (size_t)i * BOOTSIG_ENTRY_LEN;
        int end = -1;
        for (int k = 0; k < 16; k++) {
            if (end < 0 && e[k] == 0) end = k;
            else if (end >= 0 && e[k] != 0) return BOOTSIG_E_FORMAT;         /* после нуля имя обязано кончаться нулями */
            else if (end < 0 && (e[k] < 0x21 || e[k] > 0x7e)) return BOOTSIG_E_FORMAT;
            t.entry[i].name[k] = (char)e[k];
        }
        if (end <= 0) return BOOTSIG_E_FORMAT;                               /* пустое имя или без завершающего нуля */
        t.entry[i].size = rd64(e + 16);
        put(t.entry[i].sha256, e + 24, 32);
        for (uint32_t j = 0; j < i; j++) if (same(t.entry[j].name, t.entry[i].name)) return BOOTSIG_E_FORMAT;
    }
    if (!same(t.entry[0].name, BOOTSIG_NAME_KERNEL)) return BOOTSIG_E_FORMAT;   /* запись 0 - ядро */
    if (t.image_version < min_version) return BOOTSIG_E_ROLLBACK;
    *out = t;
    return BOOTSIG_OK;
}

const bootsig_entry_t* bootsig_find(const bootsig_t* s, const char* name) {
    if (!s || !name) return 0;
    for (uint32_t i = 0; i < s->count && i < BOOTSIG_MAX_ENTRIES; i++)
        if (same(s->entry[i].name, name)) return &s->entry[i];
    return 0;
}

int bootsig_check_file(const bootsig_t* s, const char* name, const void* data, size_t len) {
    if (!s || !name || (!data && len)) return BOOTSIG_E_ARGS;
    const bootsig_entry_t* e = bootsig_find(s, name);
    if (!e) return BOOTSIG_E_UNLISTED;
    if (e->size != (uint64_t)len) return BOOTSIG_E_SIZE;
    uint8_t h[32];
    sha256(data, len, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ e->sha256[i]);
    return diff ? BOOTSIG_E_HASH : BOOTSIG_OK;
}

const char* bootsig_strerror(int rc) {
    switch (rc) {
    case BOOTSIG_OK:          return "signature and hashes are valid";
    case BOOTSIG_E_ARGS:      return "internal error: bad arguments";
    case BOOTSIG_E_FORMAT:    return "manifest KIRA.SIG is malformed";
    case BOOTSIG_E_SIGNATURE: return "manifest signature does not match the embedded release key";
    case BOOTSIG_E_ROLLBACK:  return "image version is older than the allowed minimum (rollback refused)";
    case BOOTSIG_E_UNLISTED:  return "file is not listed in the signed manifest";
    case BOOTSIG_E_SIZE:      return "file size differs from the signed manifest";
    case BOOTSIG_E_HASH:      return "file was modified: SHA-256 differs from the signed manifest";
    default:                  return "unknown error";
    }
}
