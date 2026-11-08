/* 32-битные накопители, которые читает только арифметический сдвиг вправо: верх регистра может быть неточным, сдвиг обязан читать
 * только младшие 32 бита (sar r32). Переполнение накопителя, отрицательные значения, сдвиг на переменную, результат в 64-бит контексте. */
static int y[16];
static int acc_sar(const signed char *w, const int *x, int n) { int a = 0; for (int j = 0; j < n; j++) a += w[j] * x[j]; return a >> 8; }
static long acc_sar_wide(const int *x, int n, int k) { int a = 0; for (int j = 0; j < n; j++) a += x[j] * 65537; return (long)(a >> k) * 3; }
static int shl_sar(int v, int k) { int t = v * 40503; t = t << (k & 7); return t >> (k & 15); }
static unsigned shr_u(int v, int k) { int t = v * 9; return (unsigned)t >> k; }
static int neg_chain(int v) { int t = -v; t = t * 7 - 1; return (t >> 3) + (t >> 31); }
int main(void) {
    signed char w[40]; int x[40];
    unsigned r = 12345u;
    for (int i = 0; i < 40; i++) { r = r * 1103515245u + 12345u; w[i] = (signed char)(r >> 16); x[i] = (int)(r >> 3) - 100000000; }
    long acc = 0;
    for (int i = 0; i < 16; i++) { y[i] = acc_sar(w + i, x + i, 24); acc += y[i]; }
    for (int k = 0; k < 31; k++) acc += acc_sar_wide(x, 30, k);
    for (int i = -50; i < 50; i += 7) for (int k = 0; k < 20; k++) acc += shl_sar(i * 1013, k) + (long)shr_u(i, k) + neg_chain(i * 33333);
    int big = 0x7fffffff; for (int i = 0; i < 5; i++) { big += x[i] * 3; acc += big >> 4; }
    return (int)(acc & 0x7fffff);
}
