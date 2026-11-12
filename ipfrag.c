/* Сборка фрагментов IP: см. ipfrag.h. Принятые данные лежат прямо в буфере датаграммы на своих местах, карта блоков по
   8 байт говорит, что уже есть: перекрытие находится проверкой битов, полнота - счётчиком блоков. */
#include "ipfrag.h"

static void zero(void* p, uint32_t n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }
static void cpy(void* d, const void* s, uint32_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static int same(const uint8_t* a, const uint8_t* b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }

static int key_eq(const ipf_key_t* a, const ipf_key_t* b) {
    return a->fam == b->fam && a->id == b->id && a->proto == b->proto && same(a->src, b->src, 16) && same(a->dst, b->dst, 16);
}
static int bit(const uint8_t* m, uint32_t b) { return m[b >> 3] >> (b & 7) & 1; }
static void setbit(uint8_t* m, uint32_t b) { m[b >> 3] = (uint8_t)(m[b >> 3] | 1u << (b & 7)); }

void ipfrag_init(ipfrag_t* t, uint64_t ttl_ticks, uint64_t min_age_ticks) {
    zero(t, sizeof *t);
    t->ttl = ttl_ticks; t->min_age = min_age_ticks;
}

void ipfrag_expire(ipfrag_t* t, uint64_t now) {
    for (int i = 0; i < IPF_SLOTS; i++)
        if (t->slot[i].used && now - t->slot[i].born >= t->ttl) { t->slot[i].used = 0; t->expired++; }
}

unsigned ipfrag_pending(const ipfrag_t* t) {
    unsigned n = 0;
    for (int i = 0; i < IPF_SLOTS; i++) n += (unsigned)t->slot[i].used;
    return n;
}

static void drop(ipf_slot_t* s) { if (s) s->used = 0; }

int ipfrag_add(ipfrag_t* t, uint64_t now, const ipf_key_t* k, uint32_t off, int more,
               const uint8_t* d, uint32_t n, int aux, ipf_out_t* out) {
    /* Public parser boundary: reject absent state/input/output before touching
       caller memory. A zero-length fragment is malformed, not a valid null data. */
    if (!t || !k || !out || (!d && n)) return -1;
    ipfrag_expire(t, now);
    ipf_slot_t* s = 0;
    for (int i = 0; i < IPF_SLOTS && !s; i++) if (t->slot[i].used && key_eq(&t->slot[i].key, k)) s = &t->slot[i];

    /* Негодный фрагмент губит и собираемую датаграмму: иначе чужой мусор можно подмешать к настоящей. */
    if (n == 0 || (off & 7) || (more && (n & 7))) { t->malformed++; drop(s); return -1; }
    if (off > IPF_MAX || n > IPF_MAX - off) { t->toobig++; drop(s); return -1; }

    if (!s) {                                                       /* новая датаграмма: найти место */
        int per = 0;
        ipf_slot_t* oldest = 0;
        for (int i = 0; i < IPF_SLOTS; i++) {
            ipf_slot_t* c = &t->slot[i];
            if (!c->used) { if (!s) s = c; continue; }
            if (c->key.fam == k->fam && same(c->key.src, k->src, 16)) per++;
            if (!oldest || c->born < oldest->born) oldest = c;
        }
        if (per >= IPF_PER_SRC) { t->nospace++; return -1; }       /* один отправитель не занимает всё */
        if (!s) {
            if (oldest && now - oldest->born >= t->min_age) { s = oldest; t->expired++; }
            else { t->nospace++; return -1; }
        }
        s->used = 1; s->key = *k; s->born = now; s->total = 0; s->blocks = 0; s->maxend = 0; s->aux = 0;
        zero(s->map, sizeof s->map); zero(s->first, sizeof s->first); zero(s->last, sizeof s->last);
    }

    uint32_t end = off + n, b0 = off >> 3, b1 = (end - 1) >> 3;
    if (!more) {                                                    /* последний фрагмент: длина всей датаграммы */
        if ((s->total && s->total != end) || s->maxend > end) { t->malformed++; drop(s); return -1; }
        s->total = end;
    } else if (s->total && end > s->total) { t->malformed++; drop(s); return -1; }

    uint32_t have = 0, starts = 0;
    for (uint32_t b = b0; b <= b1; b++) { have += (uint32_t)bit(s->map, b); if (b > b0) starts += (uint32_t)bit(s->first, b); }
    if (have) {
        /* терпим только точный повтор: тот же первый и последний блок, внутри нет других начал, байты те же */
        if (have == b1 - b0 + 1 && bit(s->first, b0) && bit(s->last, b1) && !starts && same(s->data + off, d, n)) { t->dup++; return 0; }
        t->overlap++; drop(s); return -1;                           /* перекрытие: вся датаграмма отброшена */
    }

    if (off == 0) s->aux = aux;
    cpy(s->data + off, d, n);
    for (uint32_t b = b0; b <= b1; b++) setbit(s->map, b);
    setbit(s->first, b0); setbit(s->last, b1);
    s->blocks += b1 - b0 + 1;
    if (end > s->maxend) s->maxend = end;

    if (s->total && s->blocks == (s->total + 7) / 8) {              /* всё на месте */
        out->data = s->data; out->len = s->total; out->aux = s->aux;
        s->used = 0; t->assembled++;
        return 1;
    }
    return 0;
}
