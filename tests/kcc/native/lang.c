/* Расширения языка kcc на Кире: атомарные операции GCC (atomic и sync), struct по значению (параметры и результаты), va_list, case-диапазоны, ?: без среднего, printf.
   Программа переносима: те же строки печатает clang на хосте (tests/kcc/native/lang.out получен так). */
#include <stdio.h>
#include <stdarg.h>

struct small { int a, b; };
struct pair { long x, y; };
struct big { long v[5]; };

static struct small mk_small(int a) { struct small s = { a, a * 2 }; return s; }
static struct pair mk_pair(long x) { struct pair p = { x, -x }; return p; }
static struct big mk_big(long x) { struct big b; for (int i = 0; i < 5; i++) b.v[i] = x + i; return b; }
static long use_all(struct small s, struct pair p, struct big b, int tail) {
    long t = s.a + s.b + p.x + p.y + tail;
    for (int i = 0; i < 5; i++) t += b.v[i];
    return t;
}

static int sum_ints(int n, ...) {
    va_list ap; va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static const char *classify(int c) {
    switch (c) {
    case 0: return "zero";
    case 1 ... 9: return "digit";
    case 10 ... 99: return "two-digit";
    default: return "big";
    }
}

int main(void) {
    unsigned counter = 0;
    for (int i = 0; i < 10; i++) __atomic_fetch_add(&counter, (unsigned)i, __ATOMIC_SEQ_CST);
    unsigned expected = 45, desired = 100;
    int ok1 = __atomic_compare_exchange_n(&counter, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    expected = 7;
    int ok2 = __atomic_compare_exchange_n(&counter, &expected, 5, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    long lk = 0;
    unsigned seen = expected;
    unsigned old = __atomic_exchange_n(&counter, 9u, __ATOMIC_SEQ_CST);
    long sy = __sync_add_and_fetch(&lk, 41L) + 1;
    printf("atomic: counter=%u ok1=%d ok2=%d seen=%u xchg=%u sync=%ld\n", counter, ok1, ok2, seen, old, sy);
    long ua = use_all(mk_small(3), mk_pair(10), mk_big(100), 7);
    int sb = mk_small(21).b;
    long py = mk_pair(5).y;
    printf("struct: %ld %d %ld\n", ua, sb, py);
    int v1 = sum_ints(3, 10, 20, 30), v2 = sum_ints(9, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    printf("varargs: %d %d\n", v1, v2);
    printf("case: %s %s %s %s\n", classify(0), classify(5), classify(42), classify(1000));
    int z = 0, nz = 6;
    printf("elvis: %d %d\n", z ?: 11, nz ?: 11);
    printf("fmt: [%5d][%-5s][%05x][%c]\n", 42, "ab", 255, 'k');
    return 0;
}
