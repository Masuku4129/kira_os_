// Хост-тест логики ядра: SHA-256, иммунная система (куча - tests/heap_test.c, Разум - tests/mind_test.c).
// Сборка и запуск: см. tests/run_host_tests.sh
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "immune.h"

// --- заглушки платформы ---
static unsigned long long out_chars;
void uart_print(const char* s) { out_chars += strlen(s); }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }

char ro_area[256]; char text_area[512];

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void hexof(const uint8_t* d, char* out) {
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

int main(void) {
    // ---------- SHA-256: известные векторы NIST ----------
    char hx[65]; uint8_t d[32];
    sha256("", 0, d); hexof(d, hx);
    CHECK(!strcmp(hx, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256("abc", 3, d); hexof(d, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char* m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m2, strlen(m2), d); hexof(d, hx);
    CHECK(!strcmp(hx, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // миллион 'a' потоково (проверка переносов блоков и длины)
    sha256_ctx_t c; sha256_init(&c);
    char chunk[1000]; memset(chunk, 'a', sizeof chunk);
    for (int i = 0; i < 1000; i++) sha256_update(&c, chunk, sizeof chunk);
    sha256_final(&c, d); hexof(d, hx);
    CHECK(!strcmp(hx, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    // граничные длины 55/56/63/64/65 против эталона (значения посчитаны Python hashlib)
    FILE* f = fopen("tests/sha_vectors.txt", "r");
    if (f) {
        int len; char want[80]; int n = 0;
        while (fscanf(f, "%d %64s", &len, want) == 2) {
            uint8_t* buf = malloc(len + 1);
            for (int i = 0; i < len; i++) buf[i] = (uint8_t)(i * 7 + 3);
            sha256(buf, len, d); hexof(d, hx);
            CHECK(!strcmp(hx, want));
            free(buf); n++;
        }
        fclose(f);
        printf("sha256 boundary vectors checked: %d\n", n);
    }

    // ---------- Иммунная система ----------
    for (int i = 0; i < 256; i++) ro_area[i] = (char)i;
    for (int i = 0; i < 512; i++) text_area[i] = (char)(i * 3);
    immune_init();
    CHECK(immune_verify_self() == 0);
    text_area[100] ^= 1;                    // "заражение" кода ядра
    CHECK(immune_verify_self() == -1);
    text_area[100] ^= 1;
    CHECK(immune_verify_self() == 0);
    ro_area[5] ^= 0x80;                     // порча констант
    CHECK(immune_verify_self() == -1);
    ro_area[5] ^= 0x80;

    uint8_t sled[64]; memset(sled, 0x90, sizeof sled); sled[60] = 0xCC;
    CHECK(immune_classify(sled, sizeof sled) == IMMUNE_HOSTILE);
    uint8_t pad[64]; memset(pad, 0xCC, sizeof pad); memset(pad + 10, 0x90, 23); // 23 NOP < порога 24
    CHECK(immune_classify(pad, sizeof pad) == IMMUNE_UNKNOWN);
    memset(pad + 10, 0x90, 24);
    CHECK(immune_classify(pad, sizeof pad) == IMMUNE_HOSTILE);
    uint8_t sh[16] = { 1, 2, '/', 'b', 'i', 'n', '/', 's', 'h', 0 };
    CHECK(immune_classify(sh, sizeof sh) == IMMUNE_HOSTILE);
    uint8_t clean[8] = { 0x55, 0x48, 0x89, 0xE5, 0x31, 0xC0, 0x5D, 0xC3 };
    CHECK(immune_classify(clean, sizeof clean) == IMMUNE_UNKNOWN);
    CHECK(immune_classify(0, 10) == IMMUNE_UNKNOWN);
    sha256(clean, sizeof clean, d);
    CHECK(immune_register_self(d) == 0);
    CHECK(immune_classify(clean, sizeof clean) == IMMUNE_SELF);
    clean[3] ^= 1;
    CHECK(immune_classify(clean, sizeof clean) == IMMUNE_UNKNOWN);   // одна смена бита - уже чужой
    uint8_t custom[3] = { 0xDE, 0xAD, 0xBE };
    CHECK(immune_add_antibody("test", custom, 3) == 0);
    uint8_t withc[8] = { 0, 1, 0xDE, 0xAD, 0xBE, 9, 9, 9 };
    CHECK(immune_classify(withc, sizeof withc) == IMMUNE_HOSTILE);
    CHECK(immune_add_antibody("bad", custom, 0) == -1);
    CHECK(immune_quarantine_count() >= 6);
    uint8_t buf[32]; memset(buf, 0xAB, sizeof buf);
    immune_phagocytose(buf, sizeof buf);
    int z = 1; for (int i = 0; i < 32; i++) if (buf[i]) z = 0;
    CHECK(z);
    // кольцо карантина не переполняется
    for (int i = 0; i < 200; i++) { uint8_t t[4] = { (uint8_t)i, 7, 7, 7 }; immune_classify(t, 4); }
    CHECK(immune_quarantine_count() == 64);

    printf("%s\n", fails ? "FAILED" : "ALL LOGIC TESTS PASSED");
    return fails != 0;
}
