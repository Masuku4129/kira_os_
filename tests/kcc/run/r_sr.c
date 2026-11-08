/* Понижение силы умножения по индукционной переменной: шаги разных знаков и величин, 32- и 64-битные счётчики, continue/break/goto внутри
 * цикла, вложенные циклы, умножение до и после обновления счётчика, беззнаковые счётчики (не трогаются), отрицательные и большие множители. */
static int A[40][30];
static long L[64];
static unsigned char Bt[7 * 50];
int main(void) {
    long acc = 0;
    for (int i = 0; i < 40; i++) for (int j = 0; j < 30; j++) A[i][j] = i * 31 + j * 7 - 100;
    for (int i = 0; i < 40; i++) { int s = 0; for (int k = 0; k < 30; k++) s += A[i][k] * (k + 1); acc += s; }
    for (int j = 0; j < 30; j++) { int s = 0; for (int k = 0; k < 40; k++) s += A[k][j]; acc ^= s; }
    for (long i = 0; i < 64; i++) L[i] = i * 1000003L - i * 77;
    for (long i = 63; i >= 0; i -= 3) acc += L[i] * 12 + i * 35;              /* шаг -3, 64 бита */
    for (int i = 40; i > 0; i -= 2) acc += i * 100 + A[i - 1][i / 2] * 13;    /* шаг -2, 32 бита */
    for (int i = 0; i < 50; i++) for (int c = 0; c < 7; c++) Bt[i * 7 + c] = (unsigned char)(i * 3 + c);
    for (int i = 0; i < 50; i++) { if (i % 5 == 0) continue; if (i > 44) break; acc += Bt[i * 7 + 2] * 3 + i * 11; }
    for (int i = 0; i < 30; i++) { int x = i * 9; i += 1; acc += x + i * 9; }  /* умножение до и после обновления */
    for (unsigned u = 0; u < 20; u++) acc += (long)(u * 3000000000u) ^ (long)u;      /* беззнаковый счётчик */
    for (int i = -20; i < 20; i += 3) acc += (long)i * -12345 + i * 77;
    int i = 0;
    while (1) { acc += i * 52; if (i++ > 30) goto out; if (i == 7) continue; acc ^= i * 6; }
out:
    for (int i = 0; i < 12; i++) for (int j = i; j < 12; j++) acc += (i * 24) * (j * 10) + i * 13;
    return (int)(acc & 0xffffff);
}
