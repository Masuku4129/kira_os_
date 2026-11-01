int main(void) {
    int a = 100; unsigned b = 100; long c = 100; unsigned char d = 100; short e = 100; char f = 100; unsigned long long g = 100;
    a *= 3; a /= 7; a %= 11; a <<= 3; a >>= 1; a ^= 0x55; a |= 0x100; a &= 0x1FF; a -= 500;
    b *= 50000000; b /= 3; b >>= 2; b -= 1000000000; b += 7;
    c <<= 33; c >>= 5; c -= 12345678901L;
    d += 200; d *= 3; d -= 1; e *= 400; e >>= 2; f += 100; f /= 2; f -= 100;
    g -= 101; g /= 3; g >>= 1;
    int *p = &a; p += 0; p -= 0;
    return a + (int)b + (int)(c & 0x7fffffff) + d + e + f + (int)(g & 0xffff) + (*p == a);
}
