/* Фаззинг формата пакета драйвера Ring 3 под ASan/UBSan: kdrv_header_check, kdrv_rec_check, kdrv_match_dev, kdrv_io_check.
   Вход: искажённые верные заголовки и записи, чистая случайность, граничные значения для проверки портов.
   Инварианты: принятый заголовок точно описывает файл (размер, имя [a-z0-9_-], 1..KDRV_MATCH_MAX условий, нет условия
   «подходит всё»); принятая запись подтверждена; прошедшая проверка порта лежит строго внутри BAR и не выходит за 64 КБ.
   Использование: kdrv_fuzz ИТЕРАЦИЙ ЗЁРНО */
#include "../kdrv_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint64_t rnd64(void) { return ((uint64_t)rnd() << 32) | rnd(); }
static int fails;
#define BAD(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

static void mutate(uint8_t* b, size_t n) {
    int k = 1 + (int)(rnd() % 3);
    while (k--) {
        switch (rnd() % 4) {
        case 0: b[rnd() % n] = (uint8_t)rnd(); break;
        case 1: b[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 2: { size_t o = rnd() % n; size_t l = rnd() % 8; for (size_t i = 0; i < l && o + i < n; i++) b[o + i] = (rnd() & 1) ? 0xff : 0; break; }
        case 3: { static const uint32_t v[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff, KDRV_ELF_MAX, KDRV_ELF_MAX + 1, 8, 9};
                  if (n >= 4) { size_t o = rnd() % (n - 3); uint32_t x = v[rnd() % 9]; memcpy(b + o, &x, 4); } break; }
        }
    }
}

static void good_header(kdrv_header_t* h, uint32_t elf) {
    memset(h, 0, sizeof *h);
    h->magic = KDRV_MAGIC; h->format = KDRV_FORMAT; h->hdr_size = sizeof *h;
    strcpy(h->name, "rtl8139"); h->version = 1 + rnd() % 5; h->caps = rnd() & 0xF;
    h->elf_size = elf; h->nmatch = 1 + rnd() % KDRV_MATCH_MAX;
    for (uint32_t i = 0; i < h->nmatch; i++) { h->match[i].vendor = 0x10ec; h->match[i].device = (uint16_t)rnd(); h->match[i].cls = rnd(); h->match[i].cls_mask = 0; }
}

static void check_header(const uint8_t* file, uint32_t len) {
    kdrv_header_t h;
    memset(&h, 0xa5, sizeof h);
    int r = kdrv_header_check(file, len, &h);
    if (!kdrv_strerror(r) || !*kdrv_strerror(r)) BAD("нет текста ошибки для %d", r);
    if (r != KDRV_OK) return;
    if (h.hdr_size != sizeof h || h.elf_size > KDRV_ELF_MAX || (uint64_t)h.hdr_size + h.elf_size != len) BAD("размер заголовка/образа не сходится с файлом");
    if (h.nmatch < 1 || h.nmatch > KDRV_MATCH_MAX) BAD("число условий %u", h.nmatch);
    size_t nl = strnlen(h.name, sizeof h.name);
    if (nl == 0 || nl > KDRV_NAME_MAX || nl == sizeof h.name) BAD("длина имени %zu", nl);
    for (size_t i = 0; i < nl; i++) { char c = h.name[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) BAD("символ имени %02x", (unsigned char)c); }
    for (uint32_t i = 0; i < h.nmatch; i++)
        if (h.match[i].vendor == KDRV_ANY && h.match[i].device == KDRV_ANY && h.match[i].cls_mask == 0) BAD("принято условие «подходит всё»");
    (void)kdrv_match_dev(&h, (uint16_t)rnd(), (uint16_t)rnd(), rnd());
}

static void check_rec(const uint8_t* rec, uint32_t len) {
    kdrv_rec_t r;
    memset(&r, 0xa5, sizeof r);
    int e = kdrv_rec_check(rec, len, &r);
    if (e == KDRV_OK) {
        if (r.confirmed != KREC_CONFIRMED && !kdrv_rec_publisher_required(&r)) BAD("принята неподтверждённая запись");
        if (memchr(r.source, 0, sizeof r.source) == NULL) BAD("source без нуля");
        (void)kdrv_rec_publisher_required(&r);
    }
}

static void check_io(void) {
    static const uint64_t widths[] = {1, 2, 4, 3, 8, 0};
    uint64_t a1 = rnd() % 3 == 0 ? rnd64() : kdrv_io_pack(rnd() % 8, widths[rnd() % 6], rnd() % 5, rnd() % 5000);
    uint64_t base = rnd() % 3 == 0 ? rnd64() : (uint64_t)(rnd() % 0x10000), size = rnd() % 3 == 0 ? rnd64() : (uint64_t)(rnd() % 0x400);
    uint64_t off = rnd() % 3 == 0 ? rnd64() : (uint64_t)(rnd() % 0x500);
    uint32_t port = 0xdeadbeef, w = 0, op = 0, cnt = 0;
    int r = kdrv_io_check(a1, off, (int)(rnd() & 1), base, size, &port, &w, &op, &cnt);
    if (r != 0) return;
    if (!(w == 1 || w == 2 || w == 4) || op > KDRV_IO_WRITE_MULTI || cnt == 0 || (uint64_t)cnt * w > KDRV_IO_MAX_BYTES) BAD("параметры вне допусков");
    if (port < base || port > 0xFFFF || (uint64_t)port + w > base + size || off % w) BAD("порт %u вне BAR [%llu,+%llu) off=%llu", port, (unsigned long long)base, (unsigned long long)size, (unsigned long long)off);
    if (op <= KDRV_IO_WRITE && cnt != 1) BAD("одиночная операция с числом %u", cnt);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "kdrv_fuzz ИТЕРАЦИЙ ЗЁРНО\n"); return 2; }
    long iters = atol(argv[1]);
    rs = 0x2545f4914f6cdd1dull ^ strtoull(argv[2], NULL, 0);
    if (!rs) rs = 1;
    long acc_h = 0, acc_r = 0;
    for (long it = 0; it < iters; it++) {
        /* заголовок: верный образец с ELF разной длины, затем искажения и обрезка */
        uint32_t elf = rnd() % 64;
        size_t n = sizeof(kdrv_header_t) + elf;
        uint8_t* f = calloc(1, n + 16);
        kdrv_header_t h; good_header(&h, elf); memcpy(f, &h, sizeof h);
        uint32_t len = (uint32_t)n;
        if (rnd() % 3) mutate(f, n);
        if (rnd() % 6 == 0) len = rnd() % (n + 1);
        uint8_t* e = malloc(len ? len : 1); memcpy(e, f, len);
        if (len == n) { kdrv_header_t t; if (kdrv_header_check(e, len, &t) == KDRV_OK) acc_h++; }
        check_header(e, len); free(e); free(f);
        /* запись */
        kdrv_rec_t rec; memset(&rec, 0, sizeof rec);
        rec.magic = KREC_MAGIC; rec.format = KDRV_FORMAT; rec.rec_size = sizeof rec; rec.confirmed = KREC_CONFIRMED; rec.caps_ok = rnd() & 0xF;
        strcpy(rec.source, "https://example/drv");
        uint8_t rb[sizeof rec]; memcpy(rb, &rec, sizeof rec);
        if (rnd() % 3) mutate(rb, sizeof rb);
        uint32_t rl = rnd() % 8 == 0 ? rnd() % (sizeof rb + 1) : (uint32_t)sizeof rb;
        uint8_t* re = malloc(rl ? rl : 1); memcpy(re, rb, rl);
        { kdrv_rec_t t; if (kdrv_rec_check(re, rl, &t) == KDRV_OK) acc_r++; }
        check_rec(re, rl); free(re);
        /* чистая случайность */
        uint32_t rn = rnd() % 300; uint8_t* rr = malloc(rn ? rn : 1); for (uint32_t i = 0; i < rn; i++) rr[i] = (uint8_t)rnd();
        check_header(rr, rn); check_rec(rr, rn); free(rr);
        check_io(); check_io();
    }
    printf("kdrv_fuzz: %ld итераций, принято заголовков %ld, записей %ld, нарушений %d\n", iters, acc_h, acc_r, fails);
    if (acc_h == 0 || acc_r == 0) { printf("FAIL: исходные образцы не принимаются — фаззер ничего не проверяет\n"); return 1; }
    return fails ? 1 : 0;
}
