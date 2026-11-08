/* условия с вещественными: if/while/for/?:/!/&&/||; смешанные int/float/double, нуль -0.0 ложен. */
static int cnt;
static double dz = 0.0, dm = -0.0, dh = 0.5;
static float fz = 0.0f, fm = -0.0f, fh = 0.5f;
static int t1(double x) { if (x) return 1; return 0; }
static int t2(float x) { return x ? 10 : 20; }
static int t3(double x) { return !x; }
static int t4(double x, double y) { return x && y; }
static int t5(double x, float y) { return x || y; }
static int t6(double x) { int n = 0; while (x) { x -= 1; n++; if (n > 100) break; } return n; }
static int t7(double x) { int n = 0; for (; x > 0.25; x /= 2) n++; return n; }
static double pick(int c, double a, float b) { return c ? a : b; }
static double pick2(double c, int a, double b) { return c ? a : b; }
static float pick3(float c, float a, int b) { return !c ? a : b; }
int main(void) {
    int r = 0;
    r += t1(dz) + t1(dm) * 2 + t1(dh) * 4 + t1(-1.5) * 8 + t1(1e-300) * 16;
    r += (t2(fz) + t2(fm) + t2(fh) + t2(3.0f)) * 3;
    r += t3(dz) + t3(dm) + t3(dh) + t3(2.0);
    r += t4(1.0, 2.0) * 5 + t4(0.0, 2.0) * 7 + t4(dm, dh) * 11 + t4(dh, dh) * 13;
    r += t5(0.0, 0.0f) * 17 + t5(0.0, fh) * 19 + t5(dh, 0.0f) * 23;
    r += t6(5.0) + t6(0.5) + t7(100.0) * 3 + t7(0.1);
    r += (int)(pick(1, 2.5, 3.5f) * 4) + (int)(pick(0, 2.5, 3.5f) * 4) + (int)(pick2(dh, 7, 1.5) * 2) + (int)(pick2(dz, 7, 1.5) * 2);
    r += (int)(pick3(fz, 1.5f, 9) * 2) + (int)pick3(fh, 1.5f, 9);
    double a = 3; int k = 0;
    while (a) { a -= 1.5; k++; if (k > 10) break; }
    r += k;
    r += (dz < dh) + (fh > dz) * 2 + (1 < 1.5) * 4 + (2 >= 2.0f) * 8 + (3 == 3.0) * 16;
    r += (cnt++, dh) ? 100 : 200;
    r += cnt;
    return r;
}
