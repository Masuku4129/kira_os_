/* Распознавание циклического сдвига: 32 и 64 бита, обе стороны, и случаи, которые вращением быть не должны (знаковые, сумма не равна ширине). */
static unsigned rotl32(unsigned x, int n) { return (x << n) | (x >> (32 - n)); }
static unsigned rotr32(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }
static unsigned long rotl64(unsigned long x, int n) { return (x << n) | (x >> (64 - n)); }
int main(void) {
    unsigned a = 0x12345678u, r = 0;
    unsigned long b = 0x0123456789abcdefUL, rb = 0;
    for (int i = 0; i < 40; i++) {
        a = (a >> 7 | a << 25) ^ (unsigned)i;
        a = ((a << 13) | (a >> 19)) + 0x9e3779b9u;
        a = rotl32(a, 5) ^ rotr32(a, 11);
        b = ((b << 17) | (b >> 47)) ^ (unsigned long)i;
        b = (b >> 3 | b << 61) + 77;
        b = rotl64(b, 29);
        r += a;
        rb ^= b;
    }
    int s = (int)0x80000001u;                          /* знаковый сдвиг вправо арифметический: не вращение */
    int t = (s >> 4) | (s << 28);
    unsigned u = 0xf0f0f0f0u;
    unsigned w = (u >> 5) | (u << 20);                 /* 5 + 20 != 32 */
    unsigned long z = (b >> 8) | ((unsigned long)(unsigned)b << 8);
    return (int)((r ^ (unsigned)rb ^ (unsigned)(rb >> 32) ^ (unsigned)t ^ w ^ (unsigned)z) & 0xffffff);
}
