#include "pmm.h"
#include "uart.h"
#include "cpu.h"
#include "spinlock.h"

// Менеджер физических страниц на двух битовых картах (занята / никогда не освобождается).
// Карты лежат в самой ОЗУ (их размер зависит от объёма памяти: 1/16 байта на страницу на обе карты вместе),
// в первом гигабайте, который boot.s отображает 1:1. Память выше 1 ГБ управляется той же картой,
// но выдаётся только после того, как ramext.c её отобразил (pmm_add_high).
//
// Предел PMM_LIMIT = 512 ГБ: identity-карта ядра живёт в PML4[0], а пользовательские адреса начинаются с 512 ГБ.
#define PMM_LOW        (1ULL << 30)
#define PMM_LIMIT      (1ULL << 39)

#define MB2_MAGIC        0x36D76289UL
#define MB2_TAG_END      0
#define MB2_TAG_MMAP     6
#define MB2_MEM_AVAILABLE 1
#define MB2_MEM_ACPI      3
#define MB2_MEM_NVS       4
#define PMM_MAX_ACPI      64

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

extern char kernel_start[];
extern char kernel_end[];

#ifdef KIRA_HOST_TEST
void* pmm_test_store;                    // в хост-тесте карты лежат в буфере теста, а не по физическому адресу
uint64_t pmm_test_store_bytes;
#endif

static uint32_t* used_map;               // бит 1 = страница занята
static uint32_t* reserved_map;           // бит 1 = страница не выдаётся и не принимается обратно
static uint64_t nwords;                  // слов в каждой карте
static uint64_t managed_top;             // верхняя граница управляемых адресов
static uint32_t total_pages;             // страницы, доступные по карте памяти (выданные и свободные)
static uint32_t free_pages;
static uint32_t search_hint;             // слово, с которого искать (next-fit)
static pmm_range_t high[PMM_MAX_HIGH];
static unsigned high_n;
static pmm_range_t acpi_r[PMM_MAX_ACPI];
static unsigned acpi_n;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline int test_bit(const uint32_t* map, uint64_t page) {
    return (map[page >> 5] >> (page & 31)) & 1;
}
static inline void set_bit(uint32_t* map, uint64_t page) {
    map[page >> 5] |= (1u << (page & 31));
}
static inline void clear_bit(uint32_t* map, uint64_t page) {
    map[page >> 5] &= ~(1u << (page & 31));
}

