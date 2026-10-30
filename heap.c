#include "heap.h"
#include "pmm.h"
#include "spinlock.h"
#include "uart.h"

// Страницы PMM отображены 1:1 (первый ГБ - boot.s, остальное - ramext.c): физический адрес = указатель.

#define PAGE        4096UL
#define SLAB_MAGIC  0x424C534BU          // "KSLB"
#define SLAB_HDR    64                   // объекты начинаются с этого смещения
#define NCLASS      8

static const uint16_t class_size[NCLASS] = { 16, 32, 64, 128, 256, 512, 1024, HEAP_SLAB_MAX };

typedef struct slab {
    uint32_t magic;
    uint16_t cls;
    uint16_t used;
    struct slab* next;
    struct slab* prev;
    uint64_t map[4];                     // 1 - объект занят (не больше 252 объектов на страницу)
} slab_t;
_Static_assert(sizeof(slab_t) <= SLAB_HDR, "slab header must fit before the first object");

static inline unsigned slab_capacity(unsigned cls) { return (unsigned)((PAGE - SLAB_HDR) / class_size[cls]); }

// ---- Таблица владения: страница -> (плита | крупный блок из N страниц). Открытая адресация. ----
enum { OWN_EMPTY = 0, OWN_TOMB = 1, OWN_SLAB = 2, OWN_LARGE = 3 };
typedef struct { uint64_t page; uint32_t pages; uint32_t kind; } owner_t;

static owner_t* tab;
static uint32_t tab_cap;                 // степень двойки
static uint32_t tab_used;                // занятые и «надгробия»
static uint32_t tab_pages;               // страниц под саму таблицу
static uint32_t tab_live;

static slab_t* partial[NCLASS];          // плиты со свободными местами
static unsigned empty_kept[NCLASS];      // пустых плит, оставленных про запас (не больше одной на класс)
static heap_stats_t st;
static int ready;
static spinlock_t heap_lock = SPINLOCK_INIT;

static inline uint32_t slot_of(uint64_t page, uint32_t cap) {
    return (uint32_t)(((page >> 12) * 0x9E3779B97F4A7C15ULL) >> 32) & (cap - 1);
}

static owner_t* find(uint64_t page) {
    if (!tab) return 0;
    for (uint32_t i = slot_of(page, tab_cap), n = 0; n < tab_cap; i = (i + 1) & (tab_cap - 1), n++) {
        if (tab[i].kind == OWN_EMPTY) return 0;
        if (tab[i].kind != OWN_TOMB && tab[i].page == page) return &tab[i];
    }
    return 0;
}

static uint64_t pages_alloc(uint32_t n) { return n == 1 ? pmm_alloc_page() : pmm_alloc_contiguous(n); }
static void pages_free(uint64_t pa, uint32_t n) { if (n == 1) pmm_free_page(pa); else pmm_free_contiguous(pa, n); }

static int tab_resize(uint32_t cap) {
    uint32_t pages = (uint32_t)((cap * sizeof(owner_t) + PAGE - 1) / PAGE);
    uint64_t pa = pages_alloc(pages);
    if (!pa) return -1;
    owner_t* nt = (owner_t*)(uintptr_t)pa;
    for (uint32_t i = 0; i < cap; i++) nt[i].kind = OWN_EMPTY;
    for (uint32_t i = 0; tab && i < tab_cap; i++) {
        if (tab[i].kind < OWN_SLAB) continue;
        uint32_t j = slot_of(tab[i].page, cap);
        while (nt[j].kind != OWN_EMPTY) j = (j + 1) & (cap - 1);
        nt[j] = tab[i];
    }
    if (tab) pages_free((uint64_t)(uintptr_t)tab, tab_pages);
    tab = nt; tab_cap = cap; tab_pages = pages; tab_used = tab_live;
    return 0;
}

static int own_add(uint64_t page, uint32_t pages, uint32_t kind) {
    if ((tab_used + 1) * 10 > tab_cap * 7) {                       // заполнение не выше 70%
        uint32_t cap = tab_cap;
        while ((tab_live + 1) * 10 > cap * 4) cap *= 2;             // после перестройки - не выше 40%
        if (tab_resize(cap) != 0) return -1;
    }
    uint32_t i = slot_of(page, tab_cap);
    while (tab[i].kind >= OWN_SLAB) i = (i + 1) & (tab_cap - 1);
    if (tab[i].kind == OWN_EMPTY) tab_used++;
    tab[i].page = page; tab[i].pages = pages; tab[i].kind = kind;
    tab_live++;
    return 0;
}

