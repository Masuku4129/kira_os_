/* Сборка фрагментов IP на хосте: любой порядок, повторы, перекрытия, лишние и негодные фрагменты, переполнение,
   срок жизни, пределы памяти и числа датаграмм от одного отправителя, IPv4 и IPv6 рядом. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../ipfrag.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static ipfrag_t T;                         /* большой: на стеке не держим */
static uint8_t msg[IPF_MAX];

static ipf_key_t key4(uint32_t src, uint16_t id) {
    ipf_key_t k; memset(&k, 0, sizeof k);
    k.fam = 4; k.src[0] = (uint8_t)(src >> 24); k.src[1] = (uint8_t)(src >> 16); k.src[2] = (uint8_t)(src >> 8); k.src[3] = (uint8_t)src;
    k.dst[0] = 10; k.dst[3] = 15; k.id = id; k.proto = 17;
    return k;
}
static void fill(uint32_t n) { for (uint32_t i = 0; i < n; i++) msg[i] = (uint8_t)(i * 31 + (i >> 8) * 7 + 5); }

typedef struct { uint32_t off, len; int more; } frag_t;
static int add(const ipf_key_t* k, uint64_t now, frag_t f, ipf_out_t* o) {
    return ipfrag_add(&T, now, k, f.off, f.more, msg + f.off, f.len, f.off == 0 ? 17 : -1, o);
}
/* нарезать datagram длиной total на куски по step (кроме последнего) */
static int cut(uint32_t total, uint32_t step, frag_t* f) {
    int n = 0;
    for (uint32_t o = 0; o < total; o += step) { f[n].off = o; f[n].len = total - o < step ? total - o : step; f[n].more = o + step < total; n++; }
    return n;
}

static int perm_done, perm_bad;
static void permute(const ipf_key_t* k, frag_t* f, int n, int i, uint32_t total) {
    if (i == n) {
        ipf_out_t o; int r = 0, last = 0;
        ipfrag_init(&T, 1000, 10);
        for (int j = 0; j < n; j++) { r = add(k, 5, f[j], &o); last = j; if (r == 1) break; }
        perm_done++;
        if (r != 1 || last != n - 1 || o.len != total || memcmp(o.data, msg, total) || o.aux != 17 || ipfrag_pending(&T) != 0) perm_bad++;
        return;
    }
    for (int j = i; j < n; j++) {
        frag_t t = f[i]; f[i] = f[j]; f[j] = t;
        permute(k, f, n, i + 1, total);
        t = f[i]; f[i] = f[j]; f[j] = t;
    }
}

static void test_orders(void) {
    ipf_key_t k = key4(0xC0A80001, 7);
    /* все 720 порядков шести фрагментов, последний размер не кратен 8 */
    fill(5 * 24 + 13);
    frag_t f[16]; int n = cut(5 * 24 + 13, 24, f);
    CHECK(n == 6);
    permute(&k, f, n, 0, 5 * 24 + 13);
    CHECK(perm_done == 720 && perm_bad == 0);
    /* 5 фрагментов ровно по 8 байт и длина кратна 8 */
    perm_done = perm_bad = 0;
    fill(40); n = cut(40, 8, f); permute(&k, f, n, 0, 40);
    CHECK(perm_done == 120 && perm_bad == 0);
    /* один «фрагмент»: первый и последний сразу (MF=0, смещение 0) тоже собирается */
    ipfrag_init(&T, 1000, 10);
    ipf_out_t o; frag_t one = { 0, 33, 0 };
    fill(33);
    CHECK(add(&k, 1, one, &o) == 1 && o.len == 33 && !memcmp(o.data, msg, 33));
    /* самая большая датаграмма */
    fill(IPF_MAX);
    ipfrag_init(&T, 1000, 10);
    static frag_t big[IPF_MAX / 1480 + 2];
    n = cut(IPF_MAX, 1480 / 8 * 8, big);
    int r = 0;
    for (int j = n - 1; j >= 0; j--) r = add(&k, 1, big[j], &o);
    CHECK(r == 1 && o.len == IPF_MAX && !memcmp(o.data, msg, IPF_MAX));
}

