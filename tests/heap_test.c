// Хост-тест кучи ядра поверх модели менеджера страниц: классы размеров, крупные блоки, таблица владения,
// возврат памяти, отказ при нехватке, защита от чужих указателей и повторного освобождения, длинная случайная нагрузка.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "heap.h"
#include "pmm.h"

void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }

// ---- модель PMM: пул страниц, выдача по одной и подряд ----
#define POOL 8192
static uint8_t* pool;
static uint8_t used[POOL];
static unsigned free_count = POOL, limit = POOL;     // limit - сколько страниц разрешено выдать (проверка нехватки)
static unsigned given(void) { return POOL - free_count; }
uint64_t pmm_alloc_contiguous(uint32_t n) {
    if (given() + n > limit) return 0;
    for (unsigned s = 0; s + n <= POOL; s++) {
        unsigned k = 0; while (k < n && !used[s + k]) k++;
        if (k == n) { for (k = 0; k < n; k++) used[s + k] = 1; free_count -= n; return (uint64_t)(uintptr_t)(pool + (size_t)s * 4096); }
        s += k;
    }
    return 0;
}
uint64_t pmm_alloc_page(void) { return pmm_alloc_contiguous(1); }
int pmm_free_contiguous(uint64_t pa, uint32_t n) {
    size_t s = (size_t)((uint8_t*)(uintptr_t)pa - pool) / 4096;
    for (uint32_t k = 0; k < n; k++) { if (!used[s + k]) return -1; used[s + k] = 0; free_count++; }
    return 0;
}
int pmm_free_page(uint64_t pa) { return pmm_free_contiguous(pa, 1); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void) {
    pool = aligned_alloc(4096, (size_t)POOL * 4096);
    heap_init();
    unsigned base_pages = given();                         // таблица владения
    heap_stats_t s;

    // выравнивание и полезный размер у всех классов и у крупных блоков
    for (size_t sz = 1; sz <= 9000; sz += (sz < 64 ? 1 : 37)) {
        void* p = kmalloc(sz);
        CHECK(p && ((uintptr_t)p & 15) == 0);
        CHECK(kmalloc_usable(p) >= sz);
        if (sz > HEAP_SLAB_MAX) CHECK(((uintptr_t)p & 4095) == 0);
        memset(p, 0xA5, sz);
        kfree(p);
    }
    heap_get_stats(&s);
    CHECK(s.used_blocks == 0 && s.used_bytes == 0 && s.large_pages == 0);
    CHECK(s.slab_pages <= 8);                              // по одной пустой плите на класс в запасе, не больше

    // ошибки освобождения
    unsigned bad0 = s.bad_frees;
    void* p = kmalloc(100); kfree(p); kfree(p);            // повтор
    int local; kfree(&local);                              // чужой адрес
    void* big = kmalloc(3 * 4096); kfree((uint8_t*)big + 4096);   // середина крупного блока
    void* sm = kmalloc(40); kfree((uint8_t*)sm + 8);       // середина мелкого
    heap_get_stats(&s);
    CHECK(s.bad_frees == bad0 + 4);
    CHECK(s.used_blocks == 2);                             // big и sm живы и целы
    kfree(big); kfree(sm);

    // krealloc сохраняет содержимое
    uint8_t* r = kmalloc(10); for (int i = 0; i < 10; i++) r[i] = (uint8_t)i;
    r = krealloc(r, 5000); CHECK(r != 0);
    for (int i = 0; i < 10; i++) CHECK(r[i] == (uint8_t)i);
    kfree(r);

    // рост таблицы владения: тысячи плит и крупных блоков одновременно
    enum { MANY = 3000 };
    void** v = calloc(MANY, sizeof(void*));
    for (int i = 0; i < MANY; i++) { v[i] = kmalloc(i % 3 ? 2000 : 4097); CHECK(v[i] != 0); }
    for (int i = 0; i < MANY; i++) kfree(v[i]);
    heap_get_stats(&s);
    CHECK(s.used_blocks == 0 && s.large_pages == 0 && s.bad_frees == bad0 + 4);

    // случайная нагрузка с проверкой содержимого (соседи не портят друг друга)
    enum { LIVE = 600 };
    uint8_t** blk = calloc(LIVE, sizeof(void*)); size_t* len = calloc(LIVE, sizeof(size_t)); uint8_t* tagv = calloc(LIVE, 1);
    for (int step = 0; step < 200000; step++) {
        int i = (int)(rnd() % LIVE);
        if (blk[i]) {
            for (size_t k = 0; k < len[i]; k += 97) if (blk[i][k] != tagv[i]) { CHECK(0); break; }
            kfree(blk[i]); blk[i] = 0;
        } else {
            size_t sz = (rnd() & 7) == 0 ? 1 + rnd() % 20000 : 1 + rnd() % 700;
            blk[i] = kmalloc(sz); CHECK(blk[i] != 0);
            len[i] = sz; tagv[i] = (uint8_t)rnd(); memset(blk[i], tagv[i], sz);
        }
    }
    for (int i = 0; i < LIVE; i++) kfree(blk[i]);
    heap_get_stats(&s);
    CHECK(s.used_blocks == 0 && s.used_bytes == 0 && s.large_pages == 0);
    CHECK(given() <= base_pages + s.slab_pages);           // всё вернулось менеджеру страниц (кроме запаса плит), таблица сжалась                 // всё вернулось менеджеру страниц (кроме запаса и таблицы)

    // нехватка памяти: отказ без порчи, потом всё снова работает
    limit = given() + 40;
    int got = 0; void* held[200];
    for (int i = 0; i < 200; i++) { held[i] = kmalloc(4096 * 3); if (held[i]) got++; }
    CHECK(got > 0 && got < 200);
    CHECK(kmalloc(1 << 20) == 0);
    for (int i = 0; i < 200; i++) kfree(held[i]);
    limit = POOL;
    void* after = kmalloc(1 << 20); CHECK(after != 0); kfree(after);
    heap_get_stats(&s);
    CHECK(s.used_blocks == 0 && s.bad_frees == bad0 + 4);

    free(v); free(blk); free(len); free(tagv); free(pool);
    printf("%s\n", fails ? "FAILED" : "heap: classes, alignment, large blocks, ownership table growth, bad frees, 200k random ops, OOM recovery passed");
    return fails != 0;
}
