extern int printf(const char *, ...);
extern void *malloc(unsigned long); extern void free(void *);
int main(void) {
    int *p = malloc(64); p[0] = 1234; free(p);
    int *q = malloc(64); q[0] = 5678;        /* то же место, скорее всего */
    printf("p[0]=%d (после free) q[0]=%d same=%d\n", p[0], q[0], p == q);   /* чтение через висящий указатель */
    free(q);
    return 0;
}
