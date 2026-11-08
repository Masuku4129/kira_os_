/* Встраивание: разворачивание рекурсии на один уровень (локальные, глобальные, взаимная рекурсия, адрес локальной) и статические функции
 * с единственным вызовом (циклы, несколько return, массив в локальных, адрес функции взят отдельно). */
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int depth;
static int maxdepth;
static long walk(long n, long acc) { depth++; if (depth > maxdepth) maxdepth = depth; long r; if (n <= 0) r = acc; else r = walk(n - 1, acc + n * 3) + (n & 1); depth--; return r; }
static int ack(int m, int n) { if (m == 0) return n + 1; if (n == 0) return ack(m - 1, 1); return ack(m - 1, ack(m, n - 1)); }
static int odd(int n);
static int even(int n) { return n == 0 ? 1 : odd(n - 1); }
static int odd(int n) { return n == 0 ? 0 : even(n - 1); }
static int sumarr(int n) { int a[8]; int s = 0; for (int i = 0; i < 8; i++) a[i] = i * n; for (int i = 0; i < 8; i++) s += a[i]; if (n > 3) { int *p = &a[n & 7]; *p += 100; s += *p; } return s; }
static long rec_addr(int n, long *out) { long loc = n * 10; if (n > 0) rec_addr(n - 1, &loc); *out += loc; return loc; }
static int once(int x) { int r = 0; for (int i = 0; i < x; i++) { if (i == 7) return r * 2; r += i; } return r; }
static int twice(int x) { return x * 3 + 1; }
static int viaptr(int x) { return x - 4; }
static int (*volatile fp)(int) = viaptr;
static int taken(int x) { return x + 11; }
static int (*volatile fp2)(int) = taken;
static unsigned gcdrec(unsigned a, unsigned b) { return b == 0 ? a : gcdrec(b, a % b); }
static long pow2(long b, int e) { if (e == 0) return 1; long h = pow2(b, e / 2); return e & 1 ? h * h * b : h * h; }
/* обёртка и её единственный "внутренний" вызов: тело обёртки копируется во многие места, поэтому вызовов внутренней функции становится много */
static int wdepth;
static int wrap(int n);
static int use1(int a) { return wrap(a) + 1; }
static int use2(int a) { return wrap(a + 2) * 2; }
static int use3(int a, int b) { return wrap(a) - wrap(b); }
static int wrap_(int n);
static int wrap(int n) { wdepth++; int r = wrap_(n); wdepth--; return r; }
static int wrap_(int n) { return n <= 0 ? 0 : wrap(n - 1) + n * 2; }
static int vdepth;
static int vrap(int n);
static int vrap_(int n);
static int vrap(int n) { vdepth++; int r = vrap_(n); vdepth--; return r; }
static int vrap_(int n) { return n <= 0 ? 0 : vrap(n - 1) + n * 3; }
static int vse1(int a) { return vrap(a) + 1; }
static int vse2(int a) { return vrap(a + 2) * 2; }
static int vse3(int a, int b) { return vrap(a) - vrap(b); }
static int zdepth;
static int zrap(int n);
static int zrap_(int n);
static int zrap(int n) { zdepth++; int r = zrap_(n); zdepth--; return r; }
static int zrap_(int n) { return n <= 0 ? 0 : zrap(n - 1) + n * 5; }
static int zse1(int a) { return zrap(a) + 1; }
static int zse2(int a) { return zrap(a + 2) * 2; }
static int zse3(int a, int b) { return zrap(a) - zrap(b); }
int main(void) {
    long acc = 0;
    for (int i = 0; i < 18; i++) acc += fib(i);
    acc += walk(20, 1) + maxdepth * 1000;
    acc += ack(2, 3) + ack(3, 2);
    for (int i = 0; i < 9; i++) acc += even(i) * 10 + odd(i);
    for (int i = 0; i < 12; i++) acc += sumarr(i);
    long z = 0; acc += rec_addr(5, &z) + z;
    acc += once(5) + once(12) + twice(4) + twice(acc & 15) + fp(9) + fp2(2) + taken(3);
    acc += use1(5) + use2(4) + use3(7, 3) + wrap(6) + wdepth + vse1(5) + vse2(4) + vse3(7, 3) + vrap(6) + vdepth + zse1(5) + zse2(4) + zse3(7, 3) + zrap(6) + zdepth;
    acc += gcdrec(1071, 462) + gcdrec(17, 5) + pow2(3, 11) % 1000003;
    return (int)(acc & 0x7fffff);
}
