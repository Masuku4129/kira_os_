#include "ktest.h"
/* давление на регистры: много одновременно живых временных значений, вложенные циклы, значения, живущие через обратные переходы */
static long deep(long a, long b, long c, long d, long e, long f, long g, long h) {
    return a * (b + (c * (d + (e * (f + (g * (h + (a * (b + (c * (d + (e * (f + (g * (h + a)))))))))))))));
}
static long wide(long a, long b, long c, long d) {
    return (a + 1) * (b + 2) + (c + 3) * (d + 4) - (a ^ b) * (c | d) + ((a & c) << 2) - ((b | d) >> 1) + (a + b) * (c + d) * (a - d) + (b - c) * (b + c) * (a + 7);
}
int main(void) {
    pn(deep(1, 2, 3, 4, 5, 6, 7, 8));
    pn(deep(-3, 5, -7, 11, -13, 17, -19, 23));
    pn(wide(3, 5, 7, 11)); pn(wide(-100, 200, -300, 400));
    long acc = 0;
    for (int i = 0; i < 50; i++) {
        long t1 = i * 3, t2 = i * 5 + 1, t3 = t1 ^ t2, t4 = t3 + acc;
        for (int j = 0; j < 7; j++) { t1 += j; t2 -= j * t3; t4 ^= t1 + t2; if (t4 & 1) continue; acc += t4 >> 3; }
        acc += t1 + t2 + t3 + t4;
    }
    pn(acc);
    int x = 10, y = 20, z = 30;
    int r = (x < y) + (y < z) * 2 + (x == 10 && y == 20) * 4 + (x > y || z == 30) * 8 + ((x < y ? y : x) > z) * 16;
    pn(r);
    return 0;
}
