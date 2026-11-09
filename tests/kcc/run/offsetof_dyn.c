/* offsetof с индексом, не являющимся константой (расширение GNU, его использует fxp(4) из OpenBSD): смещение считается во время выполнения. */
struct S { int a; struct { char c[3]; int d[4]; } b[5]; short tail; };
unsigned long off(int i, int j) { return __builtin_offsetof(struct S, b[i].d[j]); }
unsigned long off2(int i) { return __builtin_offsetof(struct S, b[i]); }
int main(void) {
    struct S s;
    for (int i = 0; i < 5; i++) {
        if (off2(i) != (unsigned long)((char *)&s.b[i] - (char *)&s)) return 1 + i;
        for (int j = 0; j < 4; j++)
            if (off(i, j) != (unsigned long)((char *)&s.b[i].d[j] - (char *)&s)) return 10 + i * 4 + j;
    }
    if (__builtin_offsetof(struct S, b[2].d[1]) != (unsigned long)((char *)&s.b[2].d[1] - (char *)&s)) return 60;   /* константный путь по-прежнему работает */
    return 0;
}
