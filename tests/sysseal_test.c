/* Печать системы: сборка манифеста, сверка, подмена файла, пропажа, порча манифеста, подмена корня. */
#include "sysseal.h"
#include "sha256.h"
#include "cbor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

typedef struct { const char* path; const char* data; int present; } file_t;
static file_t files[] = { { "/bin/a", "program a", 1 }, { "/bin/b", "second program", 1 }, { "/etc/x", "config", 1 } };
static int hash_fn(void* ctx, const char* path, uint64_t* size, uint8_t h[32]) {
    (void)ctx;
    for (size_t i = 0; i < 3; i++) if (!strcmp(files[i].path, path) && files[i].present) {
        *size = strlen(files[i].data); sha256(files[i].data, *size, h); return 0;
    }
    return -1;
}
static int bad_calls; static char bad_last[128];
static void bad(void* c, const char* p, const char* w) { (void)c; bad_calls++; snprintf(bad_last, sizeof bad_last, "%s: %s", p, w); }

int main(void) {
    seal_entry e[3];
    for (int i = 0; i < 3; i++) { strcpy(e[i].path, files[i].path); e[i].size = strlen(files[i].data); sha256(files[i].data, e[i].size, e[i].hash); }
    static uint8_t m[4096], m2[4096]; static uint8_t leaves[SEAL_MAX_FILES][32];
    size_t n = seal_build(e, 3, m, sizeof m);
    CHECK(n > 0 && n < 400, "манифест собрался");
    CHECK(cbor_check(m, n, 1) == CBOR_OK, "манифест - строгий CBOR");
    CHECK(seal_build(e, 3, m2, sizeof m2) == n && !memcmp(m, m2, n), "детерминированность");
    CHECK(seal_build(e, 3, m2, 40) == 0, "мало места - 0");
    seal_report r;
    CHECK(seal_check(m, n, hash_fn, 0, leaves, bad, &r) == 0 && r.files == 3 && r.ok == 3 && !r.changed && !r.missing && r.root_ok, "всё совпало");
    files[1].data = "second program!";
    CHECK(seal_check(m, n, hash_fn, 0, leaves, bad, &r) == 0 && r.changed == 1 && r.ok == 2 && r.root_ok, "изменённый файл замечен");
    CHECK(bad_calls == 1 && !strcmp(bad_last, "/bin/b: изменён"), "сообщение об изменении");
    files[1].data = "second program"; files[2].present = 0;
    CHECK(seal_check(m, n, hash_fn, 0, leaves, bad, &r) == 0 && r.missing == 1 && r.ok == 2, "пропавший файл замечен");
    files[2].present = 1;
    /* подмена корня в манифесте: записи сходятся, корень - нет */
    memcpy(m2, m, n);
    int pos = -1; for (size_t i = 0; i + 4 < n; i++) if (!memcmp(m2 + i, "root", 4)) { pos = (int)i + 4; break; }
    CHECK(pos > 0, "ключ root найден");
    m2[pos + 2] ^= 1;   /* первый байт значения после заголовка bytes(32) = 0x58 0x20 */
    CHECK(seal_check(m2, n, hash_fn, 0, leaves, bad, &r) == 0 && r.ok == 3 && !r.root_ok, "подменённый корень не сходится");
    /* усечение и мусор */
    CHECK(seal_check(m, n - 1, hash_fn, 0, leaves, bad, &r) != 0, "усечённый манифест отвергнут");
    memcpy(m2, m, n); m2[0] = 0x80;
    CHECK(seal_check(m2, n, hash_fn, 0, leaves, bad, &r) != 0, "не карта - отвергнут");
    /* пустая печать */
    n = seal_build(e, 0, m, sizeof m);
    CHECK(n > 0 && seal_check(m, n, hash_fn, 0, leaves, bad, &r) == 0 && r.files == 0 && r.root_ok, "пустая печать корректна");
    if (fails) return 1;
    printf("sysseal: build, check, tamper, missing, bad root, truncation - passed\n");
    return 0;
}
