/* kcc libc-lite: stdlib.h. malloc/free/calloc/realloc - из кучи программы (SYS_HEAP), блоки с заголовком, слияние свободных
   соседей при поиске. Всё в заголовке: в образ попадает только вызванное. Нет: strtod, system, getenv, atexit. */
#ifndef KCC_STDLIB_H
#define KCC_STDLIB_H
#include <stddef.h>
#include <kira.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

typedef struct kmh { size_t size, used; } kmh;     /* заголовок блока: 16 байт, данные идут сразу за ним */
static char *kheap_lo, *kheap_top, *kheap_lim;     /* начало, конец нарезанных блоков, конец полученной памяти */

static inline void *malloc(size_t n) {
    if (n == 0) n = 1;
    if (n > ((size_t)1 << 40)) return 0;
    n = (n + 15) & ~(size_t)15;
    for (char *p = kheap_lo; p < kheap_top;) {
        kmh *h = (kmh *)p;
        if (!h->used) {
            while (p + 16 + h->size < kheap_top && !((kmh *)(p + 16 + h->size))->used)
                h->size += 16 + ((kmh *)(p + 16 + h->size))->size;
            if (h->size >= n) {
                if (h->size >= n + 32) {
                    kmh *r = (kmh *)(p + 16 + n);
                    r->size = h->size - n - 16; r->used = 0;
                    h->size = n;
                }
                h->used = 1;
                return p + 16;
            }
        }
        p += 16 + h->size;
    }
    size_t need = n + 16;
    if ((size_t)(kheap_lim - kheap_top) < need) {
        size_t lack = need - (size_t)(kheap_lim - kheap_top);
        unsigned long pages = (unsigned long)((lack + 4095) / 4096), exact = pages;
        if (pages < 16) pages = 16;
        char *g = (char *)k_heap_grow(pages);
        if (!g && pages != exact) { pages = exact; g = (char *)k_heap_grow(pages); }
        if (!g) return 0;
        if (!kheap_lo) { kheap_lo = kheap_top = kheap_lim = g; }
        else if (g != kheap_lim) return 0;           /* куски кучи идут подряд; разрыв - отказ */
        kheap_lim = g + pages * 4096;
    }
    kmh *h = (kmh *)kheap_top;
    h->size = n; h->used = 1;
    kheap_top += need;
    return (char *)h + 16;
}
static inline void free(void *p) {
    if (!p) return;
    char *c = (char *)p;
    if (c < kheap_lo + 16 || c >= kheap_top) return;
    ((kmh *)(c - 16))->used = 0;
}
static inline void *calloc(size_t n, size_t m) {
    if (m && n > ((size_t)1 << 40) / m) return 0;
    size_t t = n * m;
    char *p = (char *)malloc(t);
    if (p) for (size_t i = 0; i < t; i++) p[i] = 0;
    return p;
}
static inline void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    kmh *h = (kmh *)((char *)p - 16);
    if (n <= h->size) return p;
    char *q = (char *)malloc(n);
    if (!q) return 0;
    for (size_t i = 0; i < h->size; i++) q[i] = ((char *)p)[i];
    free(p);
    return q;
}

static inline int abs(int v) { return v < 0 ? -v : v; }
static inline long labs(long v) { return v < 0 ? -v : v; }
static inline long atol(const char *s) {
    long v = 0; int neg = 0;
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static inline int atoi(const char *s) { return (int)atol(s); }
static inline void exit(int code) __attribute__((noreturn));
static inline void exit(int code) { k_exit(code); }
static inline void abort(void) __attribute__((noreturn));
static inline void abort(void) { k_exit(134); }

static inline unsigned long krand_state = 1;
static inline void srand(unsigned seed) { krand_state = seed; }
static inline int rand(void) { krand_state = krand_state * 6364136223846793005UL + 1442695040888963407UL; return (int)((krand_state >> 33) & 0x7fffffff); }

/* Пирамидальная сортировка: без рекурсии и без дополнительной памяти. */
static inline void kq_swap(unsigned char *a, unsigned char *b, size_t n) { while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; } }
static inline void kq_sift(unsigned char *base, size_t size, size_t i, size_t n, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && cmp(base + c * size, base + (c + 1) * size) < 0) c++;
        if (cmp(base + i * size, base + c * size) >= 0) return;
        kq_swap(base + i * size, base + c * size, size);
        i = c;
    }
}
static inline void qsort(void *vb, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *base = (unsigned char *)vb;
    if (n < 2 || !size) return;
    for (size_t i = n / 2; i-- > 0;) kq_sift(base, size, i, n, cmp);
    for (size_t e = n - 1; e > 0; e--) { kq_swap(base, base + e * size, size); kq_sift(base, size, 0, e, cmp); }
}
#endif
