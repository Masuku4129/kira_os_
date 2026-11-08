/* Свёртка смещения в обращение к памяти ([база + K], в том числе когда базу меняют между вычислением адреса и обращением) и слияние
 * расширения с загрузкой той же ширины. rdx в пуле: значения рядом с делением. */
typedef struct { long a; int b; short c; signed char d; unsigned char e; long f[4]; } St;
static St arr[5];
static long fieldsum(St *p, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) { p->a += i; p->b -= 3; p->c += 2; p->d = (signed char)(p->d + 7); p->e = (unsigned char)(p->e + 200); p->f[2] += p->f[1] + i; s += p->a + p->b + p->c + p->d + p->e + p->f[2]; p++; }
    return s;
}
static long moving(long *p, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) { long *q = p + 3; p = p + 1; s += *q * 3 + p[1]; *q = s & 0xff; }   /* q вычислен от старого p */
    return s;
}
static long between(long *p) {
    long *q = p + 2;
    long x = *q;
    p = p + 5;                        /* база изменилась, q - нет */
    long y = *q;
    *q = x + y + p[-1];
    return x * 7 + y + *q + p[0];
}
static int sextzext(const char *s, const unsigned char *u, const signed char *g, const short *sh, const unsigned short *us, int n) {
    int h = 17;
    for (int i = 0; i < n; i++) {
        h = h * 31 + (unsigned char)s[i];            /* char со знаком, читается как беззнаковый */
        h = h * 33 + (signed char)u[i];
        h = h * 35 + (unsigned char)g[i];
        h = h * 37 + (unsigned short)sh[i];
        h = h * 39 + (short)us[i];
        h ^= (int)(unsigned char)s[i] << 8;
    }
    return h;
}
static long divmix(long a, long b, long c) {
    long x1 = a + 1, x2 = b + 2, x3 = c + 3, x4 = a * 3, x5 = b * 5;
    long q = a / (b | 1), r = c % (a | 1);
    return x1 * q + x2 * r + x3 + x4 * x5 + q + r + (x1 ^ x2) + (x3 ^ x4);
}
int main(void) {
    long buf[16]; char cs[12]; unsigned char us8[12]; signed char gs[12]; short ss[12]; unsigned short uss[12];
    for (int i = 0; i < 16; i++) buf[i] = i * 11 - 40;
    for (int i = 0; i < 12; i++) { cs[i] = (char)(i * 29 + 120); us8[i] = (unsigned char)(i * 53 + 200); gs[i] = (signed char)(i * 71 + 100); ss[i] = (short)(i * 9001 - 20000); uss[i] = (unsigned short)(i * 7777 + 40000); }
    for (int i = 0; i < 5; i++) { arr[i].a = i; arr[i].b = 100 - i; arr[i].c = (short)(i * 1000); arr[i].d = (signed char)(120 + i); arr[i].e = (unsigned char)(250 + i); for (int k = 0; k < 4; k++) arr[i].f[k] = i * k; }
    long acc = fieldsum(arr, 5) + moving(buf, 10) + between(buf + 3) + sextzext(cs, us8, gs, ss, uss, 12);
    for (long i = 1; i < 20; i++) acc += divmix(i * 100003, i + 3, i * 7 - 50);
    return (int)(acc & 0x7fffff);
}
