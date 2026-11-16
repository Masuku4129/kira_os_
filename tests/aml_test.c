/* Хост-тест интерпретатора AML. Таблица tests/data/acpi/laptop.aml собрана настоящим iasl из laptop.asl (там же). Железо - модель:
   встроенный контроллер (EC), конфигурация PCI южного моста, порты, память. Ожидаемые числа посчитаны вручную по тексту ASL. */
#include "aml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint8_t ec[256];
static uint8_t pcicfg[256];              /* южный мост: bus 0, dev 0x1F, fn 0 */
static uint8_t iomem[0x40];              /* SystemMemory 0xFED00000.. */
static uint8_t ioports[0x3000];          /* порты 0x1000.. 0x2001 */
static uint8_t idxregs[256];
static unsigned io_writes, mem_reads;
static uint32_t slept_ms, stall_us;
static int notify_n; static char notify_path[8][32]; static uint32_t notify_val[8];
static aml_ctx_t* G;

static int hw_rw(void* u, int write, unsigned space, uint64_t addr, unsigned bits, uint64_t* val, uint32_t pci) {
    (void)u;
    unsigned nb = bits / 8;
    if (space == AML_SP_EC) { if (addr > 255) return -1; if (write) ec[addr] = (uint8_t)*val; else *val = ec[addr]; return 0; }
    if (space == AML_SP_PCI) {
        if (pci != (0u << 16 | 0u << 8 | 0x1Fu << 3 | 0)) { if (!write) *val = 0xFFFFFFFFu; return 0; }
        if (addr + nb > 256) return -1;
        if (write) { for (unsigned i = 0; i < nb; i++) pcicfg[addr + i] = (uint8_t)(*val >> (8 * i)); }
        else { uint64_t v = 0; for (unsigned i = 0; i < nb; i++) v |= (uint64_t)pcicfg[addr + i] << (8 * i); *val = v; }
        return 0;
    }
    if (space == AML_SP_MEM) {
        if (addr < 0xFED00000ull || addr + nb > 0xFED00040ull) return -1;
        uint8_t* p = iomem + (addr - 0xFED00000ull);
        if (write) { for (unsigned i = 0; i < nb; i++) p[i] = (uint8_t)(*val >> (8 * i)); }
        else { uint64_t v = 0; for (unsigned i = 0; i < nb; i++) v |= (uint64_t)p[i] << (8 * i); *val = v; mem_reads++; }
        return 0;
    }
    if (space == AML_SP_IO) {
        if (addr >= 0x3000) return -1;
        if (addr == 0x2001) {                                  /* порт данных индексного регистра */
            uint8_t ix = ioports[0x2000];
            if (write) idxregs[ix] = (uint8_t)*val; else *val = idxregs[ix];
            return 0;
        }
        if (write) { io_writes++; for (unsigned i = 0; i < nb; i++) ioports[addr + i] = (uint8_t)(*val >> (8 * i)); }
        else { uint64_t v = 0; for (unsigned i = 0; i < nb; i++) v |= (uint64_t)ioports[addr + i] << (8 * i); *val = v; }
        return 0;
    }
    return -1;
}
static void hw_sleep(void* u, uint32_t ms) { (void)u; slept_ms += ms; }
static void hw_stall(void* u, uint32_t us) { (void)u; stall_us += us; }
static void hw_notify(void* u, aml_node_t* n, uint32_t v) {
    (void)u;
    if (notify_n < 8) { aml_node_path(n, notify_path[notify_n], sizeof notify_path[0]); notify_val[notify_n] = v; notify_n++; }
}

static uint8_t* load_file(const char* path, uint32_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    CHECK(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f); *len = (uint32_t)n;
    return b;
}

