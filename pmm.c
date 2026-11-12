#include "pmm.h"
#include "bootinfo.h"
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
#define PMM_DMA_RESERVE_WORDS ((uint32_t)(((16ULL << 20) / PMM_PAGE_SIZE) / 32))

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
static pmm_range_t active_high[PMM_MAX_HIGH]; /* high pages mapped by ramext and admitted by pmm_add_high */
static unsigned active_high_n;
static pmm_range_t acpi_r[PMM_MAX_ACPI];
static unsigned acpi_n;
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* Validate the complete Multiboot2 block before any memory-map pass. */
static int mb2_validate(uint64_t addr, uint64_t total) {
    if (total < 16 || (total & 7) || addr >= PMM_LOW || total > PMM_LOW - addr)
        return 0;
    const uint8_t* p = (const uint8_t*)(uintptr_t)(addr + 8);
    uint64_t remaining = total - 8;
    while (remaining >= sizeof(struct mb2_tag)) {
        const struct mb2_tag* tag = (const struct mb2_tag*)p;
        if (tag->size < sizeof(struct mb2_tag) || tag->size > remaining) return 0;
        if (tag->type == MB2_TAG_END)
            return tag->size == 8 && remaining == 8;
        uint64_t padded = ((uint64_t)tag->size + 7u) & ~7ULL;
        if (padded > remaining) return 0;
        if (tag->type == MB2_TAG_MMAP) {
            if (tag->size < 16) return 0;
            uint32_t entry_size = *(const uint32_t*)(p + 8);
            uint32_t payload = tag->size - 16;
            if (entry_size < sizeof(struct mb2_mmap_entry) || payload % entry_size)
                return 0;
        }
        p += padded;
        remaining -= padded;
    }
    return 0;
}

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
    /* Mark only firmware-usable high pages as eligible for later mapping.
       Reserved modules/framebuffer are re-marked below during BootInfo setup. */
    for(uint64_t a=s;a<e;a+=PMM_PAGE_SIZE) clear_bit(reserved_map,a/PMM_PAGE_SIZE);
    if (high_n == PMM_MAX_HIGH) {
        uart_print("[KIRA PMM] Too many separate RAM ranges above 1 GB: one is ignored.\n");
        return;
    }
    high[high_n].start = s; high[high_n].end = e; high_n++;
}

static int high_page_active_locked(uint64_t paddr) {
    if (paddr < PMM_LOW) return 1;
    for (unsigned i = 0; i < active_high_n; i++)
        if (paddr >= active_high[i].start && paddr < active_high[i].end) return 1;
    return 0;
}

