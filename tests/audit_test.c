/* Журнал аудита: формат, цепочка, подписи Ed25519, сегменты и ротация на настоящей KiraFS, все виды подмены, обрыв питания, заполнение диска.
   Использование: audit_test [ЗЁРЕН ОБРЫВОВ] [каталог для выгрузки файлов журнала (для tools/auditverify.py)] */
#include "auditstore.h"
#include "pk.h"
#include "sha256.h"
#include "kfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---------- диск с кэшем записи и обрывом питания (как tests/kfs_crash_test.c) ---------- */
#define SECT (16 + 4096 * 8)                      /* 16 МБ */
#define PEND_MAX 40000
static uint8_t disk[(size_t)SECT * 512];
static struct { uint64_t lba; uint8_t d[512]; } pend[PEND_MAX];
static int npend; static long crash_after = -1; static int tripped;
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b) {
    (void)d; if (lba + n > SECT || tripped) return -1;
    memcpy(b, disk + lba * 512, (size_t)n * 512);
    for (int i = 0; i < npend; i++) if (pend[i].lba >= lba && pend[i].lba < lba + n) memcpy((uint8_t*)b + (pend[i].lba - lba) * 512, pend[i].d, 512);
    return 0;
}
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b) {
    (void)d; if (lba + n > SECT) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (tripped) return -1;
        if (crash_after == 0) { tripped = 1; return -1; }
        if (crash_after > 0) crash_after--;
        if (npend >= PEND_MAX) { printf("FAIL: кэш записи переполнен\n"); exit(2); }
        pend[npend].lba = lba + i; memcpy(pend[npend].d, (const uint8_t*)b + (size_t)i * 512, 512); npend++;
    }
    return 0;
}
static int fl(blk_dev_t* d) {
    (void)d; if (tripped) return -1;
    if (crash_after == 0) { tripped = 1; return -1; }
    if (crash_after > 0) crash_after--;
    for (int i = 0; i < npend; i++) memcpy(disk + pend[i].lba * 512, pend[i].d, 512);
    npend = 0; return 0;
}
static blk_dev_t dev = {"ram", SECT, rd, wr, fl, 0};
static void power_loss(void) {
    int idx[PEND_MAX]; int k = 0; int mode = (int)(rnd() % 4);
    for (int i = 0; i < npend; i++) if (mode == 1 || (mode >= 2 && (rnd() & 1))) idx[k++] = i;
    for (int i = k - 1; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (int i = 0; i < k; i++) memcpy(disk + pend[idx[i]].lba * 512, pend[idx[i]].d, 512);
    npend = 0; tripped = 0; crash_after = -1;
}
static void fresh_fs(void) { memset(disk, 0, sizeof disk); npend = 0; tripped = 0; crash_after = -1; if (kfs_format(&dev, 16)) { printf("FAIL: format\n"); exit(1); } }

/* ---------- писатель: то же, что делает audit.c (запечатать, подписать точку, принять в хранилище) ---------- */
static uint8_t SEED[32], PUB[32];
static aud_chain_t CH;
static aud_store_t ST;
static unsigned since_ckpt;
static aud_rec_t Q[4096]; static unsigned qh, qt;          /* очередь запечатанных записей (кольцо ядра) */
static uint64_t mono;

static void seal_ckpt(void) {
    aud_rec_t r; memset(&r, 0, sizeof r);
    r.mono_ms = ++mono; r.real_s = 1763380000 + mono; r.boot = 1;
    aud_ckpt_fill(&r, PUB); aud_seal(&CH, &r);
    CHECK(aud_ckpt_sign(&r, SEED) == 0, "подпись точки");
    Q[qh++ % 4096] = r; since_ckpt = 0;
}
static void emit(unsigned kind, unsigned src, unsigned res, const char* subj, const char* det) {
    if (aud_next_is_ckpt(&CH)) seal_ckpt();
    aud_rec_t r; memset(&r, 0, sizeof r);
    r.mono_ms = ++mono; r.real_s = 1763380000 + mono; r.kind = (uint16_t)kind; r.src = (uint8_t)src; r.res = (uint8_t)res; r.boot = 1; r.pid = 7; r.domain = 2; r.arg = (uint32_t)mono;
    strncpy(r.subject, subj, 23); strncpy(r.detail, det, 55);
    aud_seal(&CH, &r);
    Q[qh++ % 4096] = r;
    if (++since_ckpt >= AUD_CKPT_EVERY) seal_ckpt();
}
/* Поток записи: из очереди в хранилище, при нехватке места - на диск. 0 - всё принято. */
static int pump(void) {
    while (qt != qh) {
        if (!aud_store_room(&ST) && aud_store_flush(&ST) != 0) return -1;
        if (aud_store_append(&ST, &Q[qt % 4096]) != 0) { printf("FAIL: append отказал при room\n"); exit(1); }
        qt++;
    }
    return 0;
}
static int pump_flush(void) { if (pump()) return -1; return aud_store_flush(&ST); }

static void open_store(aud_vstate_t* v) {
    int r = aud_store_load(&ST, PUB, v, &CH);
    CHECK(r == 0, "загрузка хранилища: %d", r);
    qh = qt = 0; since_ckpt = 0;
}
static void populate(unsigned n) { for (unsigned i = 0; i < n; i++) { emit(AUD_K_EXEC + (i % 3), AUD_SRC_PROGRAM, i % 2, "prog", "detail"); if (i % 7 == 0) pump(); } CHECK(pump_flush() == 0, "сброс"); }

static int64_t rdf(const char* p, uint8_t* b, size_t cap) { return kfs_read(p, 0, b, cap); }
static int wrf(const char* p, const uint8_t* b, size_t n) { return kfs_write(p, b, n); }
static const char* F1 = "/var/audit.0000000001.log", *F2 = "/var/audit.0000000257.log";

static aud_vstate_t V;
static void verify_fresh(void) { aud_store_t* s = malloc(sizeof *s); aud_chain_t c; aud_store_load(s, PUB, &V, &c); free(s); }

/* ---------- векторы Ed25519 ---------- */
static int hx(const char* h, uint8_t* o) { int n = 0; while (h[0] && h[1]) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2; } return n; }
static void test_ed25519(void) {
    static const char* const vec[][4] = {
        { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
        { "a54dca182530bb1d6d132cded6237b2ed91e3f721fcb1971174494d6493c9d5c", "2fa3c670e1fb2f12fb6b3c04a3014dc627f9fb9ec099840cbe13a878ff73e1dd", "",
          "00d9286174bad530a4b92f85f0a6d010e2ab4fafe25be6f4d18b4e80f5594e44a8151e3fb610e32dd41b8c7fb20c1398f4285abf957bd51f654a90bd117a4203" },
        { "a0aeb3fee9232f8af2211f9ee491c5b10becb5563bfc1e6f93427ecbc8fe2955", "e33966cad0fd45285736d6957c32c6420fd2536bedceec345d1af43073e68638",
          "e5cd8e46dc8ed4b7c2764d2a5a4d767706f85d8690024ad6bda3401be9c8cbccc935f6cd1f61226ae15338ae1a34004d",
          "bf323b5a2164e59b72b1fa6f2ffd46d67f42749324f7a9eb1e121eea7cf579bb0434e2b34a7ba356dfc500e603ddb0bcb8921cb242ee06c366a8845371b97702" },
        { "33ba0d246ac04c81b1baf23e3bf9eef5f79f2b4934af87f5520b69b94b0d982e", "fd0c5f09a66a014836534a06eab1d0662fb9ba49ab19b790a5ad02abab645cb7",
          "85bb55b672a872637acd7466fcb60e0e8ff18463b0e4b2ba29703474f064ac68f700f5b02b3dc666f45bdeaa2ccaedcd2b5157410e4dee4af2b34f430a073447de636c0e806c957ba684d6431fb5ead7424d09e15d024c5848f23d1fa6f7361d7f618d1532e70e20e2a6668de7f47e8467e546d53ec8e2a1257bdb256c9b3e4fbb498146ef7030cbf9537252dcceadd764b6a32fbb09adeae109c4a997203975352b878b145c8a42d884cf4cfda72d8e1d5dd92589082d852a7122873ee805add58942167a385286",
          "e85a193e1c86835ab50ac3d077930d670722e3e0a7c3eb216495d59cfa96916e9d0b42f6bd614a9b9392c5662833144e8dc0c6ca1dbf2287ef88e56e4275ff02" },
    };
    for (unsigned i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        uint8_t seed[32], pub[32], gp[32], msg[300], sig[64], gs[64];
        hx(vec[i][0], seed); hx(vec[i][1], pub); int ml = hx(vec[i][2], msg); hx(vec[i][3], sig);
        CHECK(ed25519_pubkey(seed, gp) == 0 && !memcmp(gp, pub, 32), "Ed25519 вектор %u: открытый ключ", i);
        CHECK(ed25519_sign(seed, msg, (size_t)ml, gs) == 0 && !memcmp(gs, sig, 64), "Ed25519 вектор %u: подпись", i);
        CHECK(ed25519_verify(pub, msg, (size_t)ml, sig) == 0, "Ed25519 вектор %u: проверка", i);
        gs[5] ^= 1; CHECK(ed25519_verify(pub, msg, (size_t)ml, gs) != 0, "Ed25519 вектор %u: искажённая подпись принята", i);
    }
}

/* ---------- подмена ---------- */
static uint8_t SNAP[sizeof disk];
static aud_store_t SAVED_ST; static aud_chain_t SAVED_CH;      /* живое состояние пишущего на момент снимка */
static void snap(void) { memcpy(SNAP, disk, sizeof disk); SAVED_ST = ST; SAVED_CH = CH; }
static void restore(void) { memcpy(disk, SNAP, sizeof disk); npend = 0; kfs_mount(&dev, 16); ST = SAVED_ST; CH = SAVED_CH; }
static uint8_t BUF[AUD_SEGBYTES + 4096];
static aud_rec_t* RECP(uint8_t* b, size_t i) { return (aud_rec_t*)(b + AUD_HDR_SIZE + i * AUD_REC_SIZE); }
static size_t nrecs(int64_t len) { return ((size_t)len - AUD_HDR_SIZE) / AUD_REC_SIZE; }
static size_t first_nonckpt(uint8_t* b, size_t n, size_t from) { for (size_t i = from; i < n; i++) if (RECP(b, i)->kind != AUD_K_CKPT && RECP(b, i + 1)->kind != AUD_K_CKPT) return i; return 0; }
static void expect(const char* what, int code, uint64_t seq) {
    verify_fresh();
    CHECK(V.code == code && (seq == 0 || V.bad_seq == seq), "%s: код %d (%s) запись %llu, ожидалось %d запись %llu", what, V.code, aud_verr_name(V.code), (unsigned long long)V.bad_seq, code, (unsigned long long)seq);
}
static void rehash_after(uint8_t* b, size_t n, size_t from) {        /* подмена умная: пересчитывает prev у всех следующих (ключа нет, подписи остаются прежними) */
    uint8_t h[32];
    for (size_t i = from; i + 1 < n; i++) { aud_hash(RECP(b, i), h); memcpy(RECP(b, i + 1)->prev, h, 32); }
}

static void test_tamper(void) {
    fresh_fs(); open_store(&V);
    populate(600);
    emit(AUD_K_SHUTDOWN, AUD_SRC_SYSTEM, 0, "kernel", "end"); seal_ckpt(); CHECK(pump_flush() == 0, "сброс");     /* последняя запись - подписанная точка */
    snap();
    verify_fresh();
    CHECK(V.code == AUD_V_OK && V.count >= 640 && V.first_seq == 1, "чистый журнал: код %d, записей %llu", V.code, (unsigned long long)V.count);
    CHECK(ST.nsegs == 2 && ST.cur_first == 513, "ротация: закрытых %u, текущий с %llu", ST.nsegs, (unsigned long long)ST.cur_first);
    aud_vstate_t vl; CHECK(aud_store_verify(&ST, &vl) == 0 && vl.code == AUD_V_OK && vl.ckpts > 30, "полная проверка с диска: код %d, точек %llu", vl.code, (unsigned long long)vl.ckpts);
    int64_t n1 = rdf(F1, BUF, sizeof BUF); size_t k = first_nonckpt(BUF, nrecs(n1), 20);
    uint64_t kseq = RECP(BUF, k)->seq;

    /* 1. изменение записи: в закрытом сегменте, в текущем, в начале и в конце сегмента */
    restore(); rdf(F1, BUF, sizeof BUF); RECP(BUF, k)->detail[0] ^= 0x20; wrf(F1, BUF, (size_t)n1);
    expect("изменение записи (закрытый сегмент)", AUD_V_CHAIN, kseq + 1);
    restore(); n1 = rdf(F2, BUF, sizeof BUF); k = first_nonckpt(BUF, nrecs(n1), 30); kseq = RECP(BUF, k)->seq; RECP(BUF, k)->res ^= 1; wrf(F2, BUF, (size_t)n1);
    expect("изменение записи (второй сегмент)", AUD_V_CHAIN, kseq + 1);
    restore(); n1 = rdf(AUD_ACTIVE, BUF, sizeof BUF); k = first_nonckpt(BUF, nrecs(n1), 40); kseq = RECP(BUF, k)->seq; RECP(BUF, k)->pid ^= 0x100; wrf(AUD_ACTIVE, BUF, (size_t)n1);
    expect("изменение записи (текущий сегмент)", AUD_V_CHAIN, kseq + 1);
    /* 2. изменение + пересчёт цепочки после неё: подпись следующей точки ломается */
    restore(); n1 = rdf(F1, BUF, sizeof BUF); k = first_nonckpt(BUF, nrecs(n1), 20); kseq = RECP(BUF, k)->seq;
    RECP(BUF, k)->subject[0] = 'X'; rehash_after(BUF, nrecs(n1), k); wrf(F1, BUF, (size_t)n1);
    verify_fresh(); CHECK(V.code == AUD_V_SIG && V.bad_seq > kseq && V.bad_seq <= kseq + AUD_CKPT_EVERY + 1, "изменение с пересчётом цепочки: код %d запись %llu", V.code, (unsigned long long)V.bad_seq);
    /* 3. подпись и поля точки */
    restore(); n1 = rdf(F1, BUF, sizeof BUF); for (k = 0; k < nrecs(n1) && RECP(BUF, k)->kind != AUD_K_CKPT; k++) ;
    kseq = RECP(BUF, k)->seq; ((aud_ckpt_t*)RECP(BUF, k))->sig[3] ^= 1; wrf(F1, BUF, (size_t)n1);
    expect("искажена подпись точки", AUD_V_SIG, kseq);
    restore(); n1 = rdf(F1, BUF, sizeof BUF); ((aud_ckpt_t*)RECP(BUF, k))->keyid[0] ^= 1; wrf(F1, BUF, (size_t)n1);
    expect("точка с чужим ключом (keyid)", AUD_V_SIG, kseq);
    /* 4. удаление, вставка, перестановка */
    restore(); n1 = rdf(F1, BUF, sizeof BUF); k = first_nonckpt(BUF, nrecs(n1), 20); kseq = RECP(BUF, k)->seq;
    memmove(RECP(BUF, k), RECP(BUF, k + 1), (nrecs(n1) - k - 1) * AUD_REC_SIZE); wrf(F1, BUF, (size_t)n1 - AUD_REC_SIZE);
    expect("удалена запись", AUD_V_SEQ, kseq);
    restore(); n1 = rdf(AUD_ACTIVE, BUF, sizeof BUF); k = first_nonckpt(BUF, nrecs(n1), 40); kseq = RECP(BUF, k)->seq;
    { aud_rec_t t = *RECP(BUF, k); memmove(RECP(BUF, k + 1), RECP(BUF, k), (nrecs(n1) - k) * AUD_REC_SIZE); *RECP(BUF, k + 1) = t; }
    wrf(AUD_ACTIVE, BUF, (size_t)n1 + AUD_REC_SIZE); expect("вставлена копия записи", AUD_V_SEQ, kseq + 1);
    restore(); n1 = rdf(F1, BUF, sizeof BUF); k = first_nonckpt(BUF, nrecs(n1), 20); kseq = RECP(BUF, k)->seq;
    { aud_rec_t t = *RECP(BUF, k); *RECP(BUF, k) = *RECP(BUF, k + 1); *RECP(BUF, k + 1) = t; } wrf(F1, BUF, (size_t)n1);
    expect("записи переставлены", AUD_V_SEQ, kseq);
    restore(); n1 = rdf(AUD_ACTIVE, BUF, sizeof BUF); { aud_rec_t f; memcpy(&f, RECP(BUF, k), sizeof f); f.detail[0] = 'F'; memmove(RECP(BUF, k + 1), RECP(BUF, k), (nrecs(n1) - k) * AUD_REC_SIZE); *RECP(BUF, k + 1) = f; }
    wrf(AUD_ACTIVE, BUF, (size_t)n1 + AUD_REC_SIZE); verify_fresh(); CHECK(V.code == AUD_V_SEQ || V.code == AUD_V_CHAIN, "вставлена поддельная запись: код %d", V.code);
    /* 5. хвост */
    restore(); n1 = rdf(AUD_ACTIVE, BUF, sizeof BUF); wrf(AUD_ACTIVE, BUF, (size_t)n1 - 3 * AUD_REC_SIZE);
    verify_fresh(); CHECK(V.code == AUD_V_TRUNC, "обрезан хвост (подписанная точка в /var/audit.ckpt): код %d", V.code);
    { aud_vstate_t vv; aud_store_verify(&ST, &vv); CHECK(vv.code == AUD_V_TRUNC, "обрезан хвост, проверка живой системы: код %d", vv.code); }
    restore(); n1 = rdf(AUD_ACTIVE, BUF, sizeof BUF);                          /* обрезка + откат файла-свидетеля на прежнюю точку: офлайн не видна (граница, docs/AUDIT.md), живая система видит */
    { uint8_t ck[AUD_REC_SIZE]; size_t nr = nrecs(n1), i = nr - 1; for (size_t j = nr - 2; ; j--) { if (RECP(BUF, j)->kind == AUD_K_CKPT) { i = j; break; } if (!j) break; }
      memcpy(ck, RECP(BUF, i), sizeof ck); wrf(AUD_ACTIVE, BUF, AUD_HDR_SIZE + (i + 1) * AUD_REC_SIZE); wrf(AUD_CKPTFILE, ck, sizeof ck); }
    verify_fresh(); CHECK(V.code == AUD_V_OK, "обрезка вместе с откатом свидетеля офлайн не видна (граница): код %d", V.code);
    { aud_vstate_t vv; aud_store_verify(&ST, &vv); CHECK(vv.code == AUD_V_TRUNC, "та же обрезка, проверка живой системы (память помнит): код %d", vv.code); }
    restore(); n1 = rdf(AUD_ACTIVE, BUF, sizeof BUF); wrf(AUD_ACTIVE, BUF, (size_t)n1 - 50); verify_fresh();
    CHECK(V.code == AUD_V_FORMAT || V.code == AUD_V_TRUNC, "оборвана запись посередине: код %d", V.code);
    { kfs_stat_t st; CHECK(kfs_stat("/var/audit.0000000513.bad", &st) != 0, "улика не должна появляться при проверке без продолжения"); }
    /* 6. файлы */
    restore(); kfs_unlink(F2); expect("удалён средний файл", AUD_V_SEQ, 513);
    restore(); kfs_unlink(F1); expect("удалён самый старый файл без записи prune", AUD_V_PRUNE, 257);
    restore(); n1 = rdf(F2, BUF, sizeof BUF); BUF[8 + 4 + 4 + 8 + 32 + 3] ^= 1; wrf(F2, BUF, (size_t)n1); expect("искажён якорь сегмента", AUD_V_ANCHOR, 257);
    restore(); n1 = rdf(F1, BUF, sizeof BUF); BUF[0] = 'X'; wrf(F1, BUF, (size_t)n1); verify_fresh(); CHECK(V.code == AUD_V_FORMAT, "чужая метка файла: код %d", V.code);
    { uint8_t other[32] = { 9 }, op[32]; ed25519_pubkey(other, op); restore(); aud_vstate_t vv; aud_store_t* s = malloc(sizeof *s); aud_chain_t c; aud_store_load(s, op, &vv, &c); free(s);
      CHECK(vv.code == AUD_V_KEY, "чужой ключ устройства: код %d", vv.code); }
    /* 7. честная очистка: PRUNE, затем удаление самого старого файла */
    restore(); populate(0);
    emit(AUD_K_PRUNE, AUD_SRC_KERNEL, 0, "audit", "prune"); RECP(BUF, 0); { aud_rec_t* last = &Q[(qh - 1) % 4096]; (void)last; }
    /* arg записи prune - номер последней удалённой записи: 256; запись уже запечатана без него, поэтому делаем её явно */
    qh = qt;  /* отбросить запечатанную без arg (в очереди, не в хранилище) */
    CH.next_seq = ST.next_seq; memcpy(CH.head, ST.tail_head, 32);
    { aud_rec_t r; memset(&r, 0, sizeof r); r.kind = AUD_K_PRUNE; r.arg = 256; r.boot = 1; aud_seal(&CH, &r); Q[qh++ % 4096] = r; }
    seal_ckpt(); CHECK(pump_flush() == 0, "сброс prune");
    CHECK(aud_store_prune(&ST, 256) == 1, "удалён один сегмент");
    { aud_vstate_t vv; aud_store_verify(&ST, &vv); CHECK(vv.code == AUD_V_OK && vv.first_seq == 257, "после честной очистки проверка проходит: код %d, с записи %llu", vv.code, (unsigned long long)vv.first_seq); }
    verify_fresh(); CHECK(V.code == AUD_V_OK && V.first_seq == 257, "после честной очистки перезагрузка: код %d", V.code);
    restore();
}

/* ---------- ротация и продолжение после перезагрузки ---------- */
static void test_rotation(const char* dump) {
    fresh_fs(); open_store(&V);
    CHECK(V.code == AUD_V_OK && ST.next_seq == 1, "пустой диск: код %d next %llu", V.code, (unsigned long long)ST.next_seq);
    for (int round = 0; round < 5; round++) {
        populate(137);
        aud_vstate_t vv;
        CHECK(aud_store_verify(&ST, &vv) == 0 && vv.code == AUD_V_OK, "раунд %d: проверка: код %d", round, vv.code);
        open_store(&V);                                                              /* «перезагрузка» */
        CHECK(V.code == AUD_V_OK, "раунд %d: загрузка после перезагрузки: код %d запись %llu", round, V.code, (unsigned long long)V.bad_seq);
    }
    CHECK(ST.next_seq > 700 && ST.nsegs >= 2, "цепочка продолжается через перезагрузки: next %llu, закрытых %u", (unsigned long long)ST.next_seq, ST.nsegs);
    kfs_stat_t st; CHECK(kfs_stat(F1, &st) == 0 && st.size == AUD_SEGBYTES, "закрытый файл полного размера");
    if (dump) {
        char p[300]; uint8_t b[AUD_SEGBYTES];
        snprintf(p, sizeof p, "%s/pub.hex", dump); FILE* f = fopen(p, "w"); for (int i = 0; i < 32; i++) fprintf(f, "%02x", PUB[i]); fprintf(f, "\n"); fclose(f);
        struct { const char* kfs; const char* host; } names[] = { { AUD_ACTIVE, "audit.log" }, { AUD_CKPTFILE, "audit.ckpt" } };
        for (unsigned i = 0; i < ST.nsegs + 2; i++) {
            char kp[96], hp[64];
            if (i < ST.nsegs) { snprintf(hp, sizeof hp, "audit.%010llu.log", (unsigned long long)ST.segs[i]); snprintf(kp, sizeof kp, "/var/%s", hp); }
            else { snprintf(kp, sizeof kp, "%s", names[i - ST.nsegs].kfs); snprintf(hp, sizeof hp, "%s", names[i - ST.nsegs].host); }
            int64_t n = rdf(kp, b, sizeof b); snprintf(p, sizeof p, "%s/%s", dump, hp); f = fopen(p, "wb"); fwrite(b, 1, (size_t)n, f); fclose(f);
        }
    }
}

/* ---------- обрыв питания посреди записи ---------- */
static void test_power(unsigned seeds) {
    unsigned crashes = 0;
    for (unsigned sd = 0; sd < seeds; sd++) {
        rs = 88172645463325252ull ^ ((uint64_t)(sd + 1) * 0x9E3779B97F4A7C15ull);
        fresh_fs(); open_store(&V);
        uint64_t acked = 0;                                                         /* записи до этого номера точно лежат на диске (flush вернул 0) */
        for (int op = 0; op < 60; op++) {
            unsigned burst = 1 + rnd() % (op % 9 == 0 ? 300 : 30);
            for (unsigned i = 0; i < burst; i++) emit(AUD_K_EXEC, AUD_SRC_PROGRAM, 0, "p", "d");
            crash_after = (rnd() % 3) ? (long)(rnd() % 120) : -1;
            int r = pump_flush();
            if (r == 0) { acked = ST.flushed_next; crash_after = -1; continue; }
            if (!tripped) { crash_after = -1; continue; }                          /* ошибка без обрыва - не наш случай */
            crashes++;
            power_loss();
            if (kfs_mount(&dev, 16)) { CHECK(0, "seed %u op %d: монтирование после обрыва", sd, op); break; }
            open_store(&V);
            CHECK(V.code == AUD_V_OK, "seed %u op %d: после обрыва журнал не проходит проверку: %s (запись %llu)", sd, op, aud_verr_name(V.code), (unsigned long long)V.bad_seq);
            CHECK(ST.next_seq >= acked, "seed %u op %d: пропали подтверждённые записи: на диске до %llu, подтверждено до %llu", sd, op, (unsigned long long)ST.next_seq - 1, (unsigned long long)acked - 1);
            { aud_vstate_t vv; aud_store_verify(&ST, &vv); CHECK(vv.code == AUD_V_OK, "seed %u op %d: полная проверка после обрыва: %s", sd, op, aud_verr_name(vv.code)); }
            acked = ST.flushed_next;                                                 /* то, что пережило обрыв, - уже на диске */
        }
        CHECK(pump_flush() == 0, "seed %u: итоговый сброс", sd);
        aud_vstate_t vv; CHECK(aud_store_verify(&ST, &vv) == 0 && vv.code == AUD_V_OK, "seed %u: итоговая проверка: %s", sd, aud_verr_name(vv.code));
        verify_fresh(); CHECK(V.code == AUD_V_OK, "seed %u: итоговая загрузка: %s (запись %llu)", sd, aud_verr_name(V.code), (unsigned long long)V.bad_seq);
    }
    printf("   обрывов питания: %u (по %u последовательностям)\n", crashes, seeds);
}

/* ---------- диск заполнен ---------- */
static void test_full(void) {
    fresh_fs(); open_store(&V);
    populate(300);
    static uint8_t junk[1 << 20]; memset(junk, 0xA5, sizeof junk);
    int n = 0; char p[32];
    for (;;) { snprintf(p, sizeof p, "/junk%d", n); for (size_t q = 0; q < 200000; q += 4) memcpy(junk + q, &(uint32_t){ (uint32_t)(n * 100003u + q * 7919u) }, 4);   /* у каждого файла своё содержимое: одинаковое KiraFS разделит */
               if (kfs_write(p, junk, 200000) != 0 || ++n > 200) break; }
    for (int m = 0; m < 400; m++) { snprintf(p, sizeof p, "/small%d", m); for (size_t q = 0; q < 4096; q += 4) memcpy(junk + q, &(uint32_t){ (uint32_t)(m * 77777u + q) }, 4); if (kfs_write(p, junk, 4096) != 0) break; }       /* добить до нуля свободных блоков */
    aud_store_t* keep = malloc(sizeof *keep); *keep = ST;
    int refused = 0;
    for (int i = 0; i < 2000 && !refused; i++) { emit(AUD_K_EXEC, AUD_SRC_PROGRAM, 0, "p", "d"); if (pump_flush() != 0) refused = 1; }
    CHECK(refused, "запись на полный диск отказала (а не прошла молча)");
    CHECK(ST.io_errors > 0, "ошибки записи посчитаны: %u", ST.io_errors);
    uint64_t pending = qh - qt; (void)pending;
    for (int i = 0; i < n; i++) { snprintf(p, sizeof p, "/junk%d", i); kfs_unlink(p); }
    for (int m = 0; m < 400; m++) { snprintf(p, sizeof p, "/small%d", m); kfs_unlink(p); }
    CHECK(pump_flush() == 0, "после освобождения места журнал дописался");
    aud_vstate_t vv; CHECK(aud_store_verify(&ST, &vv) == 0 && vv.code == AUD_V_OK, "после заполнения и освобождения диска цепочка цела: %s", aud_verr_name(vv.code));
    free(keep);
}

static void test_bench(void) {
    fresh_fs(); open_store(&V);
    struct timespec a, b; const int N = 20000;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++) {
        aud_rec_t r; memset(&r, 0, sizeof r); r.kind = AUD_K_EXEC; r.mono_ms = (uint64_t)i; strcpy(r.subject, "demo");
        if (aud_next_is_ckpt(&CH)) { aud_rec_t c; memset(&c, 0, sizeof c); aud_ckpt_fill(&c, PUB); aud_seal(&CH, &c); }
        aud_seal(&CH, &r);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("   запечатать запись (SHA-256 160 байт + цепочка): %.0f нс\n", ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / N);
    aud_rec_t r; memset(&r, 0, sizeof r); r.kind = AUD_K_CKPT; r.seq = 16; aud_ckpt_fill(&r, PUB);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 20; i++) aud_ckpt_sign(&r, SEED);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("   подпись контрольной точки Ed25519: %.2f мс\n", ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 20 / 1e6);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 20; i++) aud_ckpt_check(&r, PUB);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("   проверка подписи точки: %.2f мс\n", ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 20 / 1e6);
}

int main(int argc, char** argv) {
    unsigned seeds = argc > 1 ? (unsigned)atoi(argv[1]) : 12;
    const char* dump = argc > 2 ? argv[2] : 0;
    for (int i = 0; i < 32; i++) SEED[i] = (uint8_t)(i * 7 + 3);
    CHECK(ed25519_pubkey(SEED, PUB) == 0, "ключ устройства");
    CHECK(sizeof(aud_rec_t) == 160 && sizeof(aud_hdr_t) == 256, "размеры формата");
    test_ed25519();
    test_rotation(dump);
    test_tamper();
    test_full();
    test_power(seeds);
    test_bench();
    printf("audit: проверок %d, ошибок %d\n", checks, fails);
    return fails ? 1 : 0;
}
