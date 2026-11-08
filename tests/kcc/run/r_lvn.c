/* Локальная нумерация значений: повторные вычисления и загрузки сливаются только пока ничего не изменилось. Запись через указатель, вызов,
 * изменение операнда и volatile не должны давать устаревших значений. */
static int g[16];
static volatile int vol;
static int bump(int *p) { *p += 5; g[3] += 2; return *p; }
static int sum(const int *a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i] * 3 + a[i]; return s; }
int main(void) {
    int w[16];
    for (int i = 0; i < 16; i++) { w[i] = i * i + 1; g[i] = i; }
    long acc = 0;
    for (int i = 2; i < 14; i++) {
        int x = w[i - 1] + w[i - 1];            /* повтор загрузки и вычитания индекса */
        int *p = &w[i - 1];
        *p = x + 1;                              /* запись через указатель: следующая загрузка новая */
        int y = w[i - 1] * 2;
        acc += x + y + (w[i + 1] >> 1) + (w[i + 1] << 2);
        acc += bump(&w[i]) + w[i];               /* вызов меняет память */
        acc += g[3] + g[3];
        g[3] = g[3] + 1;
        acc += g[3];
        vol = vol + 1; acc += vol + vol;         /* volatile читается каждый раз */
    }
    int k = 3;
    int a1 = k * 7 + w[5];
    k = k + 1;
    int a2 = k * 7 + w[5];                       /* операнд изменился: не повтор */
    acc += a1 * 100 + a2 + sum(w, 16);
    return (int)(acc & 0xffffff);
}
