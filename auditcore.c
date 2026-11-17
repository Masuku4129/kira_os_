/* Журнал аудита: формат, цепочка, контрольные точки, проверка. Чистый код без ядра (см. auditcore.h и docs/AUDIT.md). */
#include "auditcore.h"
#include "sha256.h"
#include "pk.h"
#include "crypto.h"

static const char CKPT_DOMAIN[16] = { 'K','I','R','A','-','A','U','D','I','T','-','C','K','P','T','1' };   /* 16 байт: отделяет подпись точки от любой другой подписи ключа */

static void mcpy(void* d, const void* s, size_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; }
static void mzero(void* d, size_t n) { uint8_t* a = d; while (n--) *a++ = 0; }
static int  meq(const void* x, const void* y, size_t n) { return crypto_eq(x, y, n); }
static int  is_zero(const void* x, size_t n) { const uint8_t* a = x; uint8_t o = 0; while (n--) o |= *a++; return o == 0; }

const char* aud_kind_name(unsigned k) {
    static const char* const n[AUD_K_MAX] = { "?", "boot", "ckpt", "loss", "prune", "key", "exec", "cap-deny", "drv-install", "drv-remove",
        "rcon-passwd", "rcon-login", "mount", "config", "trust", "sleep", "wake", "panic", "watchdog", "immune", "time", "shutdown", "tamper", "rcon-cmd" };
    return k < AUD_K_MAX ? n[k] : "?";
}
const char* aud_src_name(unsigned s) { static const char* const n[AUD_SRC_MAX] = { "kernel", "console", "rcon", "program", "system" }; return s < AUD_SRC_MAX ? n[s] : "?"; }
const char* aud_res_name(unsigned r) { static const char* const n[AUD_R_MAX] = { "OK", "DENIED", "FAIL" }; return r < AUD_R_MAX ? n[r] : "?"; }

void aud_hash(const aud_rec_t* r, uint8_t out[32]) {
    if (r->kind == AUD_K_CKPT) {
        aud_rec_t t; mcpy(&t, r, sizeof t);
        mzero(((aud_ckpt_t*)&t)->sig, 64);                                   /* подпись в хэш цепочки не входит */
        sha256(&t, sizeof t, out);
    } else sha256(r, sizeof *r, out);
}
void aud_keyid(const uint8_t pub[32], uint8_t id[16]) { uint8_t h[32]; sha256(pub, 32, h); mcpy(id, h, 16); }

void aud_chain_init(aud_chain_t* c, uint64_t next_seq, const uint8_t head[32]) {
    c->next_seq = next_seq ? next_seq : 1;
    if (head) mcpy(c->head, head, 32); else mzero(c->head, 32);
}
int aud_next_is_ckpt(const aud_chain_t* c) { return c->next_seq % AUD_SEG_RECS == 0; }
void aud_seal(aud_chain_t* c, aud_rec_t* r) {
    r->seq = c->next_seq;
    if (r->kind == AUD_K_CKPT) ((aud_ckpt_t*)r)->arg = (uint32_t)(r->seq - 1);     /* сколько записей подтверждено: входит в хэш цепочки */
    mcpy(r->prev, c->head, 32);
    aud_hash(r, c->head);
    c->next_seq++;
}
void aud_ckpt_fill(aud_rec_t* r, const uint8_t pub[32]) {
    aud_ckpt_t* k = (aud_ckpt_t*)r;
    k->kind = AUD_K_CKPT; k->src = AUD_SRC_KERNEL; k->res = AUD_R_OK;
    mzero(k->sig, 64);
    aud_keyid(pub, k->keyid);
}
static void ckpt_msg(const aud_rec_t* r, uint8_t msg[48]) { mcpy(msg, CKPT_DOMAIN, 16); aud_hash(r, msg + 16); }
int aud_ckpt_sign(aud_rec_t* r, const uint8_t seed[32]) {
    uint8_t m[48];
    ckpt_msg(r, m);
    return ed25519_sign(seed, m, sizeof m, ((aud_ckpt_t*)r)->sig);
}
int aud_ckpt_check(const aud_rec_t* r, const uint8_t pub[32]) {
    if (r->kind != AUD_K_CKPT) return -1;
    uint8_t m[48], id[16]; ckpt_msg(r, m);
    aud_keyid(pub, id);
    if (!meq(id, ((const aud_ckpt_t*)r)->keyid, 16)) return -1;
    return ed25519_verify(pub, m, sizeof m, ((const aud_ckpt_t*)r)->sig);
}

void aud_hdr_make(aud_hdr_t* h, uint64_t first_seq, const uint8_t pub[32], const aud_rec_t* anchor_ckpt) {
    mzero(h, sizeof *h);
    mcpy(h->magic, "KIRAAUD1", 8);
    h->version = 1; h->rec_size = AUD_REC_SIZE; h->first_seq = first_seq;
    mcpy(h->pub, pub, 32);
    if (anchor_ckpt) { mcpy(h->anchor_ckpt, anchor_ckpt, AUD_REC_SIZE); aud_hash(anchor_ckpt, h->anchor); }
}

