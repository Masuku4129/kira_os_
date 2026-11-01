#include "ktest.h"
static int fillsum(int n) { int a[1000]; long s = 0; for (int i = 0; i < n; i++) a[i] = i * i; for (int i = 0; i < n; i++) s += a[i]; return (int)s; }
static char big[70000];
int main(void) {
    char local[20000];
    for (int i = 0; i < 20000; i++) local[i] = (char)(i * 7);
    for (int i = 0; i < 70000; i++) big[i] = (char)(i ^ (i >> 8));
    long h = 0; for (int i = 0; i < 20000; i++) h = h * 31 + local[i]; pn(h);
    h = 0; for (int i = 0; i < 70000; i++) h = h * 131 + big[i]; pn(h);
    pn(fillsum(1000));
    int primes = 0; for (int n = 2; n < 3000; n++) { int ok = 1; for (int d = 2; d * d <= n; d++) if (n % d == 0) { ok = 0; break; } primes += ok; } pn(primes);
    unsigned long x = 1; for (int i = 0; i < 40; i++) x = x * 6364136223846793005UL + 1442695040888963407UL; pn((long)x);
    unsigned y = 1; for (int i = 0; i < 40; i++) y = y * 1664525u + 1013904223u; pn(y);
    return 0;
}
