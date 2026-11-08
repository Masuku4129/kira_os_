/* VLA: локальные массивы, sizeof времени выполнения, многомерные, указатели на VLA, параметры, typedef в блоке, break/continue/goto, рекурсия. */
struct P { int a; char c; double d; };
static int sum(int n, int a[n]) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
static int sum2(int n, int m, int a[n][m]) {
    int s = 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < m; j++) s += a[i][j] * (i + 1);
    return s + (int)sizeof(a[0]) + (int)sizeof(*a);
}
static int sum3(int n, int a[][n], int rows) { int s = 0; for (int i = 0; i < rows; i++) for (int j = 0; j < n; j++) s += a[i][j]; return s; }
static int rec(int n) {
    if (n == 0) return 0;
    char buf[n + 3];
    for (int i = 0; i < n + 3; i++) buf[i] = (char)(i + n);
    return buf[n + 2] + rec(n - 1) + (int)sizeof buf;
}
static int leak_loop(int rounds) {                 /* без освобождения стека на выходе из блока 20000 * 4 КБ переполнили бы стек */
    int t = 0;
    for (int r = 0; r < rounds; r++) {
        int n = 1000 + r % 7;
        long big[n];
        for (int i = 0; i < n; i++) big[i] = i + r;
        t += (int)(big[n - 1] & 0xff);
        if (r % 3 == 0) continue;
        if (r == rounds - 2) break;
        t++;
    }
    return t;
}
static int goto_loop(int rounds) {
    int t = 0, r = 0;
again:
    {
        int n = 800 + r % 5;
        double d[n];
        for (int i = 0; i < n; i++) d[i] = i * 0.5;
        t += (int)d[n - 1];
        r++;
        if (r < rounds) goto again;
    }
    return t;
}
static int sw(int k, int n) {
    int t = 0;
    switch (k) {
    case 1: { int a[n]; for (int i = 0; i < n; i++) a[i] = i; t = a[n - 1]; break; }
    case 2: { char a[n * 2]; t = (int)sizeof a; break; }
    default: t = -1;
    }
    return t;
}
int main(void) {
    int total = 0, n = 7;
    int a[n];
    for (int i = 0; i < n; i++) a[i] = i * i;
    total += sum(n, a) + (int)sizeof a + (int)(sizeof(int[n + 1]));
    int m = 4, w = 3;
    int g[m][w];
    for (int i = 0; i < m; i++) for (int j = 0; j < w; j++) g[i][j] = i + j * 5;
    total += sum2(m, w, g) + sum3(w, g, m);
    typedef int row_t[w + 1];
    row_t r1, r2;
    for (int i = 0; i <= w; i++) { r1[i] = i; r2[i] = i * 2; }
    total += r1[w] + r2[w] + (int)sizeof(row_t) + (int)sizeof r1;
    int (*pr)[w] = g;
    pr++;
    total += pr[0][1] + (int)(pr - g) + (int)(&g[3] - &g[1]);
    int (*q)[w] = (int (*)[w])g;
    total += q[2][2] + (int)sizeof(*q);
    struct P ps[n];
    for (int i = 0; i < n; i++) { ps[i].a = i; ps[i].c = (char)i; ps[i].d = i * 0.25; }
    total += ps[6].a + ps[5].c + (int)ps[4].d + (int)sizeof(ps) + (int)sizeof(struct P);
    int cnt = 0;
    int k = 3;
    int se[k++];                                        /* размер вычисляется один раз */
    total += (int)sizeof(se) + k;
    (void)cnt;
    int d3[2][n][3];
    for (int i = 0; i < 2; i++) for (int j = 0; j < n; j++) for (int l = 0; l < 3; l++) d3[i][j][l] = i * 100 + j * 10 + l;
    total += d3[1][6][2] + d3[0][3][1] + (int)sizeof d3;
    {
        int inner[n * 2];
        for (int i = 0; i < n * 2; i++) inner[i] = i;
        total += inner[n * 2 - 1];
        { char c2[n + 1]; c2[n] = 9; total += c2[n]; }
        total += (int)sizeof inner;
    }
    total += rec(12) + leak_loop(20000) + goto_loop(20000) + sw(1, 9) + sw(2, 9) + sw(3, 1);
    double dd[n];
    for (int i = 0; i < n; i++) dd[i] = i / 4.0;
    total += (int)(dd[6] * 100);
    return total & 0xffffff;
}