const char* aud_verr_name(int c) {
    switch (c) {
    case AUD_V_OK: return "ok";
    case AUD_V_FORMAT: return "формат файла нарушен (обрезан или чужой)";
    case AUD_V_KEY: return "ключ не принадлежит устройству";
    case AUD_V_ANCHOR: return "начало сегмента не подтверждено подписью или не стыкуется";
    case AUD_V_SEQ: return "разрыв, повтор или перестановка номеров (запись удалена, вставлена или переставлена)";
    case AUD_V_CHAIN: return "цепочка хэшей разорвана (запись изменена, подменена или удалена)";
    case AUD_V_SIG: return "подпись контрольной точки неверна";
    case AUD_V_TRUNC: return "журнал короче подтверждённого подписью (хвост отрезан)";
    case AUD_V_PRUNE: return "начало журнала удалено без записи prune";
    default: return "?";
    }
}

void aud_vstate_init(aud_vstate_t* v) { mzero(v, sizeof *v); v->verify_sigs = 1; }
void aud_vstate_want(aud_vstate_t* v, const aud_rec_t* ckpt) { v->want_seq = ckpt->seq; aud_hash(ckpt, v->want_hash); }

static void fail(aud_vstate_t* v, int code, uint64_t seq, uint64_t off) {
    if (v->code != AUD_V_OK) return;
    v->code = code; v->bad_seq = seq; v->bad_off = off; v->bad_file = v->files - 1;
}

int aud_verify_file(aud_vstate_t* v, const uint8_t* buf, size_t len, const uint8_t pub[32]) {
    v->files++;
    if (len < AUD_HDR_SIZE) { fail(v, AUD_V_FORMAT, v->next_seq, 0); return v->code; }
    aud_hdr_t h; mcpy(&h, buf, sizeof h);
    if (!meq(h.magic, "KIRAAUD1", 8) || h.version != 1 || h.rec_size != AUD_REC_SIZE || h.first_seq == 0) { fail(v, AUD_V_FORMAT, v->next_seq, 0); return v->code; }
    if (!meq(h.pub, pub, 32)) { fail(v, AUD_V_KEY, h.first_seq, 0); }
    size_t body = len - AUD_HDR_SIZE, n = body / AUD_REC_SIZE;
    if (body % AUD_REC_SIZE) fail(v, AUD_V_FORMAT, h.first_seq + n, AUD_HDR_SIZE + n * AUD_REC_SIZE);   /* обрезанная запись: проверяем целые */
    /* Начало сегмента. */
    if (!v->started) {
        v->started = 1; v->first_seq = h.first_seq; v->next_seq = h.first_seq;
        if (h.first_seq == 1) {
            if (!is_zero(h.anchor, 32) || !is_zero(h.anchor_ckpt, AUD_REC_SIZE)) fail(v, AUD_V_ANCHOR, 1, 0);
            mzero(v->head, 32);
        } else {
            const aud_rec_t* a = (const aud_rec_t*)h.anchor_ckpt;
            uint8_t ah[32]; aud_hash(a, ah);
            if (a->kind != AUD_K_CKPT || a->seq + 1 != h.first_seq || !meq(ah, h.anchor, 32) || aud_ckpt_check(a, pub) != 0) fail(v, AUD_V_ANCHOR, h.first_seq, 0);
            mcpy(v->head, h.anchor, 32);
            v->last_ckpt_seq = a->seq;
        }
    } else if (h.first_seq != v->next_seq || !meq(h.anchor, v->head, 32)) {
        fail(v, h.first_seq != v->next_seq ? AUD_V_SEQ : AUD_V_ANCHOR, h.first_seq, 0);
        v->next_seq = h.first_seq; mcpy(v->head, h.anchor, 32);       /* продолжаем с заявленного: найдём и остальное */
    }
    for (size_t i = 0; i < n; i++) {
        aud_rec_t r; mcpy(&r, buf + AUD_HDR_SIZE + i * AUD_REC_SIZE, sizeof r);
        uint64_t off = AUD_HDR_SIZE + i * AUD_REC_SIZE;
        if (r.seq != v->next_seq) { fail(v, AUD_V_SEQ, v->next_seq, off); v->next_seq = r.seq; }
        else if (!meq(r.prev, v->head, 32)) fail(v, AUD_V_CHAIN, r.seq, off);
        if (r.seq % AUD_SEG_RECS == 0 && r.kind != AUD_K_CKPT) fail(v, AUD_V_SEQ, r.seq, off);       /* на границе сегмента обязана стоять точка */
        if (r.kind == AUD_K_CKPT) {
            if (v->verify_sigs && r.seq >= v->sig_from) {
                if (aud_ckpt_check(&r, pub) != 0) fail(v, AUD_V_SIG, r.seq, off);
                else v->ckpts++;
            }
            v->last_ckpt_seq = r.seq;
        } else if (r.kind == AUD_K_PRUNE && r.arg > v->prune_upto) v->prune_upto = r.arg;
        aud_hash(&r, v->head);
        if (v->want_seq && r.seq == v->want_seq) { v->want_seen = 1; if (!meq(v->head, v->want_hash, 32)) fail(v, AUD_V_CHAIN, r.seq, off); }
        v->next_seq = r.seq + 1;
        v->count++;
    }
    return v->code;
}

