int main(void) {
    signed char sc = -5; unsigned char uc = 250; short ss = -300; unsigned short us = 65000;
    int i = sc + uc + ss + us;
    unsigned u = sc; long l = sc; unsigned long ul = uc;
    char c = 300; unsigned char c2 = -1; short s2 = 70000;
    int d = (-7) / 2 + (-7) % 3 + 7 / -2 + 7 % -3;
    unsigned x = 0xF0000000u; int y = (int)x >> 28; unsigned z = x >> 28;
    return i + (int)(u >> 24) + (int)l + (int)ul + c + c2 + s2 + d + y + (int)z;
}
