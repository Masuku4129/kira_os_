/* __builtin_alloca: блок живёт до выхода из функции; значения и выравнивание. */
static int use(int n) {
    char *p = __builtin_alloca(n);
    for (int i = 0; i < n; i++) p[i] = (char)(i * 7);
    long *q = __builtin_alloca(sizeof(long) * 4);
    for (int i = 0; i < 4; i++) q[i] = (long)i << 40;
    return p[n - 1] + (int)(q[3] >> 40) + (int)(((unsigned long)p | (unsigned long)q) & 15);
}
int main(void) {
    int t = 0;
    for (int i = 1; i < 200; i++) t += use(i);
    return t & 0xffffff;
}