int aud_verify_end(aud_vstate_t* v) {
    if (v->started && v->first_seq > 1 && v->prune_upto + 1 < v->first_seq) fail(v, AUD_V_PRUNE, v->first_seq, 0);
    if (v->want_seq && v->want_seq >= v->first_seq && v->next_seq <= v->want_seq) fail(v, AUD_V_TRUNC, v->next_seq, 0);
    return v->code;
}

int aud_verify_range(const aud_rec_t* r, size_t n, const uint8_t pub[32], int check_sigs, uint64_t* bad) {
    uint8_t h[32];
    for (size_t i = 0; i < n; i++) {
        if (i && r[i].seq != r[i - 1].seq + 1) { if (bad) *bad = r[i].seq; return AUD_V_SEQ; }
        if (i) { aud_hash(&r[i - 1], h); if (!meq(h, r[i].prev, 32)) { if (bad) *bad = r[i].seq; return AUD_V_CHAIN; } }
        if (check_sigs && r[i].kind == AUD_K_CKPT && aud_ckpt_check(&r[i], pub) != 0) { if (bad) *bad = r[i].seq; return AUD_V_SIG; }
    }
    return AUD_V_OK;
}

static size_t put_s(char* o, size_t cap, size_t n, const char* s) { while (*s && n + 1 < cap) o[n++] = *s++; return n; }
static size_t put_u(char* o, size_t cap, size_t n, uint64_t v) { char t[24]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k && n + 1 < cap) o[n++] = t[--k]; return n; }
static size_t put_2(char* o, size_t cap, size_t n, unsigned v) { return put_u(o, cap, put_s(o, cap, n, v < 10 ? "0" : ""), v); }

size_t aud_format(const aud_rec_t* r, char* o, size_t cap) {
    size_t n = 0;
    n = put_s(o, cap, n, "#"); n = put_u(o, cap, n, r->seq);
    n = put_s(o, cap, n, " b"); n = put_u(o, cap, n, r->boot);
    n = put_s(o, cap, n, " +"); n = put_u(o, cap, n, r->mono_ms); n = put_s(o, cap, n, "ms ");
    if (r->real_s) {                                              /* дата UTC без зависимостей от rtc.c */
        uint64_t s = r->real_s, d = s / 86400, t = s % 86400;
        int64_t z = (int64_t)d + 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; uint64_t doe = (uint64_t)(z - era * 146097);
        uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t y = (int64_t)yoe + era * 400;
        uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); uint64_t mp = (5 * doy + 2) / 153;
        unsigned dd = (unsigned)(doy - (153 * mp + 2) / 5 + 1), mm = (unsigned)(mp < 10 ? mp + 3 : mp - 9); if (mm <= 2) y++;
        n = put_u(o, cap, n, (uint64_t)y); n = put_s(o, cap, n, "-"); n = put_2(o, cap, n, mm); n = put_s(o, cap, n, "-"); n = put_2(o, cap, n, dd);
        n = put_s(o, cap, n, "T"); n = put_2(o, cap, n, (unsigned)(t / 3600)); n = put_s(o, cap, n, ":"); n = put_2(o, cap, n, (unsigned)(t / 60 % 60));
        n = put_s(o, cap, n, ":"); n = put_2(o, cap, n, (unsigned)(t % 60)); n = put_s(o, cap, n, "Z ");
    } else n = put_s(o, cap, n, "no-rtc ");
    n = put_s(o, cap, n, aud_kind_name(r->kind)); n = put_s(o, cap, n, " "); n = put_s(o, cap, n, aud_src_name(r->src));
    n = put_s(o, cap, n, " "); n = put_s(o, cap, n, aud_res_name(r->res));
    n = put_s(o, cap, n, " pid="); n = put_u(o, cap, n, r->pid); n = put_s(o, cap, n, " dom="); n = put_u(o, cap, n, r->domain);
    n = put_s(o, cap, n, " arg="); n = put_u(o, cap, n, r->arg);
    if (r->kind != AUD_K_CKPT) {
        char s[25], d[57]; mcpy(s, r->subject, 24); s[24] = 0; mcpy(d, r->detail, 56); d[56] = 0;
        for (int i = 0; s[i]; i++) if ((unsigned char)s[i] < 0x20) s[i] = '?';
        for (int i = 0; d[i]; i++) if ((unsigned char)d[i] < 0x20) d[i] = '?';
        if (s[0]) { n = put_s(o, cap, n, " "); n = put_s(o, cap, n, s); }
        if (d[0]) { n = put_s(o, cap, n, " | "); n = put_s(o, cap, n, d); }
    }
    if (n < cap) o[n] = 0;
    return n;
}