static void own_del(owner_t* o) {
    o->kind = OWN_TOMB;
    tab_live--;
    if (tab_cap > 512 && tab_live * 10 < tab_cap) {                // после пика нагрузки таблица сжимается
        uint32_t cap = 512;
        while (tab_live * 10 > cap * 4) cap *= 2;
        tab_resize(cap);                                            // не вышло (нет памяти) - остаётся прежней
    }
}

void heap_init(void) {
    uint64_t f = spin_lock_irqsave(&heap_lock);
    if (!ready) {
        tab_live = 0;
        if (tab_resize(512) == 0) ready = 1;
    }
    spin_unlock_irqrestore(&heap_lock, f);
    uart_print(ready ? "[KIRA HEAP] Kernel heap online: size classes 16..2016 B on pages, large blocks as page runs, grows with RAM.\n"
                     : "[KIRA HEAP ERROR] No memory for the heap ownership table.\n");
}

// ---- мелкие блоки ----
static void list_push(slab_t** head, slab_t* s) {
    s->prev = 0; s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}
static void list_unlink(slab_t** head, slab_t* s) {
    if (s->prev) s->prev->next = s->next; else *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = 0;
}

static void* slab_alloc(unsigned cls) {
    slab_t* s = partial[cls];
    if (!s) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) return 0;
        if (own_add(pa, 1, OWN_SLAB) != 0) { pmm_free_page(pa); return 0; }
        s = (slab_t*)(uintptr_t)pa;
        s->magic = SLAB_MAGIC; s->cls = (uint16_t)cls; s->used = 0;
        s->map[0] = s->map[1] = s->map[2] = s->map[3] = 0;
        list_push(&partial[cls], s);
        st.slab_pages++;
        st.slab_free_bytes += (size_t)slab_capacity(cls) * class_size[cls];
    } else if (s->used == 0) {
        empty_kept[cls]--;                                          // запасная пустая плита пошла в дело
    }
    unsigned cap = slab_capacity(cls);
    for (unsigned w = 0; w < 4; w++) {
        uint64_t free_bits = ~s->map[w];
        if (w * 64 + 64 > cap) free_bits &= (cap > w * 64) ? ((1ULL << (cap - w * 64)) - 1) : 0;
        if (!free_bits) continue;
        unsigned b = (unsigned)__builtin_ctzll(free_bits);
        s->map[w] |= 1ULL << b;
        if (++s->used == cap) list_unlink(&partial[cls], s);         // полная плита уходит из списка
        st.slab_free_bytes -= class_size[cls];
        return (uint8_t*)s + SLAB_HDR + (size_t)(w * 64 + b) * class_size[cls];
    }
    return 0;                                                        // недостижимо: плита в списке не полна
}

static int slab_free(slab_t* s, uintptr_t addr) {
    if (s->magic != SLAB_MAGIC || s->cls >= NCLASS) return -1;
    unsigned cls = s->cls, size = class_size[cls];
    uintptr_t off = addr - (uintptr_t)s;
    if (off < SLAB_HDR || (off - SLAB_HDR) % size) return -1;
    unsigned idx = (unsigned)((off - SLAB_HDR) / size);
    if (idx >= slab_capacity(cls) || !((s->map[idx / 64] >> (idx % 64)) & 1)) return -1;   // не выдан или уже свободен
    s->map[idx / 64] &= ~(1ULL << (idx % 64));
    if (s->used == slab_capacity(cls)) list_push(&partial[cls], s);   // была полной: снова в списке
    s->used--;
    st.slab_free_bytes += size;
    st.used_bytes -= size;
    if (s->used == 0) {
        if (empty_kept[cls] >= 1) {                                  // одна пустая плита в запасе, остальные - назад в PMM
            list_unlink(&partial[cls], s);
            owner_t* o = find((uint64_t)(uintptr_t)s);
            if (o) own_del(o);
            s->magic = 0;
            st.slab_pages--;
            st.slab_free_bytes -= (size_t)slab_capacity(cls) * size;
            pmm_free_page((uint64_t)(uintptr_t)s);
        } else {
            empty_kept[cls]++;
        }
    }
    return 0;
}