// Свободными становятся только целиком покрытые страницы (внутрь); [base, base+len) уже обрезан по PMM_LOW.
static void mark_range_free(uint64_t base, uint64_t len) {
    uint64_t start = (base + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    uint64_t end = base + len;
    if (end < base) return; // переполнение
    end &= ~(PMM_PAGE_SIZE - 1);
    if (end > managed_top) end = managed_top;
    for (uint64_t a = start; a < end; a += PMM_PAGE_SIZE) {
        uint64_t p = a / PMM_PAGE_SIZE;
        clear_bit(reserved_map, p);
        if (test_bit(used_map, p)) {
            clear_bit(used_map, p);
            total_pages++;
        }
    }
}

// Зарезервированный диапазон округляется наружу.
static void reserve_range(uint64_t base, uint64_t len) {
    uint64_t start = base & ~(PMM_PAGE_SIZE - 1);
    uint64_t end = (base + len + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    if (end > managed_top) end = managed_top;
    for (uint64_t a = start; a < end; a += PMM_PAGE_SIZE) {
        uint64_t p = a / PMM_PAGE_SIZE;
        if (!test_bit(used_map, p) ) {
            total_pages--;
            set_bit(used_map, p);
        }
        set_bit(reserved_map, p);
    }
}

// Запоминает кусок ОЗУ выше 1 ГБ до отображения; в конце pmm_init список сортируется и сливается.
static void defer_range(uint64_t base, uint64_t end) {
    uint64_t s = (base + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    uint64_t e = end & ~(PMM_PAGE_SIZE - 1);
    if (e > managed_top) e = managed_top;
    if (e <= s) return;
    if (high_n == PMM_MAX_HIGH) {
        uart_print("[KIRA PMM] Too many separate RAM ranges above 1 GB: one is ignored.\n");
        return;
    }
    high[high_n].start = s; high[high_n].end = e; high_n++;
}

static void merge_high(void) {
    for (unsigned i = 1; i < high_n; i++) {                       // вставками: записей мало
        pmm_range_t k = high[i]; unsigned j = i;
        while (j > 0 && high[j - 1].start > k.start) { high[j] = high[j - 1]; j--; }
        high[j] = k;
    }
    unsigned w = 0;
    for (unsigned i = 0; i < high_n; i++) {
        if (w > 0 && high[i].start <= high[w - 1].end) {
            if (high[i].end > high[w - 1].end) high[w - 1].end = high[i].end;
        } else {
            high[w++] = high[i];
        }
    }
    high_n = w;
}

int pmm_init(unsigned long magic, unsigned long addr) {
    uart_print("[KIRA PMM] Parsing Multiboot2 memory map...\n");

    if (magic != MB2_MAGIC) {
        uart_print("[KIRA PMM ERROR] Bad Multiboot2 magic: ");
        uart_print_hex(magic);
        uart_print("\n");
        return -1;
    }
    if (addr == 0 || (addr & 7) != 0 || addr >= PMM_LOW) {
        uart_print("[KIRA PMM ERROR] Multiboot2 info is outside the mapped region.\n");
        return -2;
    }

    uint64_t info_size = *(const uint32_t*)addr;
    if (info_size < 16 || addr + info_size > PMM_LOW) {
        uart_print("[KIRA PMM ERROR] Corrupt Multiboot2 info size.\n");
        return -3;
    }

    const uint8_t* end = (const uint8_t*)(addr + info_size);

    // Проход 1: проверка карты и верхняя граница доступной ОЗУ (для размера карт).
    int mmap_found = 0;
    uint64_t ram_top = 0;
    for (const uint8_t* p = (const uint8_t*)(addr + 8); p + sizeof(struct mb2_tag) <= end; ) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->size < sizeof(struct mb2_tag) || p + tag->size > end) {
            uart_print("[KIRA PMM ERROR] Corrupt Multiboot2 tag.\n");
            return -3;
        }
        if (tag->type == MB2_TAG_MMAP && tag->size >= 16) {
            uint32_t entry_size = *(const uint32_t*)(p + 8);
            if (entry_size < sizeof(struct mb2_mmap_entry)) {
                uart_print("[KIRA PMM ERROR] Bad memory map entry size.\n");
                return -3;
            }
            mmap_found = 1;
            for (const uint8_t* e = p + 16; e + sizeof(struct mb2_mmap_entry) <= p + tag->size; e += entry_size) {
                const struct mb2_mmap_entry* ent = (const struct mb2_mmap_entry*)e;
                if (ent->type != MB2_MEM_AVAILABLE || ent->addr + ent->len < ent->addr) continue;
                uint64_t t = ent->addr + ent->len;
                if (t > PMM_LIMIT) t = PMM_LIMIT;
                if (t > ram_top) ram_top = t;
            }
        }
        p += (tag->size + 7) & ~7u;
    }
    if (!mmap_found || ram_top == 0) {
        uart_print("[KIRA PMM ERROR] Bootloader did not provide a memory map.\n");
        return -4;
    }

    const uint64_t group = 32 * PMM_PAGE_SIZE;                    // слово карты = 32 страницы
    managed_top = (ram_top + group - 1) & ~(group - 1);
    nwords = managed_top / PMM_PAGE_SIZE / 32;
    uint64_t store_bytes = (2 * nwords * 4 + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);

    // Где лежат карты: в ОЗУ первого гигабайта, после образа ядра и Multiboot2-структуры.
    uint64_t store_base = 0;
#ifdef KIRA_HOST_TEST
    if (!pmm_test_store || pmm_test_store_bytes < store_bytes) {
        uart_print("[KIRA PMM ERROR] No room for the page bitmaps.\n");
        return -5;
    }
    uint32_t* store = (uint32_t*)pmm_test_store;
#else
    {
        uint64_t floor = (uint64_t)(uintptr_t)kernel_end;
        if (addr + info_size > floor) floor = addr + info_size;
        if (floor < 0x100000) floor = 0x100000;
        floor = (floor + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
        for (const uint8_t* p = (const uint8_t*)(addr + 8); p + sizeof(struct mb2_tag) <= end && !store_base; ) {
            const struct mb2_tag* tag = (const struct mb2_tag*)p;
            if (tag->type == MB2_TAG_END) break;
            if (tag->type == MB2_TAG_MMAP && tag->size >= 16) {
                uint32_t entry_size = *(const uint32_t*)(p + 8);
                for (const uint8_t* e = p + 16; e + sizeof(struct mb2_mmap_entry) <= p + tag->size; e += entry_size) {
                    const struct mb2_mmap_entry* ent = (const struct mb2_mmap_entry*)e;
                    if (ent->type != MB2_MEM_AVAILABLE || ent->addr + ent->len < ent->addr) continue;
                    uint64_t s = ent->addr > floor ? ent->addr : floor;
                    s = (s + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
                    uint64_t t = ent->addr + ent->len;
                    if (t > PMM_LOW) t = PMM_LOW;
                    if (t > s && t - s >= store_bytes) { store_base = s; break; }
                }
            }
            p += (tag->size + 7) & ~7u;
        }
        if (!store_base) {
            uart_print("[KIRA PMM ERROR] No room for the page bitmaps below 1 GB.\n");
            return -5;
        }
    }
    uint32_t* store = (uint32_t*)(uintptr_t)store_base;
#endif
    used_map = store;
    reserved_map = store + nwords;

    // Старт: всё занято и зарезервировано; освобождаем то, что карта памяти называет доступным.
    for (uint64_t i = 0; i < nwords; i++) { used_map[i] = 0xFFFFFFFFu; reserved_map[i] = 0xFFFFFFFFu; }
    total_pages = 0;
    high_n = 0; acpi_n = 0;

    uint64_t usable_bytes = 0;
    for (const uint8_t* p = (const uint8_t*)(addr + 8); p + sizeof(struct mb2_tag) <= end; ) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_MMAP && tag->size >= 16) {
            uint32_t entry_size = *(const uint32_t*)(p + 8);
            for (const uint8_t* e = p + 16; e + sizeof(struct mb2_mmap_entry) <= p + tag->size; e += entry_size) {
                const struct mb2_mmap_entry* ent = (const struct mb2_mmap_entry*)e;
                if ((ent->type == MB2_MEM_ACPI || ent->type == MB2_MEM_NVS) && ent->addr + ent->len > ent->addr && acpi_n < PMM_MAX_ACPI) {
                    acpi_r[acpi_n].start = ent->addr; acpi_r[acpi_n].end = ent->addr + ent->len; acpi_n++;
                }
                if (ent->type != MB2_MEM_AVAILABLE || ent->addr + ent->len < ent->addr) continue;
                usable_bytes += ent->len;
                uint64_t a = ent->addr, b = ent->addr + ent->len;
                if (a < PMM_LOW) mark_range_free(a, (b < PMM_LOW ? b : PMM_LOW) - a);
                if (b > PMM_LOW) defer_range(a > PMM_LOW ? a : PMM_LOW, b);
            }
        }
        p += (tag->size + 7) & ~7u;
    }
    merge_high();

    // Резервируем то, что занято НЕ по карте памяти:
    reserve_range(0, 0x100000);                                  // всё ниже 1 МБ (BIOS, IVT, VGA, EBDA)
    reserve_range((uint64_t)(uintptr_t)kernel_start,
                  (uint64_t)(uintptr_t)kernel_end - (uint64_t)(uintptr_t)kernel_start); // образ ядра, .bss, стек, таблицы страниц, куча
    reserve_range(addr, info_size);                              // сама Multiboot2-структура
    if (store_base) reserve_range(store_base, store_bytes);      // карты страниц

    free_pages = 0;
    for (uint64_t pg = 0; pg < nwords * 32; pg++) {
        if (!test_bit(used_map, pg)) free_pages++;
    }
    search_hint = 0;

    uint64_t high_bytes = 0;
    for (unsigned i = 0; i < high_n; i++) high_bytes += high[i].end - high[i].start;
    uart_print("[KIRA PMM] Usable RAM per memory map: ");
    uart_print_dec(usable_bytes >> 20);
    uart_print(" MB; allocatable now (below 1 GB, minus kernel and bitmaps): ");
    uart_print_dec(pmm_get_free_memory_kb() >> 10);
    uart_print(" MB; above 1 GB waiting to be mapped: ");
    uart_print_dec(high_bytes >> 20);
    uart_print(" MB; bitmaps ");
    uart_print_dec(store_bytes >> 10);
    uart_print(" KB.\n");
    return 0;
}

unsigned pmm_high_ranges(pmm_range_t* out, unsigned max) {
    unsigned n = high_n < max ? high_n : max;
    for (unsigned i = 0; i < n; i++) out[i] = high[i];
    return n;
}

unsigned pmm_acpi_ranges(pmm_range_t* out, unsigned max) {
    unsigned n = acpi_n < max ? acpi_n : max;
    for (unsigned i = 0; i < n; i++) out[i] = acpi_r[i];
    return n;
}

uint64_t pmm_managed_top(void) { return managed_top; }

int64_t pmm_add_high(uint64_t start, uint64_t end) {
    if ((start | end) & (PMM_PAGE_SIZE - 1) || end <= start || start < PMM_LOW) return -1;
    int inside = 0;
    for (unsigned i = 0; i < high_n; i++) if (start >= high[i].start && end <= high[i].end) inside = 1;
    if (!inside) return -1;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    int64_t added = 0;
    for (uint64_t a = start; a < end; a += PMM_PAGE_SIZE) {
        uint64_t p = a / PMM_PAGE_SIZE;
        if (!test_bit(used_map, p)) continue;                    // уже выдаётся
        clear_bit(used_map, p);
        clear_bit(reserved_map, p);
        total_pages++; free_pages++; added++;
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    return added;
}

uint64_t pmm_alloc_page(void) {
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    uint64_t result = 0;
    for (uint64_t n = 0; n < nwords; n++) {
        uint64_t w = (search_hint + n) % nwords;
        if (used_map[w] != 0xFFFFFFFFu) {
            uint32_t bit = (uint32_t)__builtin_ctz(~used_map[w]);
            used_map[w] |= (1u << bit);
            search_hint = (uint32_t)w;
            free_pages--;
            result = (w * 32 + bit) * PMM_PAGE_SIZE;
            break;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    if (!result) uart_print("[KIRA PMM ERROR] Out of memory!\n");
    return result;
}

// Непрерывный блок страниц (буфер кадра, z-буфер, DMA). Страницы идут подряд в физической
// памяти, а т.к. вся управляемая память отображена 1:1, блок непрерывен и в виртуальной.
uint64_t pmm_alloc_contiguous(uint32_t pages) {
    if (pages == 0) return 0;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    uint64_t result = 0, run = 0, start = 0;
    uint64_t total = nwords * 32;
    for (uint64_t p = 0; p < total; ) {
        if ((p & 31) == 0 && used_map[p >> 5] == 0xFFFFFFFFu) { run = 0; p += 32; continue; }
        if (test_bit(used_map, p)) { run = 0; p++; continue; }
        if (run == 0) start = p;
        run++; p++;
        if (run == pages) {
            for (uint64_t q = start; q < start + pages; q++) set_bit(used_map, q);
            free_pages -= pages;
            result = start * PMM_PAGE_SIZE;
            break;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    if (!result) uart_print("[KIRA PMM ERROR] No contiguous block of the requested size.\n");
    return result;
}

int pmm_free_contiguous(uint64_t paddr, uint32_t pages) {
    if (!pages || (paddr & (PMM_PAGE_SIZE - 1)) || paddr >= managed_top) return -1;
    if ((uint64_t)pages > (managed_top - paddr) / PMM_PAGE_SIZE) return -1;

    uint64_t first = paddr / PMM_PAGE_SIZE;
    uint64_t end = first + (uint64_t)pages;
    if (end > nwords * 32) return -1;

    /* A contiguous allocation is one ownership unit: validate every page before freeing any. */
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    for (uint64_t page = first; page < end; page++) {
        if (test_bit(reserved_map, page) || !test_bit(used_map, page)) {
            spin_unlock_irqrestore(&pmm_lock, f);
            return -1;
        }
    }
    for (uint64_t page = first; page < end; page++) clear_bit(used_map, page);
    free_pages += pages;
    uint32_t first_word = (uint32_t)(first >> 5);
    if (first_word < search_hint) search_hint = first_word;
    spin_unlock_irqrestore(&pmm_lock, f);
    return 0;
}

int pmm_free_page(uint64_t paddr) {
    if ((paddr & (PMM_PAGE_SIZE - 1)) != 0 || paddr >= managed_top) {
        return -1;
    }
    uint64_t page = paddr / PMM_PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    int rc = 0;
    if (test_bit(reserved_map, page) || !test_bit(used_map, page)) {
        rc = -1;
    } else {
        clear_bit(used_map, page);
        free_pages++;
        if ((uint32_t)(page >> 5) < search_hint) search_hint = (uint32_t)(page >> 5);
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    return rc;
}

uint32_t pmm_get_free_memory_kb(void) {
    return free_pages * (uint32_t)(PMM_PAGE_SIZE / 1024);
}

uint32_t pmm_get_total_memory_kb(void) { // всего управляемых страниц (занятых + свободных)
    return total_pages * (uint32_t)(PMM_PAGE_SIZE / 1024);
}
