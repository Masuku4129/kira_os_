/* Поворот (x>>c1)|(x<<c2) у переменной с несколькими определениями: распознаётся, только если между сдвигами и OR она не менялась. */
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static unsigned mixed(unsigned a, unsigned b, unsigned e, unsigned f) {
    unsigned h = 0;
    for (int i = 0; i < 20; i++) {
        unsigned s1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);        /* e - переменная цикла: поворот */
        unsigned t = (e >> 7);
        e = e + a * (unsigned)i + 1u;                              /* e изменилась между сдвигами: это не поворот */
        unsigned u = (e << 25);
        h += s1 + (t | u);
        a = a + f; f = f ^ s1; e ^= b; b += 3;
    }
    return h;
}
static unsigned long rot64(unsigned long x, int n) {
    unsigned long r = 0;
    for (int i = 0; i < 10; i++) { r ^= (x >> 13) | (x << 51); x = x * 6364136223846793005UL + 1442695040888963407UL; r += (x >> 40) | (x << 24); }
    return r + n;
}
int main(void) {
    unsigned acc = 0;
    for (unsigned i = 1; i < 30; i++) acc = acc * 31u + mixed(i, i * 3u, i * 0x9e3779b1u, i + 7u);
    unsigned long z = rot64(0x123456789abcdefUL, 3);
    return (int)((acc ^ (unsigned)z ^ (unsigned)(z >> 32)) & 0x7fffff);
}
