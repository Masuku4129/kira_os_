int a[12] = {9, 3, 7, 1, 8, 2, 6, 4, 12, 0, 11, 5};
int main(void) {
    int n = 12;
    for (int i = 0; i < n - 1; i++) for (int j = 0; j < n - 1 - i; j++) if (a[j] > a[j + 1]) { int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }
    int r = 0; for (int i = 0; i < n; i++) r = r * 3 + a[i];
    return r & 0xffffff;
}
