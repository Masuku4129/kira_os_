/* Перестановка слагаемых адреса в циклах: (база + k*строка) + j*4 -> (база + j*4) + k*строка. Неизменное в цикле слагаемое вычисляется
 * позже по тексту, слагаемое меняется в самом цикле, запись по адресу, разные ширины строки, вложенность 3-4 уровня. */
static int A[12][12], B[12][12], C[12][12];
static long D[5][7][3];
static short E[9][9];
static long colsum(int j) { long s = 0; for (int k = 0; k < 12; k++) s += B[k][j] * 3; return s; }
static long varying(int j) { long s = 0; for (int k = 0; k < 12; k++) s += B[k][(k + j) & 7]; return s; }       /* второй индекс меняется в цикле */
static long both(int i, int j) { long s = 0; for (int k = 0; k < 12; k++) s += A[i][k] * B[k][j]; return s; }
static void fill(int j, int v) { for (int k = 0; k < 12; k++) C[k][j] = v + k; }                              /* запись по переставленному адресу */
static long cube(int a, int b) { long s = 0; for (int i = 0; i < 5; i++) for (int j = 0; j < 7; j++) for (int k = 0; k < 3; k++) s += D[i][j][k] * (a + i) + D[i][b % 7][k]; return s; }
static long shorts(int j) { long s = 0; for (int i = 0; i < 9; i++) s += E[i][j] + E[j][i]; return s; }
static long later(const long *base, int n, int j) { long s = 0; for (int k = 0; k < n; k++) { const long *row = base + k * 3; s += row[j] + row[(j + 1) % 3]; } return s; }
int main(void) {
    long x[24];
    for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) { A[i][j] = i * 3 + j; B[i][j] = i - j * 2; C[i][j] = 0; }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 7; j++) for (int k = 0; k < 3; k++) D[i][j][k] = i * 100 + j * 10 + k;
    for (int i = 0; i < 9; i++) for (int j = 0; j < 9; j++) E[i][j] = (short)(i * j - 20);
    for (int i = 0; i < 24; i++) x[i] = i * i - 7;
    long acc = 0;
    for (int j = 0; j < 12; j++) { acc += colsum(j) + varying(j); fill(j, j * 7); }
    for (int i = 0; i < 12; i += 5) for (int j = 0; j < 12; j += 3) acc += both(i, j);
    for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) acc += C[i][j] * (i + 1);
    acc += cube(3, 12) + shorts(4) + shorts(8);
    for (int j = 0; j < 3; j++) acc += later(x, 8, j);
    return (int)(acc & 0x7fffff);
}
