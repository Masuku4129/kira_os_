extern int printf(const char *, ...);
extern void *malloc(unsigned long); extern void free(void *);
int main(int argc, char **argv) {
    volatile int n = 8 + (argc > 99);
    int *a = malloc(8 * sizeof(int)); int *b = malloc(8 * sizeof(int));
    for (int i = 0; i < 8; i++) { a[i] = i; b[i] = 100 + i; }
    for (int i = 0; i <= n + 3; i++) a[i] = 7777;   /* запись за границу блока кучи на 4 int */
    printf("b0=%d b1=%d\n", b[0], b[1]);
    free(a); free(b);
    return 0;
}
