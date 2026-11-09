extern int printf(const char *, ...);
typedef unsigned long pthread_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
static volatile long counter;                   /* общий счётчик без синхронизации */
static void *w(void *a) { (void)a; for (int i = 0; i < 2000000; i++) counter++; return 0; }
int main(void) {
    pthread_t t1, t2; pthread_create(&t1, 0, w, 0); pthread_create(&t2, 0, w, 0);
    pthread_join(t1, 0); pthread_join(t2, 0);
    printf("counter=%ld (ожидалось 4000000)\n", counter);
    return 0;
}
