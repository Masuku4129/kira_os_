/* Проверка интерпретатора WASM: тот же код собирается обычным компилятором (эталон) и в .wasm - вывод обязан совпасть.
   Рекурсия, память (решето), call_indirect (сравнение через указатель), br_table (switch), 64-битная арифметика,
   сдвиги, вращения, clz/ctz/popcount, расширение знака. */
#include "kw.h"

static unsigned rotl32(unsigned x, int k) { return (x << (k & 31)) | (x >> ((32 - (k & 31)) & 31)); }
static unsigned rotr32(unsigned x, int k) { return (x >> (k & 31)) | (x << ((32 - (k & 31)) & 31)); }
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static unsigned char sieve[20000];
static int cmp_up(int a, int b) { return a - b; }
static int cmp_down(int a, int b) { return b - a; }
static int data[300];
static void sort(int* a, int n, int (*cmp)(int, int)) {
    for (int i = 1; i < n; i++) { int v = a[i], j = i - 1; while (j >= 0 && cmp(a[j], v) > 0) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
}
static int kind(int c) {
    switch (c) { case 'a': case 'e': case 'i': case 'o': case 'u': return 1; case ' ': return 2; case '0': case '1': case '2': case '3': return 3;
                 case '.': return 4; case ',': return 5; case '!': return 6; default: return 0; }
}

int kmain(void) {
    int f = fib(24);
    int primes = 0;
    for (int i = 2; i < 20000; i++) sieve[i] = 1;
    for (int i = 2; i * i < 20000; i++) if (sieve[i]) for (int j = i * i; j < 20000; j += i) sieve[j] = 0;
    for (int i = 2; i < 20000; i++) primes += sieve[i];
    unsigned seed = 12345; unsigned long long sum = 0;   /* без знакового переполнения: поведение определено */
    for (int i = 0; i < 300; i++) { seed = seed * 1103515245u + 12345u; data[i] = (int)(seed >> 8) % 100000 - 50000; }
    sort(data, 300, cmp_up);   for (int i = 0; i < 300; i++) sum = sum * 31 + (unsigned long long)(long long)data[i];
    sort(data, 300, cmp_down); for (int i = 0; i < 300; i++) sum = sum * 37 + (unsigned long long)(long long)data[i];
    const char* text = "hello, kira os! 0123 wasm runs on x86-64 and arm64.";
    unsigned kinds = 0; for (int i = 0; text[i]; i++) kinds = kinds * 7u + (unsigned)kind(text[i]);
    unsigned long long h = 0xcbf29ce484222325ull; long long sx = 0;
    for (int i = 0; i < 1000; i++) {
        unsigned long long x = (unsigned long long)i * 0x9E3779B97F4A7C15ull;
        h ^= x; h *= 0x100000001b3ull;
        h ^= (h >> 29) | (h << 35);
        h += (unsigned long long)__builtin_clzll(x | 1) + (unsigned long long)__builtin_ctzll(x | 0x8000000000000000ull) + (unsigned long long)__builtin_popcountll(x);
        unsigned r = rotl32((unsigned)x, i & 31) ^ rotr32((unsigned)(x >> 32), (i * 7) & 31);
        h += r;
        long long sd = (long long)(x >> 3) - 0x0FFFFFFFFFFFFFFFll;
        if (i % 13) { h += (unsigned long long)(sd / (i % 13 - 6 ? i % 13 - 6 : 1)); h += (unsigned long long)(sd % 97); }
        h += x / (unsigned long long)(i + 1) + x % 1000003u;
        signed char c = (signed char)(i * 37); short s = (short)(i * 1237); sx += c + s + (int)(signed char)(x >> 13);
    }
    kw_say("wtest: fib(24)="); kw_say_i(f);
    kw_say(" primes<20000="); kw_say_i(primes);
    kw_say(" sort="); kw_say_x((unsigned long long)sum);
    kw_say(" switch="); kw_say_i((long long)kinds);
    kw_say(" ops="); kw_say_x(h);
    kw_say(" sext="); kw_say_i(sx);
    kw_say("\n");
    return f == 46368 && primes == 2262 ? 0 : 1;
}
#ifdef KW_NATIVE
int main(void) { return kmain(); }
#endif
