/* Вынос инвариантов из циклов и слияние адресов: вложенные циклы, нулевое число итераций, goto/continue/break, деление не выносится,
   масштабы 1/2/4/8, отрицательные индексы, поля структур, записи по [база+индекс]. */
struct S { char c; short s; int i; long l; int arr[5]; };
static int g4[64]; static short g2[64]; static char g1[64]; static long g8[64];
static int div_guard(int n, int d) { int acc = 0; for (int i = 0; i < n; i++) acc += 100 / d; return acc; }
static int nested(int n, int k) {
    int t = 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { int inv = k * 7 + 3; t += inv * (i + 1) + j * inv; }
    return t;
}
static int cont_brk(int n, int a, int b) {
    int s = 0;
    for (int i = 0; i < n; i++) { if (i == 3) continue; int t = a * b + 1; if (t > 1000000) break; s += t + i; }
    return s;
}
static int dowhile(int a, int b) { int i = 0, s = 0; do { int t = a - b * 2; s += t; i++; } while (i < 5); return s; }
static int carried(int n) { int x = 0, y = 5; for (int i = 0; i < n; i++) { x = x + y; y = y + 1; int z = y * 3; x ^= z; } return x; }
static int jumpy(int n) {
    int s = 0, i = 0;
top:
    if (i >= n) goto done;
    { int inv = n * 5; s += inv + i; }
    i++;
    goto top;
done:
    return s;
}
static int scales(int n) {
    int r = 0;
    for (int i = 0; i < n; i++) { g1[i] = (char)(i * 3); g2[i] = (short)(i * 300); g4[i] = i * 70000; g8[i] = (long)i * 5000000000L; }
    for (int i = 0; i < n; i++) r += g1[i] + g2[i] + g4[i] + (int)(g8[i] >> 3);
    int *p = g4 + 32;
    for (int i = -5; i < 5; i++) { p[i] = i * 11; r += p[i] + p[-i]; }
    unsigned u = 7; g4[u] = 123; r += g4[u] + g4[u + 1];
    return r;
}
static int fields(struct S *a, int n) {
    int r = 0;
    for (int i = 0; i < n; i++) { a[i].c = (char)i; a[i].s = (short)(i * 2); a[i].i = i * 3; a[i].l = i * 4; for (int j = 0; j < 5; j++) a[i].arr[j] = i + j; }
    for (int i = 0; i < n; i++) r += a[i].c + a[i].s + a[i].i + (int)a[i].l + a[i].arr[i % 5] + a[i].arr[4];
    return r;
}
static int zero_iter(int n, int d) { int s = 1; for (int i = 0; i < n; i++) s += (7 % d) + i; return s; }
int main(void) {
    struct S st[8];
    int r = 0;
    r += div_guard(0, 0) + div_guard(3, 5);
    r += nested(4, 2) + nested(0, 9);
    r += cont_brk(10, 3, 4) + cont_brk(10, 1000, 1000);
    r += dowhile(9, 2);
    r += carried(7);
    r += jumpy(6);
    r += scales(40);
    r += fields(st, 8);
    r += zero_iter(0, 0) + zero_iter(3, 4);
    return r & 0xffffff;
}
