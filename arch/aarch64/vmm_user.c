/* ARM64: адресные пространства программ (интерфейс vmm.h, тот же, что у x86-64).
   Корень пространства - своя таблица L0: нижняя половина (L0[0..255]) - ссылки на общие таблицы ядра (тождественная
   карта, окно стеков, устройства), верхняя (L0[256..511]) - память программы. Новые записи ядра в L0 (устройство,
   отображённое позже) сразу переносятся во все живые пространства. У каждого пространства свой ASID: при смене
   пространства TLB не сбрасывается, записи ядра - глобальные, записи программ помечены nG.
   Права страниц программы: AP[1] - доступ из EL0, AP[2] - только чтение, UXN - без исполнения, PXN - ядро никогда
   не исполняет код программы (аналог SMEP). Ядро читает и пишет память программ только командами LDTR/STTR
   (uaccess.S), поэтому при включённом PAN прямое обращение ядра к ней - ошибка доступа (аналог SMAP). */
#include <stdint.h>
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "percpu.h"
#include "spinlock.h"
#include "uart.h"

#define PTE_VALID   1ull
#define PTE_TABLE   3ull
#define PTE_PAGE    3ull
#define PTE_AF      (1ull << 10)
#define PTE_SH_IN   (3ull << 8)
#define PTE_NG      (1ull << 11)
#define PTE_AP_EL0  (1ull << 6)
#define PTE_AP_RO   (1ull << 7)
#define PTE_PXN     (1ull << 53)
#define PTE_UXN     (1ull << 54)
#define PTE_SW_SHARED (1ull << 55)              /* программный бит: страница не принадлежит процессу */
#define PTE_NORMAL_ATTR (2ull << 2)
#define PTE_ADDR    0x0000FFFFFFFFF000ull
#define USER_L0_FIRST 256

extern uint64_t* arm_kernel_l0(void);

/* ---- реестр пространств: корень и ASID ---- */
typedef struct { uint64_t root; uint16_t asid; } space_t;
static space_t* spaces;
static unsigned nspaces, cap_spaces;
static uint8_t* asid_used;                     /* битовая карта ASID (8 или 16 бит - по процессору) */
static unsigned asid_max;
static spinlock_t sp_lock = SPINLOCK_INIT;

static void asid_setup(void) {
    if (asid_max) return;
    uint64_t mmfr0;
    __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    asid_max = ((mmfr0 >> 4) & 0xF) == 2 ? 65535 : 255;          /* ASIDBits: 16 или 8 */
}

/* Под sp_lock. 0 - нет свободного. ASID 0 - ядро. */
static uint16_t asid_alloc(void) {
    if (!asid_used) {
        asid_used = (uint8_t*)(uintptr_t)pmm_alloc_contiguous((uint32_t)((asid_max / 8 + 1 + 4095) / 4096));
        if (!asid_used) return 0;
        for (unsigned i = 0; i <= asid_max / 8; i++) asid_used[i] = 0;
        asid_used[0] = 1;
    }
    for (unsigned a = 1; a <= asid_max; a++)
        if (!(asid_used[a >> 3] & (1u << (a & 7)))) { asid_used[a >> 3] |= (uint8_t)(1u << (a & 7)); return (uint16_t)a; }
    return 0;
}

static space_t* find(uint64_t root) {
    for (unsigned i = 0; i < nspaces; i++) if (spaces[i].root == root) return &spaces[i];
    return 0;
}

uint64_t vmm_kernel_root(void) { return (uint64_t)(uintptr_t)arm_kernel_l0(); }

static uint16_t asid_of(uint64_t root) {
    if (root == vmm_kernel_root()) return 0;
    uint64_t f = spin_lock_irqsave(&sp_lock);
    space_t* s = find(root);
    uint16_t a = s ? s->asid : 0;
    spin_unlock_irqrestore(&sp_lock, f);
    return a;
}

void vmm_switch(uint64_t root) {
    uint64_t v = root | ((uint64_t)asid_of(root) << 48);
    __asm__ volatile ("msr ttbr0_el1, %0\n isb" : : "r"(v) : "memory");
}

