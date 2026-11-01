#ifndef KCCRT_STDLIB_H
#define KCCRT_STDLIB_H
#include <stddef.h>
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
#endif
