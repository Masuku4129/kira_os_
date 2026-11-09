extern int printf(const char *, ...);
int main(int argc, char **argv) {
    volatile int n = 8 + (argc > 99);         /* значение известно только во время выполнения */
    int guard = 1111;
    int a[8];
    for (int i = 0; i < 8; i++) a[i] = i;
    for (int i = 0; i <= n + 3; i++) a[i] = 7777;   /* запись за границу массива на стеке: a[8..11] */
    printf("guard=%d a0=%d\n", guard, a[0]);
    return 0;
}
