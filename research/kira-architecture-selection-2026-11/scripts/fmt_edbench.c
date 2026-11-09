/* [E] Стоимость проверки подписи KDRV на кодe KIRA (pk.c ed25519_verify, kdrv_sig.c, sha256.c), хост x86-64.
   Вектор: tests/kdrv_sig_test.c (ключ RFC 8032 TEST-ED25519-001). Печатает по строке на повтор: "rep metric ns_per_op" (целые, без FP).
   Сборка: см. scripts/fmt_edbench.sh. Запуск на ядре 0. */
#include "kdrv_sig.h"
#include "pk.h"
#include "immune.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
static const uint8_t package[] = "KIRA KDRV signature test package v1";
static const kdrv_sig_key_t test_key = { "TEST-ED25519-001", { 0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a }, 0 };
static const kdrv_sig_t sig = { .magic = KDRV_SIG_MAGIC, .format = KDRV_SIG_FORMAT, .size = sizeof(kdrv_sig_t), .key_id = "TEST-ED25519-001",
  .package_sha256 = { 0xcf,0x4d,0x54,0x9e,0x5a,0x1a,0x3c,0x6e,0x27,0x97,0x31,0xeb,0x35,0x46,0x37,0xc7,0xc1,0xc3,0x4a,0x4f,0xac,0x58,0xae,0xb0,0x0c,0xa8,0x95,0x12,0x92,0x82,0x1c,0x80 },
  .package_size = 35, .not_before = 1700000000ULL, .not_after = 4102444800ULL,
  .signature = { 0x4c,0x6c,0xfc,0xfa,0x69,0x9f,0x6e,0x52,0x84,0x43,0x48,0xbb,0x32,0x6f,0x8c,0x48,0x90,0xf9,0x0d,0xb2,0x98,0xd7,0xdb,0x86,0xf5,0x0a,0x34,0xd0,0xdf,0x27,0x29,0x76,
                  0x06,0xb2,0xce,0xa3,0xc3,0x35,0x66,0x2d,0x52,0x5c,0x0b,0xee,0x03,0xad,0x7d,0x46,0x54,0x98,0x54,0x6b,0x1b,0x16,0x8d,0x31,0xb7,0x7a,0x2d,0x7f,0x46,0x0c,0xb9,0x0f } };
static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }
int main(int argc, char** argv) {
    int reps = 11, n = argc > 1 ? atoi(argv[1]) : 300;
    uint8_t message[88]; static const char dom[] = "KIRA-KDRV-SIG-V1"; memcpy(message, dom, 16); memcpy(message + 16, sig.key_id, 16); memcpy(message + 32, sig.package_sha256, 32);
    for (int i = 0; i < 8; i++) { message[64 + i] = (uint8_t)(sig.package_size >> (8 * i)); message[72 + i] = (uint8_t)(sig.not_before >> (8 * i)); message[80 + i] = (uint8_t)(sig.not_after >> (8 * i)); }
    /* корректность до скорости */
    if (ed25519_verify(test_key.public_key, message, 88, sig.signature) != 0) { puts("FAIL: ed25519_verify на валидной подписи"); return 1; }
    uint8_t bad[64]; memcpy(bad, sig.signature, 64); bad[5] ^= 1; if (ed25519_verify(test_key.public_key, message, 88, bad) == 0) { puts("FAIL: принята испорченная подпись"); return 1; }
    if (kdrv_sig_verify(package, sizeof package - 1, &sig, sizeof sig, &test_key, 1, 1800000000LL) != KDRV_SIG_OK) { puts("FAIL: kdrv_sig_verify"); return 1; }
    static uint8_t big[1 << 20]; for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 2654435761u >> 24);
    uint8_t dg[32]; volatile int sink = 0;
    for (int w = 0; w < 2; w++) { for (int i = 0; i < 20; i++) sink += ed25519_verify(test_key.public_key, message, 88, sig.signature); }
    for (int r = 1; r <= reps; r++) {
        long long t0 = now_ns(); for (int i = 0; i < n; i++) sink += ed25519_verify(test_key.public_key, message, 88, sig.signature); long long t1 = now_ns();
        printf("%d ed25519_verify_ns %lld\n", r, (t1 - t0) / n);
        t0 = now_ns(); for (int i = 0; i < n; i++) sink += kdrv_sig_verify(package, sizeof package - 1, &sig, sizeof sig, &test_key, 1, 1800000000LL); t1 = now_ns();
        printf("%d kdrv_sig_verify_ns %lld\n", r, (t1 - t0) / n);
        t0 = now_ns(); for (int i = 0; i < 20; i++) { sha256(big, sizeof big, dg); sink += dg[0]; } t1 = now_ns();
        printf("%d sha256_1MiB_ns %lld\n", r, (t1 - t0) / 20);
        t0 = now_ns(); for (int i = 0; i < n; i++) { memcpy(bad, sig.signature, 64); bad[5] ^= 1; sink += ed25519_verify(test_key.public_key, message, 88, bad); } t1 = now_ns();
        printf("%d ed25519_reject_ns %lld\n", r, (t1 - t0) / n);
    }
    return sink == 0x7fffffff;
}
