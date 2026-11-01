union U { unsigned u; unsigned char c[4]; short s[2]; };
int main(void) {
    union U x; x.u = 0x11223344;
    union U y = {0x01020304};
    return x.c[0] + x.c[3] * 2 + x.s[1] + y.c[1] * 100 + (int)sizeof(union U);
}