/* Ядро добавило запись в свою L0 (нижняя половина): во все пространства программ. */
void vmm_user_kernel_l0_changed(unsigned idx, uint64_t entry) {
    if (idx >= USER_L0_FIRST) return;
    uint64_t f = spin_lock_irqsave(&sp_lock);
    for (unsigned i = 0; i < nspaces; i++) ((uint64_t*)(uintptr_t)spaces[i].root)[idx] = entry;
    __asm__ volatile ("dsb ishst" ::: "memory");
    spin_unlock_irqrestore(&sp_lock, f);
}

static uint64_t zpage(void) {
    uint64_t pa = pmm_alloc_page();
    if (pa) { volatile uint64_t* q = (volatile uint64_t*)(uintptr_t)pa; for (int i = 0; i < 512; i++) q[i] = 0; }
    return pa;
}

uint64_t vmm_space_create(void) {
    asid_setup();
    uint64_t root = zpage();
    if (!root) return 0;
    uint64_t f = spin_lock_irqsave(&sp_lock);
    if (nspaces == cap_spaces) {                                   /* реестр растёт страницами */
        unsigned nc = cap_spaces ? cap_spaces * 2 : 4096 / sizeof(space_t);
        uint32_t pages = (uint32_t)((nc * sizeof(space_t) + 4095) / 4096);
        space_t* ns = (space_t*)(uintptr_t)pmm_alloc_contiguous(pages);
        if (!ns) { spin_unlock_irqrestore(&sp_lock, f); pmm_free_page(root); return 0; }
        for (unsigned i = 0; i < nspaces; i++) ns[i] = spaces[i];
        if (spaces) pmm_free_contiguous((uint64_t)(uintptr_t)spaces, (uint32_t)((cap_spaces * sizeof(space_t) + 4095) / 4096));
        spaces = ns; cap_spaces = nc;
    }
    uint16_t asid = asid_alloc();
    if (!asid) { spin_unlock_irqrestore(&sp_lock, f); pmm_free_page(root); return 0; }
    const uint64_t* k = arm_kernel_l0();
    uint64_t* r = (uint64_t*)(uintptr_t)root;
    for (unsigned i = 0; i < USER_L0_FIRST; i++) r[i] = k[i];
    spaces[nspaces].root = root; spaces[nspaces].asid = asid; nspaces++;
    __asm__ volatile ("dsb ishst\n tlbi aside1is, %0\n dsb ish\n isb" : : "r"((uint64_t)asid << 48) : "memory");   /* ASID мог служить умершему */
    spin_unlock_irqrestore(&sp_lock, f);
    return root;
}

/* Запись L3 для va в пространстве root; create - создавать таблицы. */
static uint64_t* pte(uint64_t root, uint64_t va, int create) {
    uint64_t* t = (uint64_t*)(uintptr_t)root;
    for (int lvl = 0; lvl < 3; lvl++) {
        unsigned idx = (unsigned)(va >> (39 - 9 * lvl)) & 511;
        if (!(t[idx] & PTE_VALID)) {
            if (!create) return 0;
            uint64_t n = zpage();
            if (!n) return 0;
            __asm__ volatile ("dsb ishst" ::: "memory");
            t[idx] = n | PTE_TABLE;
        } else if ((t[idx] & 3) != PTE_TABLE) return 0;
        t = (uint64_t*)(uintptr_t)(t[idx] & PTE_ADDR);
    }
    return &t[(va >> 12) & 511];
}

static int user_va(uint64_t va) { return va >= VMM_USER_BASE && va < VMM_USER_TOP && !(va & 4095); }

int vmm_space_map(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags) {
    if (!user_va(va) || (pa & 4095)) return -1;
    uint64_t* e = pte(root, va, 1);
    if (!e || (*e & PTE_VALID)) return -1;
    uint64_t v = (pa & PTE_ADDR) | PTE_PAGE | PTE_AF | PTE_SH_IN | PTE_NG | PTE_NORMAL_ATTR | PTE_AP_EL0 | PTE_PXN;
    if (!(flags & VMM_WRITE)) v |= PTE_AP_RO;
    if (flags & VMM_NOEXEC) v |= PTE_UXN;
    else {
        /* Код программы записан командами записи данных: кэш данных - до точки единства, кэш команд сбросить,
           иначе ядро процессора может исполнить старое содержимое страницы (на эмуляторе незаметно, на железе - да). */
        for (uint64_t x = pa; x < pa + 4096; x += 64) __asm__ volatile ("dc cvau, %0" : : "r"(x) : "memory");
        __asm__ volatile ("dsb ish\n ic ialluis\n dsb ish\n isb" ::: "memory");
    }
    if (flags & VMM_SHARED) v |= PTE_SW_SHARED;
    __asm__ volatile ("dsb ishst" ::: "memory");
    *e = v;
    __asm__ volatile ("dsb ishst\n isb" ::: "memory");
    return 0;
}

