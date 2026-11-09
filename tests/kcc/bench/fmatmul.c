static double A[120][120], B[120][120], C[120][120];
int main(void) {
    for (int i = 0; i < 120; i++) for (int j = 0; j < 120; j++) { A[i][j] = (i + j) * 0.5; B[i][j] = (i - j) * 0.25 + 1.0 / (i + j + 1); }
    double t = 0;
    for (int r = 0; r < 40; r++) {
        for (int i = 0; i < 120; i++) for (int j = 0; j < 120; j++) { double s = 0; for (int k = 0; k < 120; k++) s += A[i][k] * B[k][j]; C[i][j] = s; }
        t += C[r][r];
    }
    return (int)((long long)t & 0xffffff);
}
