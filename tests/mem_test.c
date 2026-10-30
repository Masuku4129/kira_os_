// Хост-тест строковых функций ядра (mem.c) под своими именами: копирование, перекрытие в обе стороны, заполнение.
#include <stdio.h>
#include <string.h>
void* k_memcpy(void*, const void*, size_t);
void* k_memmove(void*, const void*, size_t);
void* k_memset(void*, int, size_t);
int k_memcmp(const void*, const void*, size_t);
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    unsigned char a[300], b[300], ref[300];
    for (int n = 0; n < 260; n += 7) for (int off = 0; off < 9; off++) {
        for (int i = 0; i < 300; i++) { a[i] = (unsigned char)(i * 31 + 7); b[i] = 0xEE; }
        CHECK(k_memcpy(b + off, a, (size_t)n) == b + off);
        CHECK(memcmp(b + off, a, (size_t)n) == 0 && b[off + n] == 0xEE && (off == 0 || b[off - 1] == 0xEE));
        for (int i = 0; i < 300; i++) a[i] = ref[i] = (unsigned char)(i * 13 + 1);
        memmove(ref + off, ref + 20, (size_t)n); k_memmove(a + off, a + 20, (size_t)n);   // вниз
        CHECK(memcmp(a, ref, 300) == 0);
        for (int i = 0; i < 300; i++) a[i] = ref[i] = (unsigned char)(i * 13 + 1);
        memmove(ref + 20 + off, ref + 20, (size_t)n > 270 ? 0 : (size_t)n); k_memmove(a + 20 + off, a + 20, (size_t)n > 270 ? 0 : (size_t)n);   // вверх с перекрытием
        CHECK(memcmp(a, ref, 300) == 0);
        k_memset(b, 0x5A, (size_t)n);
        for (int i = 0; i < n; i++) CHECK(b[i] == 0x5A);
        CHECK(k_memcmp(a, ref, (size_t)n) == 0);
    }
    unsigned char x[3] = {1, 2, 3}, y[3] = {1, 2, 4};
    CHECK(k_memcmp(x, y, 3) < 0 && k_memcmp(y, x, 3) > 0 && k_memcmp(x, y, 2) == 0);
    printf("%s\n", fails ? "FAILED" : "mem: memcpy/memmove (both directions)/memset/memcmp passed");
    return fails != 0;
}
