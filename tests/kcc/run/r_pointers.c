int a[10]; int m[3][4];
int main(void) {
    int *p = a, s = 0;
    for (int i = 0; i < 10; i++) *(p + i) = i * i;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) m[i][j] = i * 10 + j;
    int *q = &a[7];
    s = (int)(q - p) + *q + p[3] + 2[a] + m[2][3] + (*(m + 1))[2] + **m;
    p += 4; p -= 1; s += *p + (int)(&a[9] - &a[1]);
    return s;
}