/* Add a mapped high-memory interval, keeping the active set sorted and coalesced. Caller holds pmm_lock. */
static int active_high_add_locked(uint64_t start, uint64_t end) {
    pmm_range_t tmp[PMM_MAX_HIGH + 1];
    unsigned n = active_high_n;
    for (unsigned i = 0; i < n; i++) tmp[i] = active_high[i];
    if (n >= PMM_MAX_HIGH) {
        int merged = 0;
        for (unsigned i = 0; i < n; i++) {
            if (end < tmp[i].start || start > tmp[i].end) continue;
            if (start < tmp[i].start) tmp[i].start = start;
            if (end > tmp[i].end) tmp[i].end = end;
            merged = 1;
            break;
        }
        if (!merged) return -1;
    } else {
        tmp[n++] = (pmm_range_t){ .start=start, .end=end };
    }
    for (unsigned i = 1; i < n; i++) {
        pmm_range_t v = tmp[i]; unsigned j = i;
        while (j && tmp[j - 1].start > v.start) { tmp[j] = tmp[j - 1]; j--; }
        tmp[j] = v;
    }
    unsigned w = 0;
    for (unsigned i = 0; i < n; i++) {
        if (w && tmp[i].start <= tmp[w - 1].end) {
            if (tmp[i].end > tmp[w - 1].end) tmp[w - 1].end = tmp[i].end;
        } else tmp[w++] = tmp[i];
    }
    if (w > PMM_MAX_HIGH) return -1;
    for (unsigned i = 0; i < w; i++) active_high[i] = tmp[i];
    active_high_n = w;
    return 0;
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

static int pmm_init_map(const kbi_mmap_entry_t* map, uint32_t map_count,
                         uint64_t reserve_addr, uint64_t reserve_size,
                         const kbi_header_t* handoff) {
    if(!map || !map_count || map_count>KIRA_BOOTINFO_MAX_MMAP) return -4;
    uint64_t ram_top=0;
    for(uint32_t i=0;i<map_count;i++) {
        if(map[i].type!=KBI_MEM_USABLE || !map[i].length ||
           map[i].base>UINT64_MAX-map[i].length) continue;
        uint64_t t=map[i].base+map[i].length;
        if(t>PMM_LIMIT) t=PMM_LIMIT;
        if(t>ram_top) ram_top=t;
    }
    if(!ram_top) {
        uart_print("[KIRA PMM ERROR] BootInfo has no usable memory ranges.\n");
        return -4;
    }

    const uint64_t group=32*PMM_PAGE_SIZE;
    managed_top=(ram_top+group-1)&~(group-1);
    nwords=managed_top/PMM_PAGE_SIZE/32;
    uint64_t store_bytes=(2*nwords*4+PMM_PAGE_SIZE-1)&~(PMM_PAGE_SIZE-1);
    uint64_t store_base=0;
#ifdef KIRA_HOST_TEST
    if(!pmm_test_store || pmm_test_store_bytes<store_bytes) {
        uart_print("[KIRA PMM ERROR] No room for the page bitmaps.\n");
        return -5;
    }
    uint32_t* store=(uint32_t*)pmm_test_store;
#else
    {
        uint64_t floor=(uint64_t)(uintptr_t)kernel_end;
        if(reserve_addr && reserve_size && reserve_addr<=UINT64_MAX-reserve_size &&
           reserve_addr+reserve_size>floor) floor=reserve_addr+reserve_size;
        if(floor<0x100000) floor=0x100000;
        floor=(floor+PMM_PAGE_SIZE-1)&~(PMM_PAGE_SIZE-1);
        for(uint32_t i=0;i<map_count && !store_base;i++) {
            if(map[i].type!=KBI_MEM_USABLE || map[i].base>UINT64_MAX-map[i].length) continue;
            uint64_t s=map[i].base>floor?map[i].base:floor;
            s=(s+PMM_PAGE_SIZE-1)&~(PMM_PAGE_SIZE-1);
            uint64_t t=map[i].base+map[i].length;
            if(t>PMM_LOW)t=PMM_LOW;
            if(t<=s || t-s<store_bytes) continue;
            uint64_t store_end=s+store_bytes;
            int blocked=0;
            if(handoff) {
                const kbi_module_t* mods=(const kbi_module_t*)((const uint8_t*)handoff+handoff->modules_off);
                for(uint32_t m=0;m<handoff->modules_count;m++) {
                    uint64_t me=mods[m].base+mods[m].length;
                    if(s<me && mods[m].base<store_end) { blocked=1; break; }
                }
                if(handoff->fb_size && s<handoff->fb_phys+handoff->fb_size &&
                   handoff->fb_phys<store_end) blocked=1;
            }
            if(!blocked) {store_base=s;break;}
        }
        if(!store_base) {
            uart_print("[KIRA PMM ERROR] No room for the page bitmaps below 1 GB.\n");
            return -5;
        }
    }
    uint32_t* store=(uint32_t*)(uintptr_t)store_base;
#endif
    used_map=store;
    reserved_map=store+nwords;
    for(uint64_t i=0;i<nwords;i++){used_map[i]=0xFFFFFFFFu;reserved_map[i]=0xFFFFFFFFu;}
    total_pages=0; high_n=0; active_high_n=0; acpi_n=0;
    uint64_t usable_bytes=0;
    for(uint32_t i=0;i<map_count;i++) {
        const kbi_mmap_entry_t* e=&map[i];
        if(!e->length || e->base>UINT64_MAX-e->length) continue;
        uint64_t a=e->base,b=e->base+e->length;
        if((e->type==KBI_MEM_ACPI_RECLAIM || e->type==KBI_MEM_ACPI_NVS) &&
           acpi_n<PMM_MAX_ACPI) {
            acpi_r[acpi_n].start=a; acpi_r[acpi_n].end=b; acpi_n++;
        }
        if(e->type!=KBI_MEM_USABLE) continue;
        usable_bytes+=e->length;
        if(a<PMM_LOW) mark_range_free(a,(b<PMM_LOW?b:PMM_LOW)-a);
        if(b>PMM_LOW) defer_range(a>PMM_LOW?a:PMM_LOW,b);
    }
    merge_high();
    reserve_range(0,0x100000);
    reserve_range((uint64_t)(uintptr_t)kernel_start,
                  (uint64_t)(uintptr_t)kernel_end-(uint64_t)(uintptr_t)kernel_start);
    if(reserve_addr && reserve_size) reserve_range(reserve_addr,reserve_size);
    if(store_base) reserve_range(store_base,store_bytes);

    /* Boot modules (initrd, firmware payloads) and the framebuffer may lie in
       ranges marked usable by firmware. They must never enter the allocator. */
    if(handoff) {
        const kbi_module_t* mods=(const kbi_module_t*)((const uint8_t*)handoff+handoff->modules_off);
        for(uint32_t i=0;i<handoff->modules_count;i++)
            reserve_range(mods[i].base,mods[i].length);
        if(handoff->fb_size) reserve_range(handoff->fb_phys,handoff->fb_size);
    }

    free_pages=0;
    for(uint64_t pg=0;pg<nwords*32;pg++) if(!test_bit(used_map,pg)) free_pages++;
    /* Обычные выделения начинаем выше 16 МБ, сохраняя нижнюю память для ISA DMA.
       Ограниченные устройства используют отдельные *_below() функции и могут брать её явно. */
    search_hint = PMM_DMA_RESERVE_WORDS;
    if (search_hint >= nwords) search_hint = 0;
    uint64_t high_bytes=0;
    for(unsigned i=0;i<high_n;i++) high_bytes+=high[i].end-high[i].start;
    uart_print("[KIRA PMM] Usable RAM per BootInfo map: ");
    uart_print_dec(usable_bytes>>20);
    uart_print(" MB; allocatable now (below 1 GB, minus kernel and bitmaps): ");
    uart_print_dec(pmm_get_free_memory_kb()>>10);
    uart_print(" MB; above 1 GB waiting to be mapped: ");
    uart_print_dec(high_bytes>>20);
    uart_print(" MB; bitmaps ");
    uart_print_dec(store_bytes>>10);
    uart_print(" KB.\n");
    return 0;
}

int pmm_init_bootinfo(const void* blob, uint32_t blob_size,
                      uint64_t reserve_addr, uint64_t reserve_size) {
    if(reserve_size && reserve_addr>UINT64_MAX-reserve_size) return -3;
    if(kbi_validate(blob,blob_size)!=0) {
        uart_print("[KIRA PMM ERROR] Invalid BootInfo handoff.\n");
        return -3;
    }
    const kbi_header_t* h=(const kbi_header_t*)blob;
    if(h->arch!=KBI_ARCH_X86_64) return -3;
    return pmm_init_map(kbi_mmap(blob),h->mmap_count,reserve_addr,reserve_size,h);
}

int pmm_init(unsigned long magic, unsigned long addr) {
    uart_print("[KIRA PMM] Parsing versioned BootInfo memory map...\n");
    if(magic!=MB2_MAGIC) {
        uart_print("[KIRA PMM ERROR] Bad Multiboot2 magic: "); uart_print_hex(magic); uart_print("\n");
        return -1;
    }
    if(addr==0 || (addr&7)!=0 || addr>=PMM_LOW) {
        uart_print("[KIRA PMM ERROR] Multiboot2 info is outside the mapped region.\n");
        return -2;
    }
    uint64_t info_size=*(const uint32_t*)addr;
    if(info_size<16 || (info_size&7) || info_size>PMM_LOW-addr ||
       !mb2_validate(addr,info_size)) {
        uart_print("[KIRA PMM ERROR] Corrupt Multiboot2 info block, tag sequence or memory map.\n");
        return -3;
    }
    const void* blob=0; uint32_t blob_size=0;
    int rc=kbi_from_multiboot2((const void*)(uintptr_t)addr,(uint32_t)info_size,
        (uint64_t)(uintptr_t)kernel_start,
        (uint64_t)(uintptr_t)kernel_end-(uint64_t)(uintptr_t)kernel_start,
        &blob,&blob_size);
    if(rc) {
        uart_print("[KIRA PMM ERROR] Multiboot2 could not be normalized to BootInfo: ");
        uart_print_dec((uint32_t)(-rc)); uart_print("\n");
        return rc==-34?-4:-3;
    }
    return pmm_init_bootinfo(blob,blob_size,addr,info_size);
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
    if (active_high_add_locked(start, end)) {
        spin_unlock_irqrestore(&pmm_lock, f);
        return -1;
    }
    int64_t added = 0;
    for (uint64_t a = start; a < end; a += PMM_PAGE_SIZE) {
        uint64_t p = a / PMM_PAGE_SIZE;
        if (!test_bit(used_map, p) || test_bit(reserved_map, p)) continue;
        /* A reserved boot module or device range remains unavailable even
           when its containing firmware RAM range is explicitly mapped. */
        clear_bit(used_map, p);
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
    uint64_t preferred = (uint64_t)search_hint * 32;
    if (preferred >= total) preferred = 0;
    /* Two physical ranges, not a modulo walk: a contiguous run must never wrap at RAM's end. */
    for (unsigned pass = 0; pass < 2 && !result; pass++) {
        uint64_t lo = pass ? 0 : preferred;
        uint64_t hi = pass ? preferred : total;
        run = 0;
        for (uint64_t p = lo; p < hi; ) {
            if ((p & 31) == 0 && used_map[p >> 5] == 0xFFFFFFFFu) { run = 0; p += 32; continue; }
            if (test_bit(used_map, p)) { run = 0; p++; continue; }
            if (run == 0) start = p;
            run++; p++;
            if (run == pages) {
                for (uint64_t q = start; q < start + pages; q++) set_bit(used_map, q);
                free_pages -= pages;
                result = start * PMM_PAGE_SIZE;
                search_hint = (uint32_t)((start + pages) / 32);
                if (search_hint >= nwords) search_hint = 0;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    if (!result) uart_print("[KIRA PMM ERROR] No contiguous block of the requested size.\n");
    return result;
}

/* Непрерывный блок с верхней границей адреса (exclusive): для устройств с ограниченной DMA-адресацией. */
uint64_t pmm_alloc_contiguous_below(uint32_t pages, uint64_t limit_exclusive) {
    if (!pages || limit_exclusive < PMM_PAGE_SIZE) return 0;
    uint64_t limit = limit_exclusive / PMM_PAGE_SIZE;
    uint64_t total = nwords * 32;
    if (limit > total) limit = total;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    uint64_t result = 0, run = 0, start = 0;
    for (uint64_t p = 0; p < limit; ) {
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
    return result;
}

/* Allocate only the requested run: do not require an oversized temporary block just to align its interior. */
uint64_t pmm_alloc_contiguous_aligned_below(uint32_t pages, uint32_t align_pages, uint64_t limit_exclusive) {
    if (!pages || !align_pages || (align_pages & (align_pages - 1)) || limit_exclusive < PMM_PAGE_SIZE) return 0;
    uint64_t limit = limit_exclusive / PMM_PAGE_SIZE;
    uint64_t total = nwords * 32;
    if (limit > total) limit = total;
    if (pages > limit) return 0;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    uint64_t result = 0;
    for (uint64_t start = 0; start <= limit - pages; ) {
        uint64_t rem = start & (uint64_t)(align_pages - 1);
        if (rem) start += align_pages - rem;
        if (start > limit - pages) break;
        uint32_t i = 0;
        while (i < pages && !test_bit(used_map, start + i)) i++;
        if (i == pages) {
            for (uint64_t q = start; q < start + pages; q++) set_bit(used_map, q);
            free_pages -= pages;
            result = start * PMM_PAGE_SIZE;
            break;
        }
        start += (uint64_t)i + 1;
    }
    uint32_t low_free = 0, max_run = 0, run = 0;
    if (!result) {
        for (uint64_t p = 0; p < limit; p++) {
            if (!test_bit(used_map, p)) {
                low_free++;
                if (++run > max_run) max_run = run;
            } else run = 0;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    if (!result) {
        uart_print("[KIRA PMM] DMA allocation failed: pages=");
        uart_print_dec(pages);
        uart_print(" align_pages=");
        uart_print_dec(align_pages);
        uart_print(" limit=");
        uart_print_hex(limit_exclusive);
        uart_print(" free_pages_below_limit=");
        uart_print_dec(low_free);
        uart_print(" largest_run=");
        uart_print_dec(max_run);
        uart_print(".\n");
    }
    return result;
}

int pmm_free_contiguous(uint64_t paddr, uint32_t pages) {
    if (!pages || (paddr & (PMM_PAGE_SIZE - 1)) ||
        paddr > UINT64_MAX - (uint64_t)pages * PMM_PAGE_SIZE ||
        paddr + (uint64_t)pages * PMM_PAGE_SIZE > managed_top) return -1;
    uint64_t first = paddr / PMM_PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t page = first + i;
        uint64_t addr = paddr + (uint64_t)i * PMM_PAGE_SIZE;
        if (!high_page_active_locked(addr) || test_bit(reserved_map, page) || !test_bit(used_map, page)) {
            spin_unlock_irqrestore(&pmm_lock, f);
            return -1;
        }
    }
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t page = first + i;
        clear_bit(used_map, page);
        free_pages++;
        if ((uint32_t)(page >> 5) < search_hint &&
            ((uint32_t)(page >> 5) >= PMM_DMA_RESERVE_WORDS || search_hint < PMM_DMA_RESERVE_WORDS))
            search_hint = (uint32_t)(page >> 5);
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    return 0;
}

int pmm_free_page(uint64_t paddr) {
    if ((paddr & (PMM_PAGE_SIZE - 1)) != 0 || paddr >= managed_top) return -1;
    uint64_t page = paddr / PMM_PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    int rc = 0;
    if (!high_page_active_locked(paddr) || test_bit(reserved_map, page) || !test_bit(used_map, page)) {
        rc = -1;
    } else {
        clear_bit(used_map, page);
        free_pages++;
        if ((uint32_t)(page >> 5) < search_hint &&
            ((uint32_t)(page >> 5) >= PMM_DMA_RESERVE_WORDS || search_hint < PMM_DMA_RESERVE_WORDS))
            search_hint = (uint32_t)(page >> 5);
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
