/* принимаются: все формы вещественных констант, типов и выражений */
float f1 = 1.5f, f2 = 1e3F, f3 = .25f, f4 = 0x1.8p1f;
double d1 = 1.5, d2 = 1e-3, d3 = 5., d4 = 0x1p-2, d5 = 1E+10, d6 = 0.0;
double arr[3] = { 1, 2.5, -3 };
struct P { float x; double y; } pt = { 1, 2 };
static double sq(double x) { return x * x; }
double (*fp)(double) = sq;
int cond(double x, float y) { return x ? (y > 0) : !y; }
double mix(int i, unsigned u, long long l, unsigned long long ul, float f, double d) {
    return i + u + l + ul + f + d + (i < f) + (d == l) - -d;
}
double sel(int c, float a, double b) { return c ? a : b; }
float neg(float x) { return -x + +x; }
int main(void) {
    double x = 2;
    x += 1; x *= 2; x -= 0.5; x /= 3; x++; --x;
    float y = (float)x;
    return (int)y + (int)sizeof(float) + (int)sizeof(double) + (int)sizeof 1.0f + (int)sizeof(1.0 + 1.0f);
}