static void test_dups_overlaps(void) {
    ipf_key_t k = key4(0xC0A80001, 9);
    ipf_out_t o;
    fill(64);
    /* точный повтор игнорируется, сборка продолжается */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 0, 24, 1 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 0, 24, 1 }, &o) == 0 && T.dup == 1);
    CHECK(add(&k, 1, (frag_t){ 48, 16, 0 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 48, 16, 0 }, &o) == 0 && T.dup == 2);
    CHECK(add(&k, 1, (frag_t){ 24, 24, 1 }, &o) == 1 && o.len == 64 && !memcmp(o.data, msg, 64));
    /* после сборки тот же идентификатор начинает новую датаграмму (лишний поздний фрагмент не оживляет старую) */
    CHECK(add(&k, 2, (frag_t){ 24, 24, 1 }, &o) == 0 && ipfrag_pending(&T) == 1);
    /* частичное перекрытие: вся датаграмма отброшена, дальнейшие куски начинают сначала */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 0, 24, 1 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 16, 24, 1 }, &o) == -1 && T.overlap == 1 && ipfrag_pending(&T) == 0);
    CHECK(add(&k, 1, (frag_t){ 40, 24, 0 }, &o) == 0 && ipfrag_pending(&T) == 1);   /* новая датаграмма: одна часть */
    /* то же смещение, другие данные (подмена) - отброс */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 0, 24, 1 }, &o) == 0);
    uint8_t evil[24]; memcpy(evil, msg, 24); evil[3] ^= 0xFF;
    CHECK(ipfrag_add(&T, 1, &k, 0, 1, evil, 24, 17, &o) == -1 && T.overlap == 1);
    /* больший фрагмент покрывает меньший */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 8, 8, 1 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 0, 24, 1 }, &o) == -1 && T.overlap == 1);
    /* последний фрагмент накрывает хвост уже принятого */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 24, 24, 1 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 40, 8, 0 }, &o) == -1 && T.overlap == 1);     /* внутри принятого: не точный повтор */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 0, 16, 1 }, &o) == 0 && add(&k, 1, (frag_t){ 16, 16, 1 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 0, 32, 1 }, &o) == -1 && T.overlap == 1);     /* один кусок поверх двух принятых */
}