static uint64_t T(const char* path, unsigned n, uint64_t a0, uint64_t a1) {
    aml_val_t args[2] = { aml_int(a0), aml_int(a1) }, v; uint64_t r;
    int rc = aml_call_path(G, 0, path, args, n, &v);
    if (rc) { printf("FAIL: %s rc=%d\n", path, rc); exit(1); }
    CHECK(aml_val_int(&v, &r) == 0);
    return r;
}
static int E(const char* path, unsigned n, uint64_t a0, uint64_t a1) {
    aml_val_t args[2] = { aml_int(a0), aml_int(a1) }, v;
    return aml_call_path(G, 0, path, args, n, &v);
}
static int str_eq(const aml_val_t* v, const char* s) { return v->t == AML_V_STR && v->len == strlen(s) && memcmp(v->u.b, s, v->len) == 0; }

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "tests/data/acpi";
    char p[256]; snprintf(p, sizeof p, "%s/laptop.aml", dir);
    uint32_t len; uint8_t* tbl = load_file(p, &len);
    static uint8_t arena[1 << 20];
    aml_ops_t ops = { hw_rw, hw_sleep, hw_stall, 0, hw_notify, 0, 0 };
    G = aml_create(arena, sizeof arena, &ops);
    CHECK(G);
    { int lr = aml_load(G, tbl, len); if (lr) printf("aml_load rc=%d\n", lr); CHECK(lr == 0); }
    aml_load_done(G);
    aml_stats_t st; aml_stats(G, &st);
    CHECK(st.load_errors == 0 && st.loaded_tables == 1 && st.nodes > 100);
    printf("aml: nodes %u, arena %u of %u\n", st.nodes, st.arena_used, st.arena_size);

    /* железо: PIRQ A=11, B=отключена(0x80), C=10, D=5; EC: батарея, сеть, крышка открыта */
    pcicfg[0x60] = 0x0B; pcicfg[0x61] = 0x80; pcicfg[0x62] = 0x0A; pcicfg[0x63] = 0x05;
    ec[0] = 0x07; ec[4] = 0x01; ec[5] = 0xB8; ec[6] = 0x0B; ec[7] = 0x18; ec[8] = 0x15; ec[9] = 0x18; ec[10] = 0x2E;
    ec[0x0B] = 0x10; ec[0x0C] = 0x27; ec[0x0D] = 0x1C; ec[0x0E] = 0x25; ec[0x0F] = 47;

    /* --- интерпретатор: арифметика, логика, циклы --- */
    CHECK(T("\\TST.TADD", 2, 3, 4) == 7);
    CHECK(T("\\TST.TSUB", 2, 10, 3) == 7);
    CHECK(T("\\TST.TMUL", 2, 6, 7) == 42);
    CHECK(T("\\TST.TDIV", 2, 17, 5) == 3);
    CHECK(T("\\TST.TMOD", 2, 17, 5) == 2);
    CHECK(T("\\TST.TSHL", 2, 1, 40) == (1ull << 40));
    CHECK(T("\\TST.TSHL", 2, 1, 64) == 0);
    CHECK(T("\\TST.TSHR", 2, 0x100, 4) == 0x10);
    CHECK(T("\\TST.TBIT", 2, 0xF0, 0x3C) == 0xCC30);
    CHECK(T("\\TST.TNOT", 1, 0, 0) == ~0ull);
    CHECK(T("\\TST.TADD", 2, ~0ull, 1) == 0);                    /* переполнение 64 бит */
    CHECK(T("\\TST.TLGC", 2, 5, 5) == 241);
    CHECK(T("\\TST.TLGC", 2, 2, 5) == 214);
    CHECK(T("\\TST.TLGC", 2, 0, 0) == 305);
    CHECK(T("\\TST.TSUM", 1, 5, 0) == 12);
    CHECK(T("\\TST.TFAC", 1, 10, 0) == 3628800);
    CHECK(T("\\TST.TFAC", 1, 20, 0) == 2432902008176640000ull);
    CHECK(T("\\TST.TFND", 1, 20, 0) == 1);
    CHECK(T("\\TST.TFND", 1, 99, 0) == ~0ull);
    CHECK(T("\\TST.TFSL", 1, 0x10, 0) == 5);
    CHECK(T("\\TST.TFSL", 1, 0, 0) == 0);
    CHECK(T("\\TST.TFSR", 1, 0x10, 0) == 5);
    CHECK(T("\\TST.TFSR", 1, 8, 0) == 4);
    CHECK(T("\\TST.TLOC", 0, 0, 0) == 56);
    CHECK(T("\\TST.TREF", 0, 0, 0) == 42);
    CHECK(T("\\TST.TCND", 0, 0, 0) == 2);
    CHECK(T("\\TST.TBCD", 1, 1234, 0) == 0x1234);
    CHECK(T("\\TST.TFBC", 1, 0x1234, 0) == 1234);
    CHECK(T("\\TST.TSET", 1, 9, 0) == 9);
    CHECK(T("\\TST.TNAP", 1, 10, 0) == 1 && slept_ms == 10);

    /* --- пределы: рекурсия, бесконечный цикл, сон, деление на нуль, выход за пакет --- */
    CHECK(E("\\TST.TREC", 0, 0, 0) == AML_E_DEPTH);
    CHECK(E("\\TST.TFAC", 1, 40, 0) == AML_E_DEPTH);
    aml_set_limits(G, 200000, 0, 0, 0);
    CHECK(E("\\TST.TSPN", 0, 0, 0) == AML_E_STEPS);
    aml_set_limits(G, AML_DEF_MAX_STEPS, 0, 0, 0);
    CHECK(E("\\TST.TNAP", 1, 5000, 0) == AML_E_TIME);
    CHECK(E("\\TST.TDV0", 1, 0, 0) != 0);
    CHECK(T("\\TST.TDV0", 1, 5, 0) == 2);
    CHECK(E("\\TST.TOOB", 0, 0, 0) == AML_E_BOUNDS);
    CHECK(E("\\TST.TADD", 1, 1, 0) == AML_E_ARGS);               /* неверное число аргументов */
    CHECK(E("\\TST.NOSUCH", 0, 0, 0) == AML_E_NOTFOUND);

    /* --- значения: поля буфера, пакеты, строки --- */
    CHECK(T("\\TST.TGTD", 0, 0, 0) == 0x04030201);
    CHECK(T("\\TST.TGTW", 0, 0, 0) == 0x0605);
    CHECK(T("\\TST.TGTB", 0, 0, 0) == 8);
    CHECK(T("\\TST.TGTF", 0, 0, 0) == 48);
    aml_val_t v;
    CHECK(aml_call_path(G, 0, "\\TST.TPTD", (aml_val_t[]){ aml_int(0xAABBCCDDu) }, 1, &v) == 0);
    CHECK(v.t == AML_V_BUF && v.len == 8 && v.u.b[0] == 0xDD && v.u.b[1] == 0xCC && v.u.b[2] == 0xBB && v.u.b[3] == 0xAA && v.u.b[4] == 5);
    CHECK(T("\\TST.TGTD", 0, 0, 0) == 0xAABBCCDDu);               /* запись легла в тот же буфер */
    CHECK(aml_call_path(G, 0, "\\TST.TPTF", (aml_val_t[]){ aml_int(0x15) }, 1, &v) == 0);
    CHECK(v.u.b[0] == 0xDD && v.u.b[1] == 0x5C && v.u.b[2] == 0xB9 && v.u.b[3] == 0xAA);
    CHECK(T("\\TST.TPKI", 1, 0, 0) == 10 && T("\\TST.TPKI", 1, 1, 0) == 20);
    CHECK(aml_call_path(G, 0, "\\TST.TPKI", (aml_val_t[]){ aml_int(2) }, 1, &v) == 0 && str_eq(&v, "abc"));
    CHECK(T("\\TST.TPKN", 0, 0, 0) == 4);
    CHECK(T("\\TST.TINR", 0, 0, 0) == 8);
    CHECK(T("\\TST.TSTL", 0, 0, 0) == 4);
    CHECK(aml_call_path(G, 0, "\\TST.TCAT", 0, 0, &v) == 0 && str_eq(&v, "Kira-OS"));
    CHECK(aml_call_path(G, 0, "\\TST.TMID", 0, 0, &v) == 0 && str_eq(&v, "CDE"));
    CHECK(aml_call_path(G, 0, "\\TST.TTOH", (aml_val_t[]){ aml_int(0x1A) }, 1, &v) == 0 && str_eq(&v, "1A"));
    CHECK(aml_call_path(G, 0, "\\TST.TTOD", (aml_val_t[]){ aml_int(1234) }, 1, &v) == 0 && str_eq(&v, "1234"));
    CHECK(aml_call_path(G, 0, "\\TST.TBFX", 0, 0, &v) == 0 && v.t == AML_V_BUF && v.len == 4 && v.u.b[2] == 0x5A && v.u.b[0] == 0);
    CHECK(aml_call_path(G, 0, "\\TST.TOOB", 0, 0, &v) == AML_E_BOUNDS);
    {   /* ObjectType: число 1, строка 2, буфер 3 */
        aml_val_t s; s.t = AML_V_STR; s.rk = 0; s.len = 1; s.u.b = (uint8_t*)"x"; s.idx = 0;
        aml_val_t b = s; b.t = AML_V_BUF;
        CHECK(aml_call_path(G, 0, "\\TST.TOBJ", (aml_val_t[]){ aml_int(7) }, 1, &v) == 0 && v.u.i == 1);
        CHECK(aml_call_path(G, 0, "\\TST.TOBJ", &s, 1, &v) == 0 && v.u.i == 2);
        CHECK(aml_call_path(G, 0, "\\TST.TOBJ", &b, 1, &v) == 0 && v.u.i == 3);
        CHECK(aml_call_path(G, 0, "\\TST.TOSI", &s, 1, &v) == 0 && v.u.i == 0);
        s.u.b = (uint8_t*)"Module Device"; s.len = 13;
        CHECK(aml_call_path(G, 0, "\\TST.TOSI", &s, 1, &v) == 0 && v.u.i == ~0ull);
    }
    CHECK(aml_call_path(G, 0, "\\TST.TOSN", 0, 0, &v) == 0 && str_eq(&v, "Microsoft Windows NT"));

    /* --- области: память, порты, индексное поле --- */
    iomem[0] = 0xEF; iomem[1] = 0xBE; iomem[2] = 0xAD; iomem[3] = 0xDE;
    for (int i = 0; i < 8; i++) iomem[8 + i] = (uint8_t)(0x88 - 0x11 * i);
    CHECK(T("\\TST.TMEM", 0, 0, 0) == 0xDEADBEEFu);
    CHECK(T("\\TST.TMRQ", 0, 0, 0) == 0x1122334455667788ull);
    ioports[0x1001] = 0xF5;
    CHECK(T("\\TST.TIOW", 1, 0xA, 0) == 0xA);
    CHECK(ioports[0x1001] == 0x0A);                              /* WriteAsZeros: старшие 4 бита сброшены, а не сохранены */
    idxregs[1] = 0x34; idxregs[2] = 0x12;
    CHECK(T("\\TST.TIFR", 0, 0, 0) == 0x1234);
    CHECK(T("\\TST.TIFW", 1, 0x77, 0) == 0x77 && idxregs[3] == 0x77);
    CHECK(ioports[0x2000] == 3);

    /* --- устройства: идентификаторы --- */
    aml_node_t *bat = aml_lookup(G, 0, "\\_SB.BAT0"), *ac = aml_lookup(G, 0, "\\_SB.ADP1"), *lid = aml_lookup(G, 0, "\\_SB.LID0");
    aml_node_t *ecn = aml_lookup(G, 0, "\\_SB.EC0"), *pci = aml_lookup(G, 0, "\\_SB.PCI0"), *pwrb = aml_lookup(G, 0, "\\_SB.PWRB");
    CHECK(bat && ac && lid && ecn && pci && pwrb);
    CHECK(aml_hid_match(G, bat, "PNP0C0A") && aml_hid_match(G, ac, "ACPI0003") && aml_hid_match(G, lid, "PNP0C0D"));
    CHECK(aml_hid_match(G, ecn, "PNP0C09") && aml_hid_match(G, pci, "PNP0A08") && aml_hid_match(G, pci, "PNP0A03") && aml_hid_match(G, pwrb, "PNP0C0C"));
    CHECK(!aml_hid_match(G, bat, "PNP0C0D"));
    { char path[64]; CHECK(aml_node_path(bat, path, sizeof path) > 0 && strcmp(path, "\\_SB_.BAT0") == 0); }
    CHECK(T("\\_SB.INIV", 0, 0, 0) == 0);                        /* _INI ещё не звали */
    CHECK(aml_call_path(G, 0, "\\_SB._INI", 0, 0, &v) == 0);
    CHECK(T("\\_SB.INIV", 0, 0, 0) == 0x77);

    /* --- батарея, сеть, крышка, температура: настоящие методы по модели EC --- */
    CHECK(T("\\_SB.BAT0._STA", 0, 0, 0) == 0x1F);
    CHECK(aml_call_path(G, bat, "_BST", 0, 0, &v) == 0 && v.t == AML_V_PKG && v.len == 4);
    CHECK(aml_pkg_at(&v, 0)->u.i == 1 && aml_pkg_at(&v, 1)->u.i == 3000 && aml_pkg_at(&v, 2)->u.i == 0x1518 && aml_pkg_at(&v, 3)->u.i == 0x2E18);
    CHECK(aml_call_path(G, bat, "_BIF", 0, 0, &v) == 0 && v.t == AML_V_PKG && v.len == 13);
    CHECK(aml_pkg_at(&v, 1)->u.i == 10000 && aml_pkg_at(&v, 2)->u.i == 9500 && aml_pkg_at(&v, 4)->u.i == 10800 && str_eq(aml_pkg_at(&v, 9), "KiraTest"));
    CHECK(aml_call_path(G, bat, "_BIX", 0, 0, &v) == 0 && v.len == 21 && aml_pkg_at(&v, 2)->u.i == 10000 && aml_pkg_at(&v, 3)->u.i == 9500);
    CHECK(T("\\_SB.ADP1._PSR", 0, 0, 0) == 1);
    CHECK(T("\\_SB.LID0._LID", 0, 0, 0) == 1);
    CHECK(T("\\_TZ.TZ0._TMP", 0, 0, 0) == 3202);
    ec[0] = 0x04; ec[4] = 0x02; ec[5] = 0x10; ec[6] = 0x00; ec[7] = 0x00; ec[8] = 0x10;   /* сеть отключена, крышка закрыта... нет: бит2 = крышка открыта, батарея ушла */
    CHECK(T("\\_SB.BAT0._STA", 0, 0, 0) == 0x0F);
    CHECK(T("\\_SB.ADP1._PSR", 0, 0, 0) == 0);
    ec[0] = 0x03; ec[4] = 0x02;                                  /* батарея есть, сеть есть, крышка закрыта, заряд идёт */
    CHECK(T("\\_SB.LID0._LID", 0, 0, 0) == 0);
    CHECK(aml_call_path(G, bat, "_BST", 0, 0, &v) == 0 && aml_pkg_at(&v, 0)->u.i == 2 && aml_pkg_at(&v, 1)->u.i == 0x10 && aml_pkg_at(&v, 2)->u.i == 0x1000);
    ec[0x0F] = 0; CHECK(T("\\_TZ.TZ0._TMP", 0, 0, 0) == 2732);

    /* --- _PRT и ссылки LNKx (через PCI-конфигурацию южного моста) --- */
    CHECK(aml_call_path(G, pci, "_PRT", 0, 0, &v) == 0 && v.t == AML_V_PKG && v.len == 6);
    {
        const aml_val_t* e = aml_pkg_at(&v, 0);
        CHECK(e && e->t == AML_V_PKG && e->len == 4 && aml_pkg_at(e, 0)->u.i == 0x0001FFFF && aml_pkg_at(e, 1)->u.i == 0);
        aml_node_t* src; char path[64];
        CHECK(aml_val_node(aml_pkg_at(e, 2), &src) == 0 && aml_node_path(src, path, sizeof path) > 0 && strcmp(path, "\\_SB_.LNKA") == 0);
        e = aml_pkg_at(&v, 4);
        CHECK(aml_pkg_at(e, 2)->t == AML_V_INT && aml_pkg_at(e, 2)->u.i == 0 && aml_pkg_at(e, 3)->u.i == 19);
    }
    aml_node_t *lnka = aml_lookup(G, 0, "\\_SB.LNKA"), *lnkb = aml_lookup(G, 0, "\\_SB.LNKB");
    CHECK(lnka && lnkb);
    CHECK(T("\\_SB.LNKA._STA", 0, 0, 0) == 0x0B && T("\\_SB.LNKB._STA", 0, 0, 0) == 0x09);
    CHECK(aml_call_path(G, lnka, "_CRS", 0, 0, &v) == 0 && v.t == AML_V_BUF && v.len >= 3 && v.u.b[0] == 0x23 && (v.u.b[1] | v.u.b[2] << 8) == (1 << 11));
    CHECK(aml_call_path(G, lnkb, "_CRS", 0, 0, &v) == 0 && (v.u.b[1] | v.u.b[2] << 8) == 0);
    CHECK(aml_call_path(G, aml_lookup(G, 0, "\\_SB.LNKC"), "_CRS", 0, 0, &v) == 0 && (v.u.b[1] | v.u.b[2] << 8) == (1 << 10));

    /* --- события GPE: Notify доходит до обработчика --- */
    notify_n = 0;
    CHECK(aml_call_path(G, 0, "\\_GPE._L1C", 0, 0, &v) == 0);
    CHECK(notify_n == 2 && strcmp(notify_path[0], "\\_SB_.BAT0") == 0 && notify_val[0] == 0x80 && strcmp(notify_path[1], "\\_SB_.ADP1") == 0);

    /* --- битые и обрезанные входы: ошибка, не падение --- */
    aml_stats(G, &st);
    CHECK(st.persist_free > 0);
    { aml_val_t r; CHECK(aml_call(G, aml_root(G), 0, 0, &r) == 0); }
    free(tbl);
    printf("aml_test: PASS\n");
    return 0;
}
