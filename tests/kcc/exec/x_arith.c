#include "ktest.h"
static long fs(long a, long b) { return a / b * 1000 + a % b; }
static unsigned long fu(unsigned long a, unsigned long b) { return a / b * 1000 + a % b; }
int main(void) {
    int i = -17; unsigned u = 4000000000u; long l = -5000000000L; unsigned long ul = 18000000000000000000UL;
    short s = -3; unsigned short us = 65530; signed char c = -100; unsigned char uc = 250;
    pn(fs(-17, 5)); pn(fs(17, -5)); pn(fs(-17, -5)); pn((long)fu(ul, 7)); pn((long)fu(u, 13));
    pn(i / 4); pn(i % 4); pn(i >> 2); pn((unsigned)i >> 28); pn(i << 3); pn((long)(u + u)); pn((long)(u * 3));
    pn(l / 7); pn(l % 7); pn(l >> 3); pn((long)(ul >> 60)); pn(s * us); pn(c * uc); pn((char)(c - 100)); pn((unsigned char)(uc + 10));
    pn(c < uc); pn((unsigned)i < 5u); pn(i < 5); pn(-1L < 1UL); pn(~i); pn(-i); pn(!i); pn(!0);
    pn((int)(u ^ 0xf0f0f0f0u)); pn((short)(us + 10)); pn((i & 0xff) | 0x100); pn(i ^ 0x55);
    pn(1 ? 2 : 3); pn(0 && (i / 0)); pn(1 || (i / 0));
    long big = 1; for (int k = 0; k < 62; k++) big = big * 2 + (k & 1); pn(big); pn(big * big);
    return (int)(big & 255);
}