static void test_bad(void) {
    ipf_key_t k = key4(0xC0A80001, 11);
    ipf_out_t o;
    fill(256);
    /* не последний фрагмент не кратен 8 - датаграмма отброшена (RFC 791: смещение и длина кратны 8) */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 0, 16, 1 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 16, 13, 1 }, &o) == -1 && T.malformed == 1 && ipfrag_pending(&T) == 0);
    /* нулевая длина */
    CHECK(ipfrag_add(&T, 1, &k, 8, 1, msg, 0, -1, &o) == -1 && T.malformed == 2);
    /* смещение не кратно 8 */
    CHECK(ipfrag_add(&T, 1, &k, 4, 1, msg, 8, -1, &o) == -1 && T.malformed == 3);
    /* два разных «последних» */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 16, 8, 0 }, &o) == 0);                /* длина 24 */
    CHECK(add(&k, 1, (frag_t){ 8, 16, 0 }, &o) == -1 && T.overlap == 1);     /* тот же конец, но кусок накрывает принятый */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 16, 8, 0 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 32, 8, 0 }, &o) == -1 && T.malformed == 1);   /* другая длина (40) */
    /* данные за объявленным концом */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 16, 8, 0 }, &o) == 0);
    CHECK(add(&k, 1, (frag_t){ 24, 8, 1 }, &o) == -1 && T.malformed == 1);
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 32, 8, 1 }, &o) == 0);                /* уже есть данные до 40 */
    CHECK(add(&k, 1, (frag_t){ 16, 8, 0 }, &o) == -1 && T.malformed == 1);   /* а «конец» на 24 */
    /* переполнение: больше IPF_MAX */
    ipfrag_init(&T, 1000, 10);
    CHECK(ipfrag_add(&T, 1, &k, IPF_MAX - 8, 0, msg, 16, 17, &o) == -1 && T.toobig == 1);
    CHECK(ipfrag_add(&T, 1, &k, IPF_MAX + 8, 0, msg, 8, 17, &o) == -1 && T.toobig == 2);
    CHECK(ipfrag_add(&T, 1, &k, 0xFFFFFFF8u, 0, msg, 64, 17, &o) == -1 && T.toobig == 3);   /* смещение + длина не переносится через 2^32 */
    CHECK(ipfrag_add(&T, 1, &k, 0, 0, msg, 65000, 17, &o) == -1 && T.toobig == 4);
    /* у собираемой датаграммы негодный кусок губит всё накопленное */
    ipfrag_init(&T, 1000, 10);
    CHECK(add(&k, 1, (frag_t){ 0, 64, 1 }, &o) == 0 && ipfrag_pending(&T) == 1);
    CHECK(ipfrag_add(&T, 1, &k, IPF_MAX, 1, msg, 8, -1, &o) == -1 && ipfrag_pending(&T) == 0);
    /* ровно IPF_MAX проходит */
    ipfrag_init(&T, 1000, 10);
    CHECK(ipfrag_add(&T, 1, &k, IPF_MAX - 8, 0, msg, 8, 17, &o) == 0 && T.toobig == 0);
}

static void test_limits(void) {
    ipf_out_t o;
    fill(64);
    /* срок жизни */
    ipf_key_t k = key4(0x01020304, 1);
    ipfrag_init(&T, 100, 10);
    CHECK(add(&k, 1000, (frag_t){ 0, 24, 1 }, &o) == 0 && ipfrag_pending(&T) == 1);
    ipfrag_expire(&T, 1099); CHECK(ipfrag_pending(&T) == 1);
    ipfrag_expire(&T, 1100); CHECK(ipfrag_pending(&T) == 0 && T.expired == 1);
    /* остальные куски по истечении срока не сливаются со старым началом */
    CHECK(add(&k, 1000, (frag_t){ 0, 24, 1 }, &o) == 0);
    CHECK(add(&k, 1200, (frag_t){ 24, 40, 0 }, &o) == 0 && T.expired == 2);   /* начало умерло, этот кусок - новая датаграмма */
    /* не больше IPF_PER_SRC от одного отправителя */
    ipfrag_init(&T, 1000, 10);
    for (unsigned i = 0; i < IPF_PER_SRC; i++) { ipf_key_t kk = key4(0x0A0A0A0A, (uint16_t)(100 + i)); CHECK(add(&kk, 1, (frag_t){ 0, 24, 1 }, &o) == 0); }
    { ipf_key_t kk = key4(0x0A0A0A0A, 200); CHECK(add(&kk, 1, (frag_t){ 0, 24, 1 }, &o) == -1 && T.nospace == 1); }
    { ipf_key_t kk = key4(0x0B0B0B0B, 200); CHECK(add(&kk, 1, (frag_t){ 0, 24, 1 }, &o) == 0); }   /* другой отправитель - место есть */
    /* все места заняты: свежих не вытесняем, старше min_age - вытесняем самого старого */
    ipfrag_init(&T, 1000, 50);
    for (unsigned i = 0; i < IPF_SLOTS; i++) { ipf_key_t kk = key4(0x0C000000 + i, 1); CHECK(add(&kk, 10 + i, (frag_t){ 0, 24, 1 }, &o) == 0); }
    CHECK(ipfrag_pending(&T) == IPF_SLOTS);
    { ipf_key_t kk = key4(0x0D000001, 1); CHECK(add(&kk, 30, (frag_t){ 0, 24, 1 }, &o) == -1 && T.nospace == 1); }
    { ipf_key_t kk = key4(0x0D000001, 1); CHECK(add(&kk, 60, (frag_t){ 0, 24, 1 }, &o) == 0 && T.expired == 1); }
    { ipf_key_t kk = key4(0x0C000000, 1); CHECK(add(&kk, 61, (frag_t){ 24, 40, 0 }, &o) == 0); }   /* самого старого (src ...0) больше нет: это уже новая датаграмма либо отказ */
    /* разные ключи не смешиваются: id, адрес, протокол, семейство */
    ipfrag_init(&T, 1000, 10);
    ipf_key_t a = key4(0x01010101, 5), b = key4(0x01010101, 6), c = key4(0x01010102, 5);
    CHECK(add(&a, 1, (frag_t){ 0, 24, 1 }, &o) == 0 && add(&b, 1, (frag_t){ 24, 40, 0 }, &o) == 0 && add(&c, 1, (frag_t){ 24, 40, 0 }, &o) == 0);
    CHECK(ipfrag_pending(&T) == 3 && T.assembled == 0);
    CHECK(add(&a, 1, (frag_t){ 24, 40, 0 }, &o) == 1 && o.len == 64 && !memcmp(o.data, msg, 64));
    ipfrag_init(&T, 1000, 10);
    ipf_key_t e = key4(0x01010101, 5), f = key4(0x01010101, 5);
    f.proto = 6;
    CHECK(add(&e, 1, (frag_t){ 0, 24, 1 }, &o) == 0 && add(&f, 1, (frag_t){ 24, 40, 0 }, &o) == 0 && ipfrag_pending(&T) == 2);
    CHECK(add(&e, 1, (frag_t){ 24, 40, 0 }, &o) == 1 && add(&f, 1, (frag_t){ 0, 24, 1 }, &o) == 1);
}

