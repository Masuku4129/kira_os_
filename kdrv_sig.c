#include "kdrv_sig.h"
#include "pk.h"
#include "immune.h"

static int equal_bytes(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static int all_zero(const uint8_t* p, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; i++) v |= p[i];
    return v == 0;
}

static void put_le64(uint8_t* p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

int kdrv_sig_header_check(const void* signature, size_t signature_len) {
    if (!signature) return KDRV_SIG_E_ARGUMENT;
    if (signature_len != sizeof(kdrv_sig_t)) return KDRV_SIG_E_SIZE;
    const kdrv_sig_t* s = (const kdrv_sig_t*)signature;
    if (s->magic != KDRV_SIG_MAGIC || s->format != KDRV_SIG_FORMAT || s->size != sizeof(kdrv_sig_t))
        return KDRV_SIG_E_FORMAT;
    if (all_zero(s->key_id, sizeof s->key_id) || s->package_size == 0)
        return KDRV_SIG_E_FORMAT;
    if (s->not_before == 0 || s->not_after <= s->not_before || s->not_after > 0x7FFFFFFFFFFFFFFFULL)
        return KDRV_SIG_E_TIME;
    return KDRV_SIG_OK;
}

int kdrv_sig_verify(const void* package, size_t package_len,
                    const void* signature, size_t signature_len,
                    const kdrv_sig_key_t* keys, uint32_t key_count,
                    int64_t now_utc) {
    int rc = kdrv_sig_header_check(signature, signature_len);
    if (rc != KDRV_SIG_OK) return rc;
    if (!package || package_len == 0 || !keys || key_count == 0 || key_count > KDRV_SIG_MAX_KEYS)
        return KDRV_SIG_E_ARGUMENT;

    const kdrv_sig_t* s = (const kdrv_sig_t*)signature;
    if (s->package_size != (uint64_t)package_len) return KDRV_SIG_E_PACKAGE_SIZE;
    if (now_utc <= 0) return KDRV_SIG_E_TIME;
    if ((uint64_t)now_utc < s->not_before) return KDRV_SIG_E_NOT_YET;
    if ((uint64_t)now_utc >= s->not_after) return KDRV_SIG_E_EXPIRED;

    uint8_t digest[32];
    sha256(package, package_len, digest);
    if (!equal_bytes(digest, s->package_sha256, sizeof digest)) return KDRV_SIG_E_HASH;

    const kdrv_sig_key_t* found = 0;
    for (uint32_t i = 0; i < key_count; i++) {
        if (keys[i].flags & ~KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_TRUST_STORE;
        for (uint32_t j = 0; j < i; j++)
            if (equal_bytes(keys[i].id, keys[j].id, KDRV_SIG_KEY_ID_SIZE))
                return KDRV_SIG_E_TRUST_STORE;
        if (equal_bytes(keys[i].id, s->key_id, KDRV_SIG_KEY_ID_SIZE)) found = &keys[i];
    }
    if (!found) return KDRV_SIG_E_UNKNOWN_KEY;
    if (found->flags & KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_REVOKED;
    if (found->not_after && (uint64_t)now_utc >= found->not_after) return KDRV_SIG_E_KEY_EXPIRED;
    if (all_zero(found->public_key, sizeof found->public_key)) return KDRV_SIG_E_TRUST_STORE;

    static const uint8_t domain[16] = {
        'K','I','R','A','-','K','D','R','V','-','S','I','G','-','V','1'
    };
    uint8_t message[88];
    for (unsigned i = 0; i < sizeof domain; i++) message[i] = domain[i];
    for (unsigned i = 0; i < KDRV_SIG_KEY_ID_SIZE; i++) message[16 + i] = s->key_id[i];
    for (unsigned i = 0; i < 32; i++) message[32 + i] = s->package_sha256[i];
    put_le64(message + 64, s->package_size);
    put_le64(message + 72, s->not_before);
    put_le64(message + 80, s->not_after);

    return ed25519_verify(found->public_key, message, sizeof message, s->signature) == 0
        ? KDRV_SIG_OK : KDRV_SIG_E_SIGNATURE;
}

const char* kdrv_sig_strerror(int e) {
    switch (e) {
    case KDRV_SIG_OK: return "подпись подтверждена";
    case KDRV_SIG_E_ARGUMENT: return "неверные аргументы проверки";
    case KDRV_SIG_E_SIZE: return "неверный размер файла подписи";
    case KDRV_SIG_E_FORMAT: return "повреждённый формат подписи";
    case KDRV_SIG_E_TIME: return "нет достоверного времени или неверный интервал";
    case KDRV_SIG_E_NOT_YET: return "срок действия подписи ещё не начался";
    case KDRV_SIG_E_EXPIRED: return "срок действия подписи истёк";
    case KDRV_SIG_E_PACKAGE_SIZE: return "размер пакета не совпал с манифестом";
    case KDRV_SIG_E_HASH: return "хеш пакета не совпал с манифестом";
    case KDRV_SIG_E_UNKNOWN_KEY: return "неизвестный издательский ключ";
    case KDRV_SIG_E_REVOKED: return "издательский ключ отозван";
    case KDRV_SIG_E_TRUST_STORE: return "ошибка доверенного хранилища ключей";
    case KDRV_SIG_E_SIGNATURE: return "Ed25519 подпись недействительна";
    case KDRV_SIG_E_IO: return "ошибка чтения подписи";
    case KDRV_SIG_E_MISSING: return "обязательная издательская подпись отсутствует";
    case KDRV_SIG_E_KEY_EXPIRED: return "срок действия издательского ключа истёк (ключ заменён)";
    case KDRV_SIG_E_CLOCK: return "часы ушли назад больше допуска: срок подписи проверить нельзя";
    default: return "неизвестная ошибка подписи";
    }
}
