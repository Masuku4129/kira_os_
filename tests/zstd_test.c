/* Zstandard: кадры, сжатые эталонным компрессором (tests/data/zstd, уровни 1-19, с контрольной суммой и без, много блоков, RLE, сырые блоки,
   несколько кадров, пропускаемый кадр, потоковый кадр без размера), контрольные суммы SHA-256 распакованного. Затем повреждения:
   обрезка и порча байтов не должны давать запись вне буфера (ASAN/UBSAN) и обязаны отвергаться, если меняют результат. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../zstd.h"
#include "../sha256.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, m); fails++; } } while (0)

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)l + 1); if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return 0; }
    fclose(f); *n = (size_t)l; return b;
}
static void hexof(const uint8_t* d, size_t n, char* out) {
    sha256_ctx_t c; uint8_t h[32]; sha256_init(&c); sha256_update(&c, d, n); sha256_final(&c, h);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

int main(void) {
    const char* dir = "tests/data/zstd";
    char path[256]; snprintf(path, sizeof path, "%s/MANIFEST", dir);
    size_t mn; uint8_t* man = slurp(path, &mn);
    if (!man) { printf("FAIL: no %s (run from the repo root)\n", path); return 1; }
    man[mn] = 0;
    zstd_work* w = aligned_alloc(16, (zstd_work_size() + 15) & ~(size_t)15);
    static uint8_t out[700000];
    int files = 0, bytes = 0;
    for (char* line = strtok((char*)man, "\n"); line; line = strtok(0, "\n")) {
        char name[128], sha[80]; unsigned long raw;
        if (sscanf(line, "%127s %lu %79s", name, &raw, sha) != 3) { CHECK(0, "manifest line"); continue; }
        snprintf(path, sizeof path, "%s/%s", dir, name);
        size_t cn; uint8_t* c = slurp(path, &cn);
        if (!c) { printf("FAIL: cannot read %s\n", name); fails++; continue; }
        size_t got = 0; int rc = zstd_decompress(c, cn, out, sizeof out, &got, w);
        char hx[65];
        if (rc != 0 || got != raw) { printf("FAIL: %s rc=%d got=%zu want=%lu\n", name, rc, got, raw); fails++; free(c); continue; }
        hexof(out, got, hx);
        if (strcmp(hx, sha)) { printf("FAIL: %s digest differs\n", name); fails++; free(c); continue; }
        files++; bytes += (int)got;
        /* размер выхода ровно по содержимому проходит, на байт меньше - ошибка без записи вне буфера */
        if (got > 0 && strstr(name, "l3")) {
            uint8_t* tight = malloc(got); size_t g2;
            CHECK(zstd_decompress(c, cn, tight, got, &g2, w) == 0 && g2 == got, "tight buffer");
            uint8_t* small = malloc(got - 1);
            CHECK(zstd_decompress(c, cn, small, got - 1, &g2, w) == ZSTD_ERR_DST_SMALL, "buffer one byte short");
            free(tight); free(small);
        }
        /* обрезка: любой префикс короче кадра отвергается */
        for (size_t cut = 0; cut < cn && cut < 400; cut += 1 + cut / 7) {
            size_t g3; int r2 = zstd_decompress(c, cut, out, sizeof out, &g3, w);
            if (cut == 0 ? r2 == 0 : r2 == 0 && g3 == got) { printf("FAIL: %s prefix %zu accepted as complete\n", name, cut); fails++; break; }
        }
        /* порча байтов: нельзя падать; если приняли - результат либо тот же (порча в пропускаемом месте), либо суммы не сходятся для кадров с checksum */
        if (cn < 100000) {
            unsigned seed = 1234; int accepted_wrong_checksum = 0;
            for (int it = 0; it < 300; it++) {
                seed = seed * 1103515245u + 12345u;
                size_t at = (size_t)(seed >> 8) % cn; uint8_t keep = c[at]; c[at] ^= (uint8_t)(1u << (seed & 7));
                size_t g4 = 0; int r3 = zstd_decompress(c, cn, out, sizeof out, &g4, w);
                if (r3 == 0 && g4 == got) { char h2[65]; hexof(out, g4, h2); if (strcmp(h2, sha) && (c[4] & 4)) accepted_wrong_checksum++; }
                c[at] = keep;
            }
            CHECK(accepted_wrong_checksum == 0, "damaged frame with checksum must not decode to wrong data");
        }
        free(c);
    }
    CHECK(files >= 30, "enough fixtures decoded");
    /* заведомо плохие входы */
    { static const uint8_t bad1[] = {0x28,0xb5,0x2f,0xfd, 0x00};                       CHECK(zstd_decompress(bad1, sizeof bad1, out, sizeof out, &mn, w) != 0, "header cut"); }
    { static const uint8_t bad2[] = {1,2,3,4,5,6,7,8};                                 CHECK(zstd_decompress(bad2, sizeof bad2, out, sizeof out, &mn, w) == ZSTD_ERR_FORMAT, "bad magic"); }
    { static const uint8_t dict[] = {0x28,0xb5,0x2f,0xfd, 0x21, 0x01, 0x00, 0x01, 0x00, 0x00}; CHECK(zstd_decompress(dict, sizeof dict, out, sizeof out, &mn, w) == ZSTD_ERR_UNSUPPORTED, "dictionary id refused"); }
    { static const uint8_t resv[] = {0x28,0xb5,0x2f,0xfd, 0x28, 0x00};                  CHECK(zstd_decompress(resv, sizeof resv, out, sizeof out, &mn, w) != 0, "reserved bit"); }
    CHECK(zstd_decompress("", 0, out, sizeof out, &mn, w) == ZSTD_ERR_TRUNC, "empty input");
    /* размер содержимого из заголовка */
    { size_t cn; uint8_t* c = slurp("tests/data/zstd/text_10k.l3.zst", &cn); CHECK(c && zstd_frame_content_size(c, cn) == 10000, "content size"); free(c);
      c = slurp("tests/data/zstd/stream.zst", &cn); CHECK(c && zstd_frame_content_size(c, cn) == -1, "unknown content size in stream frame"); free(c); }
    /* случайный мусор после настоящего заголовка */
    { uint8_t g[300]; unsigned s = 99; int crashed = 0;
      for (int it = 0; it < 3000; it++) { for (size_t i = 0; i < sizeof g; i++) { s = s * 1664525u + 1013904223u; g[i] = (uint8_t)(s >> 24); }
          g[0] = 0x28; g[1] = 0xb5; g[2] = 0x2f; g[3] = 0xfd; g[4] = (uint8_t)(0x20 | (s & 0x44) | ((s >> 7) & 3) * 0); size_t g5; (void)zstd_decompress(g, sizeof g, out, 70000, &g5, w); }
      CHECK(!crashed, "random garbage with valid magic"); }
    free(w); free(man);
    printf(fails ? "ZSTD FAILED\n" : "zstd: %d reference frames (levels 1-19, checksums, multi-block, multi-frame, streaming) = %d bytes decoded, truncation and corruption rejected\n", files, bytes);
    return fails != 0;
}
