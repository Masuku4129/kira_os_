unsigned pop(unsigned long long v) { unsigned n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
unsigned rotl32(unsigned v, int k) { return (v << k) | (v >> (32 - k)); }
unsigned char rotl8(unsigned char v, int k) { return (unsigned char)((v << k) | (v >> (8 - k))); }
int main(void) {
    return pop(0xFFFFFFFFFFFFFFFFULL) + pop(0x1234) + (int)(rotl32(0x80000001u, 4) & 0xffff) + rotl8(0x81, 1) + (int)((~0u) >> 28) + (int)(1ULL << 40 >> 38);
}
