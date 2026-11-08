#include "vmm.h"
#include "uaccess.h"
#include "pmm.h"
#include "uart.h"
#include "spinlock.h"

#define PAGE_SIZE   4096UL
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_HUGE    (1ULL << 7)
#define PTE_ADDR    0x000FFFFFFFFFF000ULL

#define MSR_EFER    0xC0000080
#define EFER_LMA    (1ULL << 10)
#define EFER_NXE    (1ULL << 11)
#define CR0_PG      (1ULL << 31)
#define CR0_WP      (1ULL << 16)
#define CR4_PAE     (1ULL << 5)

// Страницы таблиц берутся из PMM; PMM выдаёт только отображённую 1:1 память (первый гигабайт из boot.s,
// остальное - ramext.c), поэтому физический адрес таблицы = указатель.

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr0(uint64_t v) { __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void invlpg(uint64_t a) { __asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory"); }

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static int is_canonical(uint64_t va) {
    uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}

// Возвращает следующий уровень таблицы или 0. create != 0 - создать при отсутствии.
static uint64_t* next_level(uint64_t* table, unsigned idx, int create, uint64_t user) {
    uint64_t e = table[idx];
    if (!(e & PTE_PRESENT)) {
        if (!create) return 0;
        uint64_t page = pmm_alloc_page();
        if (!page) return 0;
        uint64_t* t = (uint64_t*)(uintptr_t)page;
        for (unsigned i = 0; i < 512; i++) t[i] = 0;
        table[idx] = page | PTE_PRESENT | PTE_WRITE | user;
        return t;
    }
    if (e & PTE_HUGE) return 0; // нельзя спускаться внутрь 2-МБ/1-ГБ страницы
    if (create && user && !(e & PTE_USER)) table[idx] = e | PTE_USER;
    return (uint64_t*)(uintptr_t)(e & PTE_ADDR);
}

static uint64_t kernel_root;
static int wc_enabled;

// ---- Окно MMIO ядра ----
// Регистры устройств (LAPIC, IOAPIC, BAR PCI) и видеопамять отображаются не по своему физическому адресу, а в
// отдельную область верхней половины: PML4[510], 512 ГБ. Её таблица PDPT создаётся при старте и копируется в каждое
// адресное пространство, поэтому всё, что ядро отобразит позже, сразу видно при любом CR3, не попадает в
// пользовательскую половину и не зависит от того, где устройство на шине (хоть выше 512 ГБ).
// Крупные области отображаются страницами по 2 МБ (меньше промахов TLB при выводе кадра).
#define MMIO_PML4  510
#define MMIO_BASE  0xFFFFFF0000000000ULL
#define MMIO_TOP   0xFFFFFF4000000000ULL      /* вторая половина PML4[510] - стеки потоков ядра */
#define MMIO_MAX   1024
typedef struct { uint64_t pa, len, va, cache; } mmio_map_t;
static mmio_map_t mmio[MMIO_MAX];
static unsigned mmio_n;
static uint64_t mmio_next = MMIO_BASE;
static spinlock_t mmio_lock = SPINLOCK_INIT;

static int map_in(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags) {
    if ((va & (PAGE_SIZE - 1)) || (pa & (PAGE_SIZE - 1)) || !is_canonical(va)) return -1;

    uint64_t user = flags & PTE_USER;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(root & PTE_ADDR);
    uint64_t* pdpt = next_level(pml4, (va >> 39) & 0x1FF, 1, user);
    if (!pdpt) return -1;
    uint64_t* pd = next_level(pdpt, (va >> 30) & 0x1FF, 1, user);
    if (!pd) return -1;
    uint64_t* pt = next_level(pd, (va >> 21) & 0x1FF, 1, user);
    if (!pt) return -1;

    unsigned idx = (va >> 12) & 0x1FF;
    if (pt[idx] & PTE_PRESENT) return -1; // не перезаписываем существующее отображение

    pt[idx] = (pa & PTE_ADDR) | (flags & (PTE_WRITE | PTE_USER | VMM_NOCACHE | VMM_WC | VMM_NOEXEC | VMM_SHARED)) | PTE_PRESENT;
    invlpg(va);
    return 0;
}

// ---- Стеки потоков ядра ----
// Каждый стек - в своей ячейке окна ядра (общего для всех адресных пространств): 16 КБ отображено, ниже - 48 КБ
// пустоты. Переполнение стека - страничная ошибка, а не тихая порча кучи (раньше ловилась только меткой на дне
// стека и только при переключении). Освобождённые ячейки остаются отображёнными и выдаются снова: снимать
// отображение пришлось бы с рассылкой сброса TLB всем ядрам из того места, где поток уже нельзя будить.
#define KSTACK_BASE   0xFFFFFF4000000000ULL
#define KSTACK_STRIDE (64 * 1024ULL)
#define KSTACK_SLOTS  8192u
static spinlock_t ks_lock = SPINLOCK_INIT;
static uint32_t ks_used_hi;                       /* ячеек, когда-либо отображённых */
static uint16_t ks_free[KSTACK_SLOTS]; static uint32_t ks_nfree;
static uint8_t ks_allocated[KSTACK_SLOTS];
static uint32_t ks_size;

static int map_in(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags);

void* vmm_kstack_alloc(uint32_t bytes) {
    if (!kernel_root || bytes == 0 || bytes > KSTACK_STRIDE / 2 || (bytes & (PAGE_SIZE - 1))) return 0;
    uint64_t f = spin_lock_irqsave(&ks_lock);
    if (ks_size && ks_size != bytes) { spin_unlock_irqrestore(&ks_lock, f); return 0; }
    ks_size = bytes;
    uint64_t slot_top;
    uint32_t slot;
    if (ks_nfree) {
        slot = ks_free[--ks_nfree];
        slot_top = KSTACK_BASE + (uint64_t)(slot + 1) * KSTACK_STRIDE;
    } else {
        if (ks_used_hi >= KSTACK_SLOTS) { spin_unlock_irqrestore(&ks_lock, f); return 0; }
        slot = ks_used_hi;
        slot_top = KSTACK_BASE + (uint64_t)(slot + 1) * KSTACK_STRIDE;
        for (uint64_t va = slot_top - bytes; va < slot_top; va += PAGE_SIZE) {
            uint64_t pa = pmm_alloc_page();
            if (!pa || map_in(kernel_root, va, pa, PTE_WRITE | VMM_NOEXEC) != 0) {
                if (pa) pmm_free_page(pa);
                /* Откатываем только страницы, успешно отображённые этим вызовом. */
                for (uint64_t undo = slot_top - bytes; undo < va; undo += PAGE_SIZE) {
                    uint64_t* pml4 = (uint64_t*)(uintptr_t)(kernel_root & PTE_ADDR);
                    uint64_t* pdpt = next_level(pml4, (undo >> 39) & 0x1FF, 0, 0);
                    uint64_t* pd = pdpt ? next_level(pdpt, (undo >> 30) & 0x1FF, 0, 0) : 0;
                    uint64_t* pt = pd ? next_level(pd, (undo >> 21) & 0x1FF, 0, 0) : 0;
                    if (pt) {
                        unsigned idx = (unsigned)((undo >> 12) & 0x1FF);
                        uint64_t entry = pt[idx];
                        if (entry & PTE_PRESENT) {
                            pt[idx] = 0;
                            pmm_free_page(entry & PTE_ADDR);
                            invlpg(undo);
                        }
                    }
                }
                spin_unlock_irqrestore(&ks_lock, f);
                return 0;
            }
        }
        ks_used_hi++;
    }
    if (ks_allocated[slot]) { spin_unlock_irqrestore(&ks_lock, f); return 0; }
    ks_allocated[slot] = 1;
    spin_unlock_irqrestore(&ks_lock, f);
    return (void*)(uintptr_t)(slot_top - bytes);
}

void vmm_kstack_free(void* base) {
    uint64_t va = (uint64_t)(uintptr_t)base;
    if (va < KSTACK_BASE || va >= KSTACK_BASE + (uint64_t)KSTACK_SLOTS * KSTACK_STRIDE) return;
    uint64_t f = spin_lock_irqsave(&ks_lock);
    uint64_t rel = va - KSTACK_BASE;
    uint32_t slot = (uint32_t)(rel / KSTACK_STRIDE);
    uint64_t expected = KSTACK_BASE + (uint64_t)(slot + 1) * KSTACK_STRIDE - ks_size;
    if (ks_size == 0 || va != expected || slot >= ks_used_hi ||
        !ks_allocated[slot] || ks_nfree >= KSTACK_SLOTS) {
        spin_unlock_irqrestore(&ks_lock, f);
        return;
    }
    ks_allocated[slot] = 0;
    ks_free[ks_nfree++] = (uint16_t)slot;
    spin_unlock_irqrestore(&ks_lock, f);
}

int vmm_kstack_guard_hit(uint64_t addr) {
    if (addr < KSTACK_BASE || addr >= KSTACK_BASE + (uint64_t)KSTACK_SLOTS * KSTACK_STRIDE || !ks_size) return 0;
    uint64_t off = (addr - KSTACK_BASE) % KSTACK_STRIDE;
    return off < KSTACK_STRIDE - ks_size;                                   /* ниже отображённой части ячейки */
}

int vmm_is_kstack(uint64_t addr) { return addr >= KSTACK_BASE && addr < KSTACK_BASE + (uint64_t)KSTACK_SLOTS * KSTACK_STRIDE; }

static int map_huge_in(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(root & PTE_ADDR);
    uint64_t* pdpt = next_level(pml4, (va >> 39) & 0x1FF, 1, 0);
    if (!pdpt) return -1;
    uint64_t* pd = next_level(pdpt, (va >> 30) & 0x1FF, 1, 0);
    if (!pd) return -1;
    unsigned idx = (va >> 21) & 0x1FF;
    if (pd[idx] & PTE_PRESENT) return -1;
    pd[idx] = (pa & 0x000FFFFFFFE00000ULL) | (flags & (PTE_WRITE | VMM_NOCACHE | VMM_WC | VMM_NOEXEC)) | PTE_HUGE | PTE_PRESENT;
    return 0;
}

volatile void* vmm_map_mmio(uint64_t pa, uint64_t len, int cache) {
    if (len == 0 || (cache != VMM_MMIO_UC && cache != VMM_MMIO_WC)) return 0;
    /* MMIO PTEs carry only the physical-address bits in PTE_ADDR. Reject
       out-of-range spans instead of silently aliasing a truncated address.
       phys_end is exclusive and is safe for the page-rounding below. */
    const uint64_t phys_limit = PTE_ADDR + PAGE_SIZE;
    if (pa >= phys_limit || len > phys_limit - pa) return 0;
    uint64_t phys_end = pa + len;
    uint64_t f = spin_lock_irqsave(&mmio_lock);
    for (unsigned i = 0; i < mmio_n; i++) {                       // уже отображено (другая часть того же BAR, LAPIC)
        if (pa >= mmio[i].pa && phys_end <= mmio[i].pa + mmio[i].len && mmio[i].cache == (uint64_t)cache) {
            volatile void* v = (volatile void*)(uintptr_t)(mmio[i].va + (pa - mmio[i].pa));
            spin_unlock_irqrestore(&mmio_lock, f);
            return v;
        }
    }
    uint64_t first = pa & ~(PAGE_SIZE - 1), last = ((phys_end - 1) & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    uint64_t size = last - first;
    const uint64_t BIG = 2ULL << 20;
    uint64_t va = (mmio_next + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size >= BIG) {                                             // сдвиг va так, чтобы va = pa (mod 2 МБ): тогда возможны 2-МБ страницы
        uint64_t want = first & (BIG - 1);
        va = ((va + BIG - 1) & ~(BIG - 1)) + want;
    }
    uint64_t flags = PTE_WRITE | VMM_NOEXEC | (cache == VMM_MMIO_WC && wc_enabled ? VMM_WC : VMM_NOCACHE);
    volatile void* res = 0;
    if (mmio_n < MMIO_MAX && va + size <= MMIO_TOP && va + size > va) {
        uint64_t off = 0;
        int ok = 1;
        while (ok && off < size) {
            uint64_t v = va + off, p = first + off;
            if (!(v & (BIG - 1)) && !(p & (BIG - 1)) && size - off >= BIG) {
                ok = map_huge_in(kernel_root, v, p, flags) == 0;
                off += BIG;
            } else {
                ok = map_in(kernel_root, v, p, flags) == 0;
                off += PAGE_SIZE;
            }
        }
        if (ok) {
            mmio[mmio_n].pa = first; mmio[mmio_n].len = size; mmio[mmio_n].va = va; mmio[mmio_n].cache = (uint64_t)cache;
            mmio_n++;
            mmio_next = va + size;
            res = (volatile void*)(uintptr_t)(va + (pa - first));
        }
        if (!ok) mmio_next = va + size;       /* нет памяти под таблицы: частично занятые адреса больше не выдаются */
    }
    spin_unlock_irqrestore(&mmio_lock, f);
    return res;
}

int vmm_map_page(uint64_t va, uint64_t pa, uint64_t flags) {
    return map_in(read_cr3(), va, pa, flags);
}

uint64_t vmm_kernel_root(void) { return kernel_root; }
void vmm_switch(uint64_t root) { write_cr3(root); }

// Новое адресное пространство: нижняя 512-ГБ запись PML4 (identity-карта ядра, видеопамять,
// MMIO) общая с ядром и без бита USER, поэтому Ring 3 её не видит; остальное - своё.
uint64_t vmm_space_create(void) {
    uint64_t page = pmm_alloc_page();
    if (!page) return 0;
    uint64_t* n = (uint64_t*)(uintptr_t)page;
    const uint64_t* k = (const uint64_t*)(uintptr_t)kernel_root;
    for (unsigned i = 0; i < 512; i++) n[i] = 0;
    n[0] = k[0];
    n[MMIO_PML4] = k[MMIO_PML4];   // окно MMIO ядра (без бита USER): одна таблица на все пространства
    n[511] = k[511];   // раннее окно bootmap (без бита USER): ядро может выводить журнал при любом активном CR3
    return page;
}

int vmm_space_map(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags) {
    if (va < VMM_USER_BASE || va >= VMM_USER_TOP) return -1;
    if (flags & VMM_NOCACHE) flags |= VMM_NOEXEC;          /* регистры устройства драйвера Ring 3: исполнять нельзя ни при каких флагах */
    return map_in(root, va, pa, flags | VMM_USER);
}

// Освобождает все страницы пользовательской половины и сами таблицы. Возвращает число
// освобождённых страниц данных. Пространство не должно быть активным.
unsigned vmm_space_destroy(uint64_t root) {
    unsigned freed = 0;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(root & PTE_ADDR);
    for (unsigned a = 1; a < 256; a++) {
        if (!(pml4[a] & PTE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[a] & PTE_ADDR);
        for (unsigned b = 0; b < 512; b++) {
            if (!(pdpt[b] & PTE_PRESENT) || (pdpt[b] & PTE_HUGE)) continue;
            uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[b] & PTE_ADDR);
            for (unsigned c = 0; c < 512; c++) {
                if (!(pd[c] & PTE_PRESENT) || (pd[c] & PTE_HUGE)) continue;
                uint64_t* pt = (uint64_t*)(uintptr_t)(pd[c] & PTE_ADDR);
                for (unsigned d = 0; d < 512; d++) {
                    if ((pt[d] & PTE_PRESENT) && !(pt[d] & VMM_SHARED)) { pmm_free_page(pt[d] & PTE_ADDR); freed++; }
                }
                pmm_free_page((uint64_t)(uintptr_t)pt);
            }
            pmm_free_page((uint64_t)(uintptr_t)pd);
        }
        pmm_free_page((uint64_t)(uintptr_t)pdpt);
        pml4[a] = 0;
    }
    pmm_free_page(root & PTE_ADDR);
    return freed;
}

// Проверка пользовательского буфера в ТЕКУЩЕМ адресном пространстве: диапазон целиком
// в пользовательской половине, и на каждом уровне таблиц стоит бит USER (а для записи -
// ещё и WRITE). Без бита USER ядру нельзя верить, что адрес принадлежит процессу.
int vmm_user_range_ok(uint64_t va, uint64_t len, int write) {
    if (len == 0 || va < VMM_USER_BASE) return 0;
    uint64_t end = va + len;
    if (end < va || end > VMM_USER_TOP) return 0;
    const uint64_t* pml4 = (const uint64_t*)(uintptr_t)(read_cr3() & PTE_ADDR);
    for (uint64_t p = va & ~(PAGE_SIZE - 1); p < end; p += PAGE_SIZE) {
        uint64_t e = pml4[(p >> 39) & 0x1FF];
        if ((e & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return 0;
        const uint64_t* t = (const uint64_t*)(uintptr_t)(e & PTE_ADDR);
        for (int lvl = 0; lvl < 3; lvl++) {
            unsigned idx = (unsigned)((p >> (30 - 9 * lvl)) & 0x1FF);
            e = t[idx];
            if ((e & (PTE_PRESENT | PTE_USER)) != (PTE_PRESENT | PTE_USER)) return 0;
            /* x86 checks R/W at every paging level, not only in the leaf PTE. */
            if (write && !(e & PTE_WRITE)) return 0;
            if (lvl < 2 && (e & PTE_HUGE)) return 0;
            if (lvl < 2) t = (const uint64_t*)(uintptr_t)(e & PTE_ADDR);
        }
    }
    return 1;
}

// SMEP: ядру запрещено исполнять страницы с битом USER (классический шаг эксплойта). Заодно UMIP.
int vmm_enable_smep(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return 0;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    /* UMIP: программам запрещены SGDT/SIDT/SLDT/SMSW/STR - они раскрывали бы адреса таблиц ядра */
    if (c & (1u << 2)) __asm__ volatile ("mov %0, %%cr4" : : "r"(read_cr4() | (1ULL << 11)) : "memory");
    if (!(b & (1u << 7))) return 0;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(read_cr4() | (1ULL << 20)) : "memory");
    return 1;
}

#define MSR_PAT 0x277
// PA0=WB PA1=WC PA2=UC- PA3=UC PA4=WB PA5=WT PA6=UC- PA7=UC (по умолчанию PA1=WT)
#define PAT_VALUE 0x0007040600070106ULL

int vmm_enable_write_combining(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!((d >> 16) & 1)) return -1; // нет PAT
    wrmsr(MSR_PAT, PAT_VALUE);
    wc_enabled = 1;
    write_cr3(read_cr3());           // сброс TLB: старые трансляции не должны жить с прежним типом памяти
    return 0;
}

int vmm_space_unmap(uint64_t root, uint64_t va) {
    if ((va & (PAGE_SIZE - 1)) || va < VMM_USER_BASE || va >= VMM_USER_TOP) return -1;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(root & PTE_ADDR);
    uint64_t* pdpt = next_level(pml4, (va >> 39) & 0x1FF, 0, 0);
    if (!pdpt) return -1;
    uint64_t* pd = next_level(pdpt, (va >> 30) & 0x1FF, 0, 0);
    if (!pd) return -1;
    uint64_t* pt = next_level(pd, (va >> 21) & 0x1FF, 0, 0);
    if (!pt) return -1;
    unsigned idx = (va >> 12) & 0x1FF;
    if (!(pt[idx] & PTE_PRESENT)) return -1;
    pt[idx] = 0;
    return 0;
}

int vmm_unmap_page(uint64_t va) {
    if ((va & (PAGE_SIZE - 1)) || !is_canonical(va)) return -1;

    uint64_t* pml4 = (uint64_t*)(uintptr_t)(read_cr3() & PTE_ADDR);
    uint64_t* pdpt = next_level(pml4, (va >> 39) & 0x1FF, 0, 0);
    if (!pdpt) return -1;
    uint64_t* pd = next_level(pdpt, (va >> 30) & 0x1FF, 0, 0);
    if (!pd) return -1;
    uint64_t* pt = next_level(pd, (va >> 21) & 0x1FF, 0, 0);
    if (!pt) return -1;

    unsigned idx = (va >> 12) & 0x1FF;
    if (!(pt[idx] & PTE_PRESENT)) return -1;
    pt[idx] = 0;
    invlpg(va);
    return 0;
}

int vmm_translate(uint64_t va, uint64_t* pa) {
    if (!is_canonical(va)) return -1;

    uint64_t* t = (uint64_t*)(uintptr_t)(read_cr3() & PTE_ADDR);
    uint64_t e = t[(va >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT)) return -1;

    t = (uint64_t*)(uintptr_t)(e & PTE_ADDR);
    e = t[(va >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT)) return -1;
    if (e & PTE_HUGE) { *pa = (e & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL); return 0; }

    t = (uint64_t*)(uintptr_t)(e & PTE_ADDR);
    e = t[(va >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT)) return -1;
    if (e & PTE_HUGE) { *pa = (e & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL); return 0; }

    t = (uint64_t*)(uintptr_t)(e & PTE_ADDR);
    e = t[(va >> 12) & 0x1FF];
    if (!(e & PTE_PRESENT)) return -1;
    *pa = (e & PTE_ADDR) | (va & 0xFFFULL);
    return 0;
}

int vmm_guard_null_page(void) {
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(read_cr3() & PTE_ADDR);
    uint64_t* pdpt = next_level(pml4, 0, 0, 0);
    if (!pdpt) return -1;
    uint64_t* pd = next_level(pdpt, 0, 0, 0);
    if (!pd) return -1;

    uint64_t e = pd[0];
    if (!(e & PTE_PRESENT) || !(e & PTE_HUGE)) return -1; // ожидаем 2-МБ страницу из boot.s

    uint64_t base = e & 0x000FFFFFFFE00000ULL;
    uint64_t page = pmm_alloc_page();
    if (!page) return -1;

    // Разбиваем первую 2-МБ страницу на 512 страниц по 4 КБ (те же адреса и права),
    // кроме страницы 0: разыменование NULL теперь даёт #PF, а не молча читает IVT.
    uint64_t* pt = (uint64_t*)(uintptr_t)page;
    for (unsigned i = 0; i < 512; i++) {
        pt[i] = (base + (uint64_t)i * PAGE_SIZE) | PTE_PRESENT | PTE_WRITE;
    }
    pt[0] = 0;
    pd[0] = page | PTE_PRESENT | PTE_WRITE;

    write_cr3(read_cr3()); // полный сброс TLB
    return 0;
}

int vmm_init(void) {
    uart_print("[KIRA VMM] Verifying paging state...\n");
    kernel_root = read_cr3() & PTE_ADDR;

    if (!(read_cr0() & CR0_PG) || !(read_cr4() & CR4_PAE) || !(rdmsr(MSR_EFER) & EFER_LMA)) {
        uart_print("[KIRA VMM ERROR] CPU is not in long mode with paging enabled.\n");
        return -1;
    }

    // NXE нужен, чтобы бит 63 (NX) в PTE работал, а не вызывал #PF (reserved bit).
    // WP - запрет записи в read-only страницы и из ядра.
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    write_cr0(read_cr0() | CR0_WP);

    // Самотест 1: адрес кода ядра транслируется в самого себя (identity map).
    uint64_t pa = 0;
    uint64_t self = (uint64_t)(uintptr_t)&vmm_init;
    if (vmm_translate(self, &pa) != 0 || pa != self) {
        uart_print("[KIRA VMM ERROR] Kernel identity mapping check failed.\n");
        return -2;
    }

    // Самотест 2: реальное map -> запись через виртуальный адрес -> чтение через
    // физический -> unmap -> страница больше не транслируется.
    const uint64_t test_va = 0x0000008000000000ULL; // PML4[1], вне identity-карты
    uint64_t page = pmm_alloc_page();
    if (!page) {
        uart_print("[KIRA VMM ERROR] No memory for self-test.\n");
        return -3;
    }
    if (vmm_map_page(test_va, page, VMM_WRITE | VMM_NOEXEC) != 0) {
        uart_print("[KIRA VMM ERROR] Self-test map failed.\n");
        pmm_free_page(page);
        return -3;
    }
    volatile uint64_t* via_virtual = (volatile uint64_t*)(uintptr_t)test_va;
    volatile uint64_t* via_physical = (volatile uint64_t*)(uintptr_t)page;
    *via_virtual = 0x4B49524156414D4DULL;
    int ok = (*via_physical == 0x4B49524156414D4DULL);
    ok = ok && (vmm_translate(test_va, &pa) == 0 && pa == page);
    ok = ok && (vmm_unmap_page(test_va) == 0);
    ok = ok && (vmm_translate(test_va, &pa) != 0);
    pmm_free_page(page);
    if (!ok) {
        uart_print("[KIRA VMM ERROR] Map/translate/unmap self-test failed.\n");
        return -4;
    }

    // Окно MMIO: таблица PDPT для PML4[510] создаётся сейчас, до первого процесса, и дальше общая для всех.
    {
        uint64_t* pml4 = (uint64_t*)(uintptr_t)kernel_root;
        if (pml4[MMIO_PML4] & PTE_PRESENT) {
            uart_print("[KIRA VMM ERROR] PML4[510] is already in use: no room for the MMIO window.\n");
            return -5;
        }
        if (!next_level(pml4, MMIO_PML4, 1, 0)) {
            uart_print("[KIRA VMM ERROR] No memory for the MMIO window table.\n");
            return -5;
        }
    }

    uart_print("[KIRA VMM] 4-level paging verified (identity 0-1 GB via 2 MB pages; map/unmap self-test passed); device MMIO window at 0xFFFFFF0000000000 shared by all address spaces.\n");
    return 0;
}

// Настройки, которые каждый процессор держит у себя: CR0.WP, SMEP, PAT (EFER.NXE ставит трамплин).
void vmm_cpu_init_ap(void) {
    write_cr0(read_cr0() | CR0_WP);
    vmm_enable_smep();
    uaccess_enable_smap();
    if (wc_enabled) wrmsr(MSR_PAT, PAT_VALUE);
}
