/* Цикл подсчёта единичных битов x &= x - 1 распознаётся и заменяется битовой последовательностью (без popcnt); похожие циклы с другим шагом,
 * знаковым x, дополнительными действиями или чтением x после цикла дают прежний результат. */
static int pop64(unsigned long x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }
static int pop32(unsigned x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }
static long popl(unsigned long x, long base) { long c = base; while (x) { x = x & (x - 1); c = c + 1; } return c; }
static unsigned popafter(unsigned long x, unsigned long *left) { unsigned c = 0; while (x) { x &= x - 1; c++; } *left = x; return c; }
static int step2(unsigned long x) { int c = 0; while (x) { x &= x - 1; c += 2; } return c; }
static int sub2(unsigned long x) { int c = 0; while (x) { x &= x - 2; c++; if (c > 70) break; } return c; }
static int signedx(int x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }
static int extra(unsigned long x, int *cnt) { int c = 0; while (x) { x &= x - 1; c++; (*cnt)++; } return c; }
static int withoutc(unsigned long x) { int k = 0; unsigned long y = x; while (y) { y &= y - 1; k++; } return k + (int)(x & 1); }
static int nested(unsigned long a, unsigned long b) { int t = 0; for (int i = 0; i < 3; i++) { unsigned long x = a >> i; int c = 0; while (x) { x &= x - 1; c++; } t += c * (i + 1); x = b << i; while (x) { x &= x - 1; t++; } } return t; }
int main(void) {
    unsigned long vals[] = { 0, 1, 2, 3, 0xff, 0x8000000000000000ul, 0xffffffffffffffff, 0x123456789abcdef0ul, 0x5555555555555555ul, 0xdeadbeefcafebabeul, 0x7fffffff, 0x80000000ul, 0xffffffff00000000ul };
    long acc = 0; int cnt = 0;
    for (int i = 0; i < 13; i++) {
        unsigned long v = vals[i], left = 99;
        acc += pop64(v) * 3 + pop32((unsigned)v) * 5 + popl(v, i) * 7 + popafter(v, &left) + (long)left * 11 + step2(v) + sub2(v) + signedx((int)v) + extra(v, &cnt) + withoutc(v) + nested(v, ~v);
    }
    return (int)((acc + cnt) & 0x7fffff);
}
