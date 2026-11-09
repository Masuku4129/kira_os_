extern int printf(const char *, ...);
int main(int argc, char **argv) {
    volatile int x = 2147483647 - (argc > 99);
    int y = x + 1;                               /* знаковое переполнение: UB в C */
    int t = (x + 1 > x);                          /* компилятор вправе считать всегда истинным */
    printf("y=%d (x+1>x)=%d\n", y, t);
    return 0;
}
