/* Матрицы float и double: произведение, транспонирование, определитель и обратная матрица методом Гаусса; массивы массивов. */
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static void mf(float f) { union { float f; unsigned u; } x; x.f = f; mix(x.u); }
#define N 6
static double A[N][N], B[N][N], C[N][N], I[N][2 * N];
static float FA[N][N], FB[N][N], FC[N][N];
static void dmul(double a[N][N], double b[N][N], double c[N][N]) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { double s = 0; for (int k = 0; k < N; k++) s += a[i][k] * b[k][j]; c[i][j] = s; }
}
static void fmul_(float a[N][N], float b[N][N], float c[N][N]) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { float s = 0; for (int k = 0; k < N; k++) s += a[i][k] * b[k][j]; c[i][j] = s; }
}
static double det(double m[N][N]) {
    double t[N][N], d = 1;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) t[i][j] = m[i][j];
    for (int c = 0; c < N; c++) {
        int p = c;
        for (int r = c + 1; r < N; r++) if ((t[r][c] < 0 ? -t[r][c] : t[r][c]) > (t[p][c] < 0 ? -t[p][c] : t[p][c])) p = r;
        if (t[p][c] == 0) return 0;
        if (p != c) { for (int j = 0; j < N; j++) { double x = t[p][j]; t[p][j] = t[c][j]; t[c][j] = x; } d = -d; }
        d *= t[c][c];
        for (int r = c + 1; r < N; r++) { double f = t[r][c] / t[c][c]; for (int j = c; j < N; j++) t[r][j] -= f * t[c][j]; }
    }
    return d;
}
static int invert(double m[N][N]) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { I[i][j] = m[i][j]; I[i][N + j] = i == j; }
    for (int c = 0; c < N; c++) {
        int p = c;
        for (int r = c + 1; r < N; r++) if (I[r][c] * I[r][c] > I[p][c] * I[p][c]) p = r;
        if (I[p][c] == 0) return 0;
        for (int j = 0; j < 2 * N; j++) { double x = I[p][j]; I[p][j] = I[c][j]; I[c][j] = x; }
        double pv = I[c][c];
        for (int j = 0; j < 2 * N; j++) I[c][j] /= pv;
        for (int r = 0; r < N; r++) if (r != c) { double f = I[r][c]; for (int j = 0; j < 2 * N; j++) I[r][j] -= f * I[c][j]; }
    }
    return 1;
}
int main(void) {
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) {
        A[i][j] = (i * 7 + j * 3) % 11 / 4.0 + (i == j ? 5 : 0.125 * (j - i));
        B[i][j] = 1.0 / (i + j + 1);
        FA[i][j] = (float)A[i][j]; FB[i][j] = (float)B[i][j];
    }
    dmul(A, B, C);
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) md(C[i][j]);
    fmul_(FA, FB, FC);
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) mf(FC[i][j]);
    md(det(A)); md(det(B)); md(det(C));
    if (invert(A)) {
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) md(I[i][N + j]);
        double chk[N][N];
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) chk[i][j] = I[i][N + j];
        dmul(A, chk, C);
        double e = 0; for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { double dd = C[i][j] - (i == j); e += dd < 0 ? -dd : dd; }
        mix(e < 1e-9);
    }
    double tr = 0; for (int i = 0; i < N; i++) tr += A[i][i];
    md(tr);
    return (int)(H & 0x7fffffff);
}