int vmm_space_unmap(uint64_t root, uint64_t va) {
    if (!user_va(va)) return -1;
    uint64_t* e = pte(root, va, 0);
    if (!e || !(*e & PTE_VALID)) return -1;
    *e = 0;
    return 0;                                                      /* сброс TLB - vmm_tlb_shootdown вызывающего */
}

/* TLBI с суффиксом IS рассылается всем ядрам аппаратно - межъядерные прерывания не нужны. */
void vmm_tlb_shootdown(uint64_t root, uint64_t va, uint32_t pages) {
    uint64_t asid = asid_of(root);
    __asm__ volatile ("dsb ishst" ::: "memory");
    if (pages > 64) __asm__ volatile ("tlbi aside1is, %0" : : "r"(asid << 48) : "memory");
    else for (uint32_t i = 0; i < pages; i++)
        __asm__ volatile ("tlbi vae1is, %0" : : "r"((asid << 48) | (((va >> 12) + i) & 0xFFFFFFFFFFFull)) : "memory");
    __asm__ volatile ("dsb ish\n isb" ::: "memory");
}

static unsigned free_level(uint64_t* t, int lvl) {
    unsigned n = 0;
    for (unsigned i = (lvl == 0 ? USER_L0_FIRST : 0); i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_VALID)) continue;
        if (lvl < 3) {
            n += free_level((uint64_t*)(uintptr_t)(e & PTE_ADDR), lvl + 1);
            pmm_free_page(e & PTE_ADDR);
        } else if (!(e & PTE_SW_SHARED)) { pmm_free_page(e & PTE_ADDR); n++; }
        t[i] = 0;
    }
    return n;
}

unsigned vmm_space_destroy(uint64_t root) {
    if (!root || root == vmm_kernel_root()) return 0;
    uint64_t f = spin_lock_irqsave(&sp_lock);
    space_t* s = find(root);
    uint16_t asid = s ? s->asid : 0;
    if (s) { *s = spaces[--nspaces]; }
    if (asid) asid_used[asid >> 3] &= (uint8_t)~(1u << (asid & 7));
    spin_unlock_irqrestore(&sp_lock, f);
    if (asid) __asm__ volatile ("dsb ishst\n tlbi aside1is, %0\n dsb ish\n isb" : : "r"((uint64_t)asid << 48) : "memory");
    unsigned n = free_level((uint64_t*)(uintptr_t)root, 0);
    pmm_free_page(root);
    return n;
}

/* Диапазон отображён в текущем пространстве, доступен программе (и для записи, если write). */
int vmm_user_range_ok(uint64_t va, uint64_t len, int write) {
    if (len == 0) return 1;
    if (va < VMM_USER_BASE || va + len < va || va + len > VMM_USER_TOP) return 0;
    uint64_t root = this_cpu()->active_cr3;
    if (!root || root == vmm_kernel_root()) return 0;
    for (uint64_t p = va & ~4095ull; p < va + len; p += 4096) {
        uint64_t* e = pte(root, p, 0);
        if (!e || !(*e & PTE_VALID) || !(*e & PTE_AP_EL0)) return 0;
        if (write && (*e & PTE_AP_RO)) return 0;
    }
    return 1;
}

/* Переключение потоков: пространство следующего потока (TTBR0 с его ASID). Стек ядра для входа из EL0 - это SP_EL1
   самого потока, его переключает context_switch. */
void proc_arch_switch(const struct thread* next);
#include "sched.h"
void proc_arch_switch(const thread_t* next) {
    uint64_t want = next->cr3 ? next->cr3 : vmm_kernel_root();
    if (want != this_cpu()->active_cr3) {
        __atomic_store_n(&this_cpu()->active_cr3, want, __ATOMIC_SEQ_CST);
        vmm_switch(want);
    }
}