static void test_v6(void) {
    /* IPv6: идентификатор 32 бита, следующий заголовок из первого фрагмента, семейство отделяет ключи */
    ipf_key_t k; memset(&k, 0, sizeof k);
    k.fam = 6; k.src[0] = 0xFE; k.src[1] = 0x80; k.src[15] = 1; k.dst[0] = 0xFE; k.dst[1] = 0x80; k.dst[15] = 2; k.id = 0xDEADBEEF; k.proto = 44;
    ipf_out_t o;
    fill(200);
    ipfrag_init(&T, 1000, 10);
    CHECK(ipfrag_add(&T, 1, &k, 96, 0, msg + 96, 104, -1, &o) == 0);
    CHECK(ipfrag_add(&T, 1, &k, 48, 1, msg + 48, 48, -1, &o) == 0);
    CHECK(ipfrag_add(&T, 1, &k, 0, 1, msg, 48, 17, &o) == 1);                   /* aux = 17 (UDP) от первого фрагмента */
    CHECK(o.len == 200 && o.aux == 17 && !memcmp(o.data, msg, 200));
    /* тот же id у IPv4 - другая датаграмма */
    ipf_key_t k4 = key4(0xFE800001, 0xBEEF);
    CHECK(ipfrag_add(&T, 2, &k, 0, 1, msg, 48, 6, &o) == 0);
    CHECK(ipfrag_add(&T, 2, &k4, 48, 0, msg + 48, 16, -1, &o) == 0 && ipfrag_pending(&T) == 2);
    /* перекрытие в IPv6 - тоже отброс всей датаграммы (RFC 5722) */
    CHECK(ipfrag_add(&T, 2, &k, 40, 1, msg + 40, 16, -1, &o) == -1 && T.overlap == 1);
}

int main(void) {
    test_orders(); test_dups_overlaps(); test_bad(); test_limits(); test_v6();
    if (fails) { printf("ipfrag: %d failures\n", fails); return 1; }
    printf("ipfrag: reassembly in all orders, duplicates, overlaps, bad/oversize, ttl, memory and per-source limits, IPv6 passed\n");
    return 0;
}
