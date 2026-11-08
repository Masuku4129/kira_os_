/* Оптимизатор: сужение при записи в переменную char/short/unsigned, сравнения без знака, адрес переменной берётся. */
static void bump(int *p) { *p += 5; }
int main(void) {
    signed char c = 120; unsigned char uc = 250; short sh = 32760; unsigned short us = 65530;
    for (int i = 0; i < 10; i++) { c += 3; uc += 3; sh += 3; us += 3; }
    unsigned a = 4000000000u, b = 5;
    int r = 0;
    if (a > b) r += 1;
    if (a < b) r += 2;
    if (c < 0) r += 4;
    if (sh < 0) r += 8;
    int k = 10;
    bump(&k);
    volatile int vv = 3;
    vv += 4;
    long big = -5; unsigned long ub = (unsigned long)big;
    if (ub > 100) r += 16;
    return (r + c + uc + sh + us + k + vv) & 0xffffff;
}
