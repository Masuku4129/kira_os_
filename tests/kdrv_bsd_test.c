#include <stdint.h>
#include <stdio.h>
#define KIRA_NO_HEAP_GROW 1
#include "userland/kira.h"
void *k_heap_grow(u64 pages);
#include "userland/kdrv_bsd.h"

static uint8_t heap[16384] __attribute__((aligned(16)));
static uint64_t requested_pages;

void *k_heap_grow(u64 pages)
{
    requested_pages = pages;
    if (pages > sizeof heap / 4096u) return 0;
    return heap;
}

int main(void)
{
    if (kd_malloc(~0ull) != 0) {
        puts("FAIL: allocator accepted UINT64_MAX");
        return 1;
    }
    if (kd_malloc(~0ull - 4094ull) != 0 || requested_pages != 0) {
        puts("FAIL: overflow request reached heap allocator");
        return 1;
    }
    void *a = kd_malloc(33);
    if (a != heap || requested_pages != 4) {
        puts("FAIL: normal allocation/rounding");
        return 1;
    }
    void *b = kd_malloc(16);
    if (b != heap + 48) {
        puts("FAIL: bump pointer alignment");
        return 1;
    }
    puts("KDRV BSD allocator: overflow rejection and aligned allocation passed");
    return 0;
}