static void* alloc_locked(size_t size) {
    if (!ready) return 0;
    if (size == 0) size = 1;
    for (unsigned c = 0; c < NCLASS; c++) {
        if (size <= class_size[c]) {
            void* p = slab_alloc(c);
            if (p) { st.used_blocks++; st.used_bytes += class_size[c]; }
            return p;
        }
    }
    if (size > (size_t)0xFFFFFFFFULL * PAGE) return 0;
    uint32_t pages = (uint32_t)((size + PAGE - 1) / PAGE);
    uint64_t pa = pages_alloc(pages);
    if (!pa) return 0;
    if (own_add(pa, pages, OWN_LARGE) != 0) { pages_free(pa, pages); return 0; }
    st.used_blocks++; st.used_bytes += (size_t)pages * PAGE; st.large_pages += pages;
    return (void*)(uintptr_t)pa;
}

void* kmalloc(size_t size) {
    if (!ready) heap_init();
    uint64_t f = spin_lock_irqsave(&heap_lock);
    void* p = alloc_locked(size);
    spin_unlock_irqrestore(&heap_lock, f);
    if (!p) uart_print("[KIRA HEAP ERROR] Out of memory for a kernel allocation.\n");
    return p;
}

static size_t usable_locked(const void* ptr, owner_t** own) {
    uintptr_t a = (uintptr_t)ptr;
    owner_t* o = find(a & ~(uintptr_t)(PAGE - 1));
    if (own) *own = o;
    if (!o) return 0;
    if (o->kind == OWN_LARGE) return a == o->page ? (size_t)o->pages * PAGE : 0;
    const slab_t* s = (const slab_t*)(uintptr_t)o->page;
    return s->cls < NCLASS ? class_size[s->cls] : 0;
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint64_t f = spin_lock_irqsave(&heap_lock);
    owner_t* o = 0;
    int bad = usable_locked(ptr, &o) == 0;
    if (!bad && o->kind == OWN_LARGE) {
        uint32_t n = o->pages;
        own_del(o);
        pages_free((uint64_t)(uintptr_t)ptr, n);
        st.used_blocks--; st.used_bytes -= (size_t)n * PAGE; st.large_pages -= n;
    } else if (!bad) {
        bad = slab_free((slab_t*)(uintptr_t)o->page, (uintptr_t)ptr) != 0;
        if (!bad) st.used_blocks--;
    }
    if (bad) st.bad_frees++;
    spin_unlock_irqrestore(&heap_lock, f);
    if (bad) uart_print("[KIRA HEAP ERROR] kfree: not a live heap block (foreign pointer or double free), ignored.\n");
}

size_t kmalloc_usable(const void* ptr) {
    if (!ptr) return 0;
    uint64_t f = spin_lock_irqsave(&heap_lock);
    size_t n = usable_locked(ptr, 0);
    spin_unlock_irqrestore(&heap_lock, f);
    return n;
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return 0; }
    size_t have = kmalloc_usable(ptr);
    if (!have) return 0;
    if (have >= size) return ptr;
    void* n = kmalloc(size);
    if (!n) return 0;
    __builtin_memcpy(n, ptr, have);
    kfree(ptr);
    return n;
}

unsigned heap_trim(void) {
    unsigned n = 0;
    uint64_t f = spin_lock_irqsave(&heap_lock);
    for (unsigned c = 0; c < NCLASS; c++) {
        for (slab_t* s = partial[c]; s; ) {
            slab_t* next = s->next;
            if (s->used == 0) {
                list_unlink(&partial[c], s);
                owner_t* o = find((uint64_t)(uintptr_t)s);
                if (o) own_del(o);
                s->magic = 0;
                st.slab_pages--;
                st.slab_free_bytes -= (size_t)slab_capacity(c) * class_size[c];
                if (empty_kept[c]) empty_kept[c]--;
                pmm_free_page((uint64_t)(uintptr_t)s);
                n++;
            }
            s = next;
        }
    }
    spin_unlock_irqrestore(&heap_lock, f);
    return n;
}

void heap_get_stats(heap_stats_t* out) {
    uint64_t f = spin_lock_irqsave(&heap_lock);
    *out = st;
    spin_unlock_irqrestore(&heap_lock, f);
}
