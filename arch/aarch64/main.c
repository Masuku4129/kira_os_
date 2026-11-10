/* Kira OS на ARM64: подъём платформы. Всё железо берётся из дерева устройств - адресов в коде нет:
   порт PL011, ОЗУ (все узлы memory), контроллер прерываний GICv2 или GICv3, общий таймер ARM, ядра и способ их
   запуска (PSCI через hvc или smc). Включается MMU с тождественной картой (ОЗУ - обычная кэшируемая память,
   остальное - устройства без исполнения), каждое ядро получает свой тик и проверяется.
   Второй этап: поверх платформы работает общее ядро - менеджер страниц, куча, планировщик с очередями на каждом
   ядре, мьютексы ядра; межъядерные сигналы - SGI контроллера GIC. Файловые системы, Разум, программы - дальше. */
#include "wx_arm.h"
#include <stdint.h>
#include "../../fdt.h"
#include "bootinfo.h"
#include "fb_arm.h"
#include "timer.h"
#include "cpu.h"
#include "fpu.h"
#include "percpu.h"
#include "sched.h"
#include "heap.h"
#include "uart.h"
#include "smp.h"
#include "kstack.h"
#include "pmm_arm.h"
#include "pmm.h"
#include "selftest_arm.h"
#include "pci.h"
#include "iommu.h"
#include "blk.h"
#include "experience.h"
#include "fscmd.h"
#include "life.h"
#include "mind_sys.h"
#include "immune.h"
#include "watchdog.h"
#include "clock.h"
#include "proc.h"
#include "syscall.h"
#include "uaccess.h"
#include "progs.h"
#include "kdrv.h"
#include "kfs.h"
#include "klog.h"
#include "input.h"
#include "display.h"
#include "gfxsrv.h"
#include "console.h"
#include "virtio_input.h"
#include "e1000.h"
#include "virtio_net.h"
#include "net.h"
#include "hda.h"
#include "rtc.h"
#include "corefault.h"
#include "usb.h"
#include "rng.h"
#include "spinlock.h"
#include "platform.h"
#include "msi.h"

#define MAX_RAM 64
static uint8_t arm_bootinfo_blob[8192] __attribute__((aligned(16)));

extern char __kernel_start[], __kernel_end[];
extern void secondary_entry(void);

static inline void mmio_w32(uint64_t a, uint32_t v) { *(volatile uint32_t*)(uintptr_t)a = v; }
static inline uint32_t mmio_r32(uint64_t a) { return *(volatile uint32_t*)(uintptr_t)a; }
static inline uint64_t mmio_r64(uint64_t a) { return *(volatile uint64_t*)(uintptr_t)a; }
#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r"(_v)); _v; })
#define SYSREG_W(name, v) __asm__ volatile ("msr " #name ", %0" : : "r"((uint64_t)(v)))
static inline void isb(void) { __asm__ volatile ("isb" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* ---------------- порт PL011 ---------------- */
/* Отладка подъёма: сборка с -DKIRA_EARLY_UART=адрес печатает и до разбора дерева (только для стенда). */
#ifdef KIRA_EARLY_UART
static uint64_t uart = KIRA_EARLY_UART;
#else
static uint64_t uart;
#endif
static volatile int mmu_on;
static volatile uint32_t print_lock;

static void uart_putc(char c) {
    if (mmu_on && c != '\r') klog_putc(c);                       /* копия в журнал: её показывает окно «Терминал» */
    if (!uart) return;
    for (int i = 0; i < 1000000 && (mmio_r32(uart + 0x18) & (1u << 5)); i++) { }   /* FR.TXFF: очередь полна */
    mmio_w32(uart, (uint8_t)c);
}
static int uart_getc(void) {
    if (!uart || (mmio_r32(uart + 0x18) & (1u << 4))) return -1;                    /* FR.RXFE: пусто */
    return (int)(mmio_r32(uart) & 0xFF);
}
/* Замок вывода берётся с выключенными прерываниями: иначе обработчик прерывания на этом же ядре ждал бы сам себя. */
static uint64_t lock(void) {
    uint64_t f = irq_save();
    if (mmu_on) while (__atomic_exchange_n(&print_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("yield");
    return f;
}
static void unlock(uint64_t f) { if (mmu_on) __atomic_store_n(&print_lock, 0, __ATOMIC_RELEASE); irq_restore(f); }
static void puts_raw(const char* s) { for (; *s; s++) { if (*s == '\n') uart_putc('\r'); uart_putc(*s); } }
static void put_dec_raw(uint64_t v) { char b[21]; int i = 20; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); puts_raw(&b[i]); }
static void put_hex_raw(uint64_t v) { char b[19]; b[0] = '0'; b[1] = 'x'; for (int i = 0; i < 16; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (60 - 4 * i)) & 15]; b[18] = 0; puts_raw(b); }
/* Строка целиком под замком: строки разных ядер не перемешиваются. Формат: %s %u %x. */
static void kprintf(const char* f, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, f);
    uint64_t fl = lock();
    for (; *f; f++) {
        if (*f != '%') { if (*f == '\n') uart_putc('\r'); uart_putc(*f); continue; }
        f++;
        if (*f == 's') puts_raw(__builtin_va_arg(ap, const char*));
        else if (*f == 'u') put_dec_raw(__builtin_va_arg(ap, uint64_t));
        else if (*f == 'x') put_hex_raw(__builtin_va_arg(ap, uint64_t));
        else if (*f == '%') uart_putc('%');
    }
    unlock(fl);
    __builtin_va_end(ap);
}

/* Интерфейс uart.h для общего ядра. Вывод не буферизуется (строка уходит в порт сразу под замком), поэтому при
   смене потока досылать нечего. */
void uart_print(const char* s) { uint64_t f = lock(); puts_raw(s); unlock(f); }
void uart_putchar(char c) { char b[2] = { c, 0 }; uart_print(b); }
void uart_print_dec(uint64_t v) { uint64_t f = lock(); put_dec_raw(v); unlock(f); }
void uart_print_hex(uint64_t v) { uint64_t f = lock(); put_hex_raw(v); unlock(f); }
void uart_switch_flush(void) { }
void uart_print_serial(const char* s) { uart_print(s); }
void uart_flush_all(void) { }                                    /* буферов нет: досылать нечего */   /* экрана нет: весь вывод - в порт */
int  uart_getchar(void) { return uart_getc(); }

/* ---------------- память ---------------- */
static struct { uint64_t base, size; } ram[MAX_RAM];
static unsigned nram;
static uint64_t bump, bump_end, bump_start;

static void* palloc(uint64_t bytes, uint64_t align) {          /* страницы для таблиц и стеков - за ядром */
    uint64_t a = (bump + align - 1) & ~(align - 1);
    if (a + bytes > bump_end) return 0;
    bump = a + bytes;
    for (uint64_t i = 0; i < bytes; i += 8) *(volatile uint64_t*)(uintptr_t)(a + i) = 0;
    /* Вторичное ядро пишет в свой стек ещё до включения MMU (мимо кэша): строки, обнулённые здесь через кэш,
       выталкиваются в память и сбрасываются, иначе их вытеснение позже затёрло бы его данные. */
    if (mmu_on) {
        for (uint64_t x = a & ~63ull; x < a + bytes; x += 64) __asm__ volatile ("dc civac, %0" : : "r"(x) : "memory");
        dsb();
    }
    return (void*)(uintptr_t)a;
}

static int in_ram(uint64_t a, uint64_t len) {
    for (unsigned i = 0; i < nram; i++) if (a >= ram[i].base && a + len <= ram[i].base + ram[i].size) return 1;
    return 0;
}
int arm_mem_in_ram(uint64_t a, uint64_t len) { return in_ram(a, len); }
static int touches_ram(uint64_t a, uint64_t len) {
    for (unsigned i = 0; i < nram; i++) if (a < ram[i].base + ram[i].size && ram[i].base < a + len) return 1;
    return 0;
}

/* ---------------- MMU: тождественная карта, 4 КБ гранула, 48 бит ----------------
   MAIR: 0 - устройство nGnRnE, 1 - устройство nGnRE, 2 - обычная память с обратной записью. ОЗУ - блоки по 1 ГБ,
   а где ОЗУ занимает часть гигабайта - по 2 МБ (обращение «наугад» к несуществующей обычной памяти на настоящем
   железе даёт SError, поэтому дыры остаются устройствами без исполнения). */
#define PTE_VALID   1ull
#define PTE_TABLE   3ull
#define PTE_BLOCK   1ull
#define PTE_AF      (1ull << 10)
#define PTE_SH_IN   (3ull << 8)
#define PTE_NORMAL  ((2ull << 2) | PTE_AF | PTE_SH_IN)
#define PTE_DEVICE  ((1ull << 2) | PTE_AF | (1ull << 53) | (1ull << 54))
/* Пул DMA для плат, где устройства не видят кэш процессора (в дереве нет dma-coherent): обычная память без кэша
   (MAIR 3 = 0x44, внешняя общая), без исполнения. Блоки по 2 МБ, поэтому гигабайт с пулом дробится. */
#define PTE_DMA_NC  ((3ull << 2) | PTE_AF | (2ull << 8) | (1ull << 53) | (1ull << 54))
static uint64_t dma_lo, dma_hi, dma_bump;                       /* границы пула и «указатель» выдачи; dma_lo == 0 - пула нет */
static uint64_t* l0;

static uint64_t* next_table(uint64_t* t, unsigned idx) {
    if (!(t[idx] & PTE_VALID)) {
        uint64_t* n = (uint64_t*)palloc(4096, 4096);
        if (!n) return 0;
        t[idx] = (uint64_t)(uintptr_t)n | PTE_TABLE;
    } else if ((t[idx] & 3) != PTE_TABLE) return 0;
    return (uint64_t*)(uintptr_t)(t[idx] & 0x0000FFFFFFFFF000ull);
}

/* Отобразить гигабайт с адресом gb (1 ГБ-выровненный). */
static int map_gb(uint64_t gb) {
    uint64_t* l1 = next_table(l0, (unsigned)(gb >> 39) & 511);
    if (!l1) return -1;
    unsigned i1 = (unsigned)(gb >> 30) & 511;
    if (l1[i1] & PTE_VALID) return 0;
    int has_pool = dma_lo && dma_lo < gb + (1ull << 30) && dma_hi > gb;
    /* Ловушка NULL: первая страница физического адреса 0 не отображается, чтение или запись по нулевому указателю в ядре
       даёт ошибку, а не молча читает флеш/ПЗУ платы (QEMU virt) или портит память по адресу 0. Для этого гигабайт 0
       дробится до 4-КБ страниц. Если ядро само лежит в первых 2 МБ (платы с ОЗУ с нуля), ловушки нет. */
    int guard = gb == 0 && (uint64_t)(uintptr_t)__kernel_start >= (2ull << 20);
    if (!guard && in_ram(gb, 1ull << 30) && !has_pool) { l1[i1] = gb | PTE_NORMAL | PTE_BLOCK; return 0; }
    if (!guard && !touches_ram(gb, 1ull << 30)) { l1[i1] = gb | PTE_DEVICE | PTE_BLOCK; return 0; }
    uint64_t* l2 = next_table(l1, i1);
    if (!l2) return -1;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t a = gb + ((uint64_t)i << 21);
        uint64_t attr = in_ram(a, 1ull << 21) ? PTE_NORMAL : PTE_DEVICE;
        if (dma_lo && a >= dma_lo && a + (1ull << 21) <= dma_hi) attr = PTE_DMA_NC;
        l2[i] = a | attr | PTE_BLOCK;
    }
    if (guard) {
        uint64_t* l3 = (uint64_t*)palloc(4096, 4096);
        if (!l3) return -1;
        for (unsigned i = 0; i < 512; i++) {
            uint64_t a = (uint64_t)i << 12;
            if (i == 0) { l3[0] = 0; continue; }                       /* страница 0 не отображена */
            uint64_t attr = in_ram(a, 4096) ? PTE_NORMAL : PTE_DEVICE;
            if (dma_lo && a >= dma_lo && a + 4096 <= dma_hi) attr = PTE_DMA_NC;
            l3[i] = a | attr | 3ull;                                  /* дескриптор страницы уровня 3 */
        }
        l2[0] = (uint64_t)(uintptr_t)l3 | PTE_TABLE;
    }
    return 0;
}

static uint64_t tcr_val;
static void mmu_enable_this_cpu(void) {
    SYSREG_W(mair_el1, 0x00ull | (0x04ull << 8) | (0xFFull << 16) | (0x44ull << 24));
    SYSREG_W(tcr_el1, tcr_val);
    SYSREG_W(ttbr0_el1, (uint64_t)(uintptr_t)l0);
    dsb(); isb();
    __asm__ volatile ("tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    uint64_t sctlr = SYSREG_R(sctlr_el1);
    sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12) | (1ull << 19);   /* M, C, I, WXN: записываемое не исполняется */
    sctlr &= ~(1ull << 1);                                       /* без проверки выравнивания */
    SYSREG_W(sctlr_el1, sctlr);
    isb();
}

/* Устройства видят память согласованно с кэшем процессора только если дерево устройств так говорит (dma-coherent у
   корня или у узла хоста PCI). Иначе (Raspberry Pi 4 и др.) буферы устройств берутся из пула без кэша. Параметр
   загрузки kira.dma=noncoherent / kira.dma=coherent переопределяет решение (проверка на стенде и обход ошибок
   дерева устройств). */
static int bootarg_has(const void* fdt, const char* word) {
    int c = fdt_find_path(fdt, "/chosen");
    uint32_t len = 0;
    const char* a = c >= 0 ? (const char*)fdt_prop(fdt, c, "bootargs", &len) : 0;
    if (!a) return 0;
    unsigned wl = 0; while (word[wl]) wl++;
    for (uint32_t i = 0; i + wl <= len; i++) {
        if (i && a[i - 1] != ' ') continue;
        unsigned k = 0; while (k < wl && a[i + k] == word[k]) k++;
        if (k == wl && (i + wl == len || a[i + wl] == ' ' || a[i + wl] == 0)) return 1;
    }
    return 0;
}
static int dma_noncoherent(const void* fdt) {
    if (bootarg_has(fdt, "kira.dma=noncoherent")) return 1;
    if (bootarg_has(fdt, "kira.dma=coherent")) return 0;
    uint32_t l;
    if (fdt_prop(fdt, fdt_root(fdt), "dma-coherent", &l)) return 0;
    int pci = fdt_find_compatible(fdt, -1, "pci-host-ecam-generic");
    if (pci >= 0) return fdt_prop(fdt, pci, "dma-coherent", &l) ? 0 : 1;
    int depth = 0;                                               /* нет PCI-узла: любой узел с dma-coherent - согласованная платформа */
    for (int n = fdt_root(fdt); n >= 0; n = fdt_next_node(fdt, n, &depth)) if (fdt_prop(fdt, n, "dma-coherent", &l)) return 0;
    return 1;
}
/* Пул - верх того участка ОЗУ, где лежат ядро и ранние выделения: до 32 МБ (но не больше 1/8 участка), блоки по 2 МБ. */
static void dma_pool_choose(void) {
    for (unsigned i = 0; i < nram; i++) {
        if (!(bump >= ram[i].base && bump < ram[i].base + ram[i].size)) continue;
        uint64_t end = (ram[i].base + ram[i].size) & ~((2ull << 20) - 1), sz = ram[i].size / 8;
        if (sz > (32ull << 20)) sz = 32ull << 20;
        sz &= ~((2ull << 20) - 1);
        if (sz < (2ull << 20) || end < sz || end - sz < bump + (8ull << 20)) return;      /* места нет - пула нет */
        dma_lo = end - sz; dma_hi = end; dma_bump = dma_lo;
        bump_end = dma_lo;                                       /* ранние выделения (таблицы MMU) не залезают в пул */
        return;
    }
}
int platform_dma_noncoherent(void) { return dma_lo != 0; }   /* драйверам Ring 3 DMA без согласованного кэша не отдаётся (kdrv.c) */
void* platform_dma_pool(uint32_t bytes, uint64_t* phys, int below_4g) {
    if (!dma_lo) return 0;
    uint64_t need = ((uint64_t)bytes + 4095) & ~4095ull, f = irq_save(), a = 0;
    static volatile uint32_t lk;
    while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("yield");
    if (dma_bump + need <= dma_hi && (!below_4g || dma_bump + need <= 0x100000000ull)) { a = dma_bump; dma_bump += need; }
    __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
    irq_restore(f);
    if (!a) return 0;
    for (uint64_t i = 0; i < need; i += 8) *(volatile uint64_t*)(uintptr_t)(a + i) = 0;
    dsb();
    *phys = a;
    return (void*)(uintptr_t)a;
}

/* ---------------- W^X: права образа ядра ----------------
   Границы секций - из linker.ld (кратны 4 КБ): текст RX, данные только для чтения (после перемещений boot.S их не
   пишут) RO+PXN, данные и bss RW+PXN; вся остальная карта получает PXN/UXN. Таблицы правятся до включения MMU,
   поэтому переходного состояния с записываемым исполняемым текстом нет. SCTLR.WXN (mmu_enable_this_cpu) дополнительно
   запрещает исполнять любую записываемую страницу, даже если запись в таблицу когда-нибудь ошибётся. */
void kernel_panic(const char* reason) __attribute__((noreturn));
extern char wx_text_start[], wx_text_end[], wx_ro_start[], wx_ro_end[], wx_data_start[], wx_data_end[];
static uint64_t* wx_ptr(uint64_t phys) { return (uint64_t*)(uintptr_t)phys; }
static uint64_t wx_alloc(void) { return (uint64_t)(uintptr_t)palloc(4096, 4096); }
static const wx_env_t wx_env = { wx_ptr, wx_alloc };
static wx_layout_t wx_layout(void) {
    wx_layout_t l;
    l.text_lo = (uint64_t)(uintptr_t)wx_text_start; l.text_hi = (uint64_t)(uintptr_t)wx_text_end;
    l.ro_lo = (uint64_t)(uintptr_t)wx_ro_start;     l.ro_hi = (uint64_t)(uintptr_t)wx_ro_end;
    l.data_lo = (uint64_t)(uintptr_t)wx_data_start; l.data_hi = (uint64_t)(uintptr_t)wx_data_end;
    l.exec_lo = l.exec_hi = 0;
    return l;
}
static int wx_apply(void) {
    wx_layout_t l = wx_layout();
    if (((l.text_lo | l.text_hi | l.ro_lo | l.ro_hi | l.data_lo | l.data_hi) & 4095) || l.text_lo >= l.text_hi ||
        l.text_hi > l.ro_lo || l.ro_hi > l.data_lo) {
        kprintf("[KIRA VMM] ERROR: kernel image is not loaded at a 4 KB boundary or its sections are out of order: W^X impossible\n");
        return -1;
    }
    if (wxa_enforce(&wx_env, l0, &l) != 0) {
        kprintf("[KIRA VMM] ERROR: W^X: cannot split the kernel mapping (no memory or image outside normal memory)\n");
        return -1;
    }
    return 0;
}
static void wx_verify(int announce) {
    wx_layout_t l = wx_layout();
    wx_stats_t st;
    if (wxa_scan(&wx_env, l0, &l, &st) != 0) {
        mmu_on = 0;
        if (st.bad_count) kprintf("[KIRA VMM] ERROR: W^X violated: %u mapping(s) writable and executable, first at %x\n", st.bad_count, st.bad_va);
        if (!st.layout_ok) kprintf("[KIRA VMM] ERROR: kernel image page has wrong rights at %x\n", st.layout_bad_va);
        kernel_panic("W^X for kernel memory violated");
    }
    if (announce) kprintf("[KIRA VMM] W^X kernel: ok (text=%u KB ro=%u KB rw=%u KB)\n", st.exec_kb, st.ro_kb, st.rw_kb);
}

static int mmu_init(uint64_t top) {
    l0 = (uint64_t*)palloc(4096, 4096);
    if (!l0) return -1;
    uint64_t parange = SYSREG_R(id_aa64mmfr0_el1) & 0xF;
    static const unsigned pa_bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    unsigned bits = parange < 7 ? pa_bits[parange] : 48;
    if (bits > 48) { bits = 48; parange = 5; }
    if (top > (1ull << bits)) top = 1ull << bits;
    for (uint64_t gb = 0; gb < top; gb += 1ull << 30) if (map_gb(gb)) return -1;
    if (wx_apply() != 0) return -1;
    tcr_val = 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (parange << 32);
    if (((SYSREG_R(id_aa64mmfr0_el1) >> 4) & 0xF) == 2) tcr_val |= 1ull << 36;   /* TCR.AS: ASID 16 бит */
    mmu_enable_this_cpu();
    mmu_on = 1;
    return (int)bits;
}

/* ---------------- стеки потоков ядра с пустотой ниже ----------------
   Отдельное окно адресов (вне тождественной карты): ячейки по 64 КБ, отображена только верхняя часть размером со
   стек (страницы 4 КБ), ниже - пустота: переполнение стека - ошибка доступа, а не порча соседней памяти.
   Освобождённая ячейка остаётся отображённой и уходит в список свободных (номер следующей - в её первом слове). */
#define KSTACK_VA     0x0000700000000000ull               /* нижняя половина TTBR0: верхняя - программам */
#define KSTACK_STRIDE 0x10000ull
#define KSTACK_SLOTS  65536ull
#define PTE_PAGE      3ull
#define PTE_KDATA     ((2ull << 2) | PTE_AF | PTE_SH_IN | (1ull << 53) | (1ull << 54))
static uint64_t ks_next, ks_free = ~0ull, ks_size;
static uint8_t ks_busy[KSTACK_SLOTS / 8];          /* ячейка выдана и не возвращена: защита от двойного освобождения */
static uint32_t ks_bad;
static volatile uint32_t ks_lock;

void vmm_user_kernel_l0_changed(unsigned idx, uint64_t entry);
uint64_t* arm_kernel_l0(void) { return l0; }

static uint64_t* table_page(uint64_t* t, unsigned idx) {          /* после включения MMU - страницы из pmm */
    if (!(t[idx] & PTE_VALID)) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) return 0;
        for (int i = 0; i < 512; i++) ((volatile uint64_t*)(uintptr_t)pa)[i] = 0;
        dsb();
        t[idx] = pa | PTE_TABLE;
        if (t == l0) vmm_user_kernel_l0_changed(idx, t[idx]);       /* пространства программ видят ядро целиком */
    } else if ((t[idx] & 3) != PTE_TABLE) return 0;
    return (uint64_t*)(uintptr_t)(t[idx] & 0x0000FFFFFFFFF000ull);
}

static int map_kpage(uint64_t va, uint64_t pa) {
    uint64_t* l1 = table_page(l0, (unsigned)(va >> 39) & 511);
    uint64_t* l2 = l1 ? table_page(l1, (unsigned)(va >> 30) & 511) : 0;
    uint64_t* l3 = l2 ? table_page(l2, (unsigned)(va >> 21) & 511) : 0;
    if (!l3) return -1;
    l3[(va >> 12) & 511] = pa | PTE_KDATA | PTE_PAGE;
    __asm__ volatile ("dsb ishst\n isb" ::: "memory");
    return 0;
}

void* vmm_kstack_alloc(uint32_t bytes) {
    bytes = (bytes + 4095) & ~4095u;
    if (bytes >= KSTACK_STRIDE) return 0;
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&ks_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("yield");
    uint64_t va = 0;
    if (ks_size == bytes && ks_free != ~0ull) {
        va = KSTACK_VA + ks_free * KSTACK_STRIDE + KSTACK_STRIDE - bytes;
        ks_busy[ks_free / 8] |= (uint8_t)(1u << (ks_free % 8));
        ks_free = *(volatile uint64_t*)(uintptr_t)va;
    } else if (ks_next < KSTACK_SLOTS && (!ks_size || ks_size == bytes)) {
        uint64_t slot = ks_next, base = KSTACK_VA + slot * KSTACK_STRIDE + KSTACK_STRIDE - bytes;
        int ok = 1;
        for (uint64_t o = 0; o < bytes && ok; o += 4096) {
            uint64_t pa = pmm_alloc_page();
            ok = pa && map_kpage(base + o, pa) == 0;
        }
        if (ok) { ks_busy[slot / 8] |= (uint8_t)(1u << (slot % 8)); ks_next++; ks_size = bytes; va = base; }
    }
    __atomic_store_n(&ks_lock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
    return (void*)(uintptr_t)va;
}

void vmm_kstack_free(void* base) {
    uint64_t va = (uint64_t)(uintptr_t)base;
    if (!vmm_is_kstack(va)) return;
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&ks_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("yield");
    uint64_t off = va - KSTACK_VA, slot = off / KSTACK_STRIDE;
    /* Список свободных хранится в самих стеках: дубль в нём замкнул бы цикл и выдал один стек двум потокам.
       Освобождается только выданная ячейка и только по адресу начала её стека. */
    if (!ks_size || off % KSTACK_STRIDE != KSTACK_STRIDE - ks_size || !(ks_busy[slot / 8] & (1u << (slot % 8)))) {
        ks_bad++;
        __atomic_store_n(&ks_lock, 0, __ATOMIC_RELEASE);
        irq_restore(f);
        kprintf("[KIRA VMM] rejected kernel stack free: slot not allocated (double free or foreign address)\n");
        return;
    }
    ks_busy[slot / 8] &= (uint8_t)~(1u << (slot % 8));
    *(volatile uint64_t*)(uintptr_t)va = ks_free;
    ks_free = slot;
    __atomic_store_n(&ks_lock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

uint32_t vmm_kstack_bad_frees(void) { return ks_bad; }

/* Write-combining x86 (PAT) нужен кадровому буферу от загрузчика (fb_init, multiboot) - на ARM64 его нет: экран
   идёт через virtio-gpu, изображение лежит в обычной кэшируемой памяти, устройство забирает его командой переноса. */
int vmm_enable_write_combining(void) { return -1; }

int vmm_is_kstack(uint64_t a) { return a >= KSTACK_VA && a < KSTACK_VA + KSTACK_SLOTS * KSTACK_STRIDE; }
int vmm_kstack_guard_hit(uint64_t a) { return vmm_is_kstack(a) && ks_size && (a - KSTACK_VA) % KSTACK_STRIDE < KSTACK_STRIDE - ks_size; }

/* ---------------- окно устройств и проверка отображения (vmm.h) ----------------
   Тождественная карта уже покрывает ОЗУ и первые 4 ГБ; регистры устройств выше (ECAM, окна PCIe за 4 ГБ)
   отображаются по требованию гигабайтными блоками «устройство, без исполнения». */
static volatile uint32_t map_lock;
volatile void* vmm_map_mmio(uint64_t pa, uint64_t len, int cache) {
    (void)cache;                                                 /* устройства - всегда nGnRE */
    if (!len || pa + len < pa || pa + len > (1ull << 48)) return 0;
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&map_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("yield");
    int ok = 1;
    for (uint64_t gb = pa & ~0x3FFFFFFFull; gb < pa + len && ok; gb += 1ull << 30) {
        uint64_t* l1 = table_page(l0, (unsigned)(gb >> 39) & 511);
        if (!l1) { ok = 0; break; }
        unsigned i1 = (unsigned)(gb >> 30) & 511;
        if (!(l1[i1] & PTE_VALID)) { l1[i1] = gb | PTE_DEVICE | PTE_BLOCK; __asm__ volatile ("dsb ishst\n isb" ::: "memory"); }
    }
    __atomic_store_n(&map_lock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
    return ok ? (volatile void*)(uintptr_t)pa : 0;
}

/* Обход таблиц: адрес отображён и куда. 0 - да. */
int vmm_translate(uint64_t va, uint64_t* pa) {
    uint64_t* t = l0;
    for (int lvl = 0; lvl < 4; lvl++) {
        unsigned sh = 39 - 9 * lvl;
        uint64_t e = t[(va >> sh) & 511];
        if (!(e & PTE_VALID)) return -1;
        if (lvl == 3 || (e & 3) == PTE_BLOCK) {
            if (lvl == 0) return -1;
            uint64_t mask = (1ull << sh) - 1;
            if (pa) *pa = (e & 0x0000FFFFFFFFF000ull & ~mask) | (va & mask);
            return 0;
        }
        t = (uint64_t*)(uintptr_t)(e & 0x0000FFFFFFFFF000ull);
    }
    return -1;
}

/* ---------------- GIC ---------------- */
static int gic_v3;
static uint64_t gicd, gicc, gicr_base, gicr_size;
static unsigned timer_intid;

#define ICC_SRE_EL1     S3_0_C12_C12_5
#define ICC_PMR_EL1     S3_0_C4_C6_0
#define ICC_IGRPEN1_EL1 S3_0_C12_C12_7
#define ICC_IAR1_EL1    S3_0_C12_C12_0
#define ICC_EOIR1_EL1   S3_0_C12_C12_1
#define XSTR(x) #x
#define SREG_R(n) ({ uint64_t _v; __asm__ volatile ("mrs %0, " XSTR(n) : "=r"(_v)); _v; })
#define SREG_W(n, v) __asm__ volatile ("msr " XSTR(n) ", %0" : : "r"((uint64_t)(v)))

static void gic_dist_init(void) {
    if (gic_v3) {
        mmio_w32(gicd, (1u << 4) | (1u << 1));                  /* GICD_CTLR: ARE_NS, EnableGrp1NS */
        for (int i = 0; i < 100000 && (mmio_r32(gicd) & (1u << 31)); i++) { }    /* RWP */
    } else {
        mmio_w32(gicd, 1);                                       /* GICD_CTLR: Enable */
    }
}

/* Редистрибьютор этого ядра (GICv3): по сродству в GICR_TYPER. */
static uint64_t my_gicr(void) {
    uint64_t mp = SYSREG_R(mpidr_el1);
    uint32_t aff = (uint32_t)(((mp >> 32) & 0xFF) << 24 | (mp & 0xFFFFFF));
    for (uint64_t r = gicr_base; r < gicr_base + gicr_size; ) {
        uint64_t typer = mmio_r64(r + 0x8);
        if ((uint32_t)(typer >> 32) == aff) return r;
        if (typer & (1u << 4)) break;                            /* Last */
        r += (typer & (1u << 1)) ? 0x40000 : 0x20000;           /* VLPIS: 4 кадра, иначе 2 */
    }
    return 0;
}

#define SGI_RESCHED 1
#define SGI_HALT    2
#define ICC_SGI1R_EL1 S3_0_C12_C11_5
static uint8_t gic_mask_me;                                      /* GICv2: маска интерфейса этого ядра (на время init) */

/* ---------------- прерывания сообщениями (msi.h) ----------------
   GICv2m: устройство пишет номер SPI в регистр SETSPI_NS кадра MSI - контроллер поднимает этот SPI (по фронту).
   GICv3: ITS переводит запись устройства (номер устройства на шине PCI + номер события) в LPI загрузочного ядра. */
static uint64_t v2m;
static unsigned v2m_spi, v2m_n, v2m_used;
#define MSI_MAX 16
static struct { msi_fn fn; void* ctx; } msis[MSI_MAX];
static spinlock_t msi_lk = SPINLOCK_INIT;

/* ---- ITS (GICv3) ---- */
#define LPI_BASE 8192u
#define LPI_IDBITS 14u                                           /* номера LPI до 16383 */
static uint64_t its, its_rd;                                     /* кадр ITS; цель коллекции 0 (адрес или номер RD) */
static uint8_t* lpi_cfg;                                         /* таблица настроек LPI (байт на LPI) */
static uint8_t* its_cmdq; static uint32_t its_cw;
static unsigned lpi_used;
static struct { msi_fn fn; void* ctx; } lpis[MSI_MAX];
static struct { uint16_t id; uint8_t* itt; } its_devs[MSI_MAX];
static unsigned its_ndev;

static void w64(uint64_t a, uint64_t v) { *(volatile uint64_t*)(uintptr_t)a = v; }
/* Таблицы в обычной памяти: контроллер может читать мимо кэша процессора - записанное дописать до памяти. */
static void flush(const void* p, uint64_t n) {
    for (uint64_t a = (uint64_t)(uintptr_t)p & ~63ull; a < (uint64_t)(uintptr_t)p + n; a += 64) __asm__ volatile ("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
static int its_cmd(uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3) {
    uint64_t* c = (uint64_t*)(its_cmdq + its_cw);
    c[0] = d0; c[1] = d1; c[2] = d2; c[3] = d3;
    flush(c, 32);
    its_cw = (its_cw + 32) % 4096;
    w64(its + 0x88, its_cw);                                     /* GITS_CWRITER */
    for (int i = 0; i < 1000000; i++) {
        uint64_t rd = mmio_r64(its + 0x90);                      /* GITS_CREADR: бит 0 - ошибка очереди */
        if (rd & 1) return -1;
        if ((rd & 0xFFFE0) == its_cw) return 0;
    }
    return -1;
}
static uint64_t pages(unsigned n) {
    uint64_t pa = pmm_alloc_contiguous(n);
    if (pa) { for (uint64_t i = 0; i < (uint64_t)n * 4096; i++) ((volatile uint8_t*)(uintptr_t)pa)[i] = 0; flush((void*)(uintptr_t)pa, (uint64_t)n * 4096); }
    return pa;
}

/* LPI в редистрибуторе загрузочного ядра и сам ITS. 0 - готово. */
static int its_init(const void* fdt) {
    int n = fdt_find_compatible(fdt, -1, "arm,gic-v3-its");
    uint64_t sz;
    if (n < 0 || fdt_reg(fdt, n, 0, &its, &sz) != 0) { its = 0; return -1; }
    uint64_t rd = my_gicr();
    if (!rd || !(mmio_r64(rd + 0x8) & 1)) { its = 0; return -2; }   /* GICR_TYPER.PLPIS */
    /* таблица настроек: байт на LPI с 8192; таблица ожидания: бит на номер, выровнена на 64 КБ */
    uint64_t cfg = pages(((1u << LPI_IDBITS) - LPI_BASE) / 4096), pend = pmm_alloc_contiguous(32);
    if (!cfg || !pend) { its = 0; return -3; }
    uint64_t pend64 = (pend + 0xFFFF) & ~0xFFFFull;             /* 32 страницы: внутри есть выровненные 64 КБ */
    for (uint64_t i = 0; i < 0x10000 / 8 && pend64 + i * 8 < pend + 32 * 4096; i++) ((volatile uint64_t*)(uintptr_t)pend64)[i] = 0;
    flush((void*)(uintptr_t)pend64, (1u << LPI_IDBITS) / 8);
    lpi_cfg = (uint8_t*)(uintptr_t)cfg;
    w64(rd + 0x70, cfg | (7ull << 7) | (1ull << 10) | (LPI_IDBITS - 1));   /* PROPBASER: WB, внутр. общая, 14 бит */
    w64(rd + 0x78, pend64 | (7ull << 7) | (1ull << 10));                   /* PENDBASER */
    mmio_w32(rd, mmio_r32(rd) | 1);                                        /* GICR_CTLR.EnableLPIs */
    /* ITS: таблицы устройств и коллекций (плоские), очередь команд 4 КБ */
    uint64_t typer = mmio_r64(its + 0x8);
    unsigned devbits = (unsigned)((typer >> 13) & 31) + 1;
    if (devbits > 16) devbits = 16;                              /* номер устройства PCI - 16 бит */
    for (unsigned b = 0; b < 8; b++) {
        uint64_t reg = its + 0x100 + 8 * b, v = mmio_r64(reg);
        unsigned type = (unsigned)((v >> 56) & 7), esz = (unsigned)((v >> 48) & 31) + 1;
        if (type != 1 && type != 4) continue;                    /* 1 - устройства, 4 - коллекции */
        uint64_t entries = type == 1 ? (1ull << devbits) : 64, bytes = entries * esz, np = (bytes + 4095) / 4096;
        if (np > 256) np = 256;
        uint64_t t = pages((unsigned)np);
        if (!t) { its = 0; return -4; }
        w64(reg, (1ull << 63) | (7ull << 59) | ((uint64_t)type << 56) | ((uint64_t)(esz - 1) << 48) | t | (1ull << 10) | (np - 1));
    }
    uint64_t q = pages(1);
    if (!q) { its = 0; return -5; }
    its_cmdq = (uint8_t*)(uintptr_t)q; its_cw = 0;
    w64(its + 0x80, (1ull << 63) | (7ull << 59) | q | (1ull << 10));       /* GITS_CBASER: 1 страница */
    w64(its + 0x88, 0);
    mmio_w32(its, mmio_r32(its) | 1);                                      /* GITS_CTLR.Enabled */
    /* коллекция 0 -> редистрибутор загрузочного ядра: адрес (PTA) или номер процессора */
    its_rd = (typer & (1ull << 19)) ? (rd & 0xFFFFFFFFF0000ull) : ((mmio_r64(rd + 0x8) >> 8) & 0xFFFF) << 16;
    if (its_cmd(0x09, 0, (1ull << 63) | its_rd, 0) || its_cmd(0x05, 0, its_rd, 0)) { its = 0; return -6; }   /* MAPC, SYNC */
    kprintf("[KIRA GIC] ITS at %x: LPIs on the boot core, device id bits %u - PCI interrupts by message (MSI/MSI-X)\n", its, (uint64_t)devbits);
    return 0;
}

static int its_alloc(const pci_dev_t* d, msi_fn fn, void* ctx, uint64_t* addr, uint32_t* data) {
    if (!its || !d) return -1;
    uint64_t f = spin_lock_irqsave(&msi_lk);
    int r = -1;
    uint16_t devid = (uint16_t)(d->bus << 8 | d->dev << 3 | d->fn);
    if (lpi_used < MSI_MAX && its_ndev < MSI_MAX) {
        unsigned i = lpi_used, lpi = LPI_BASE + i;
        uint64_t itt = pages(1);                                 /* таблица событий устройства: 2 события */
        if (itt) {
            lpis[i].fn = fn; lpis[i].ctx = ctx;
            lpi_cfg[lpi - LPI_BASE] = 0xA0 | 1;                 /* приоритет, включён */
            flush(&lpi_cfg[lpi - LPI_BASE], 1);
            if (!its_cmd(0x08 | (uint64_t)devid << 32, 0, (1ull << 63) | itt, 0) &&               /* MAPD: 1 бит события */
                !its_cmd(0x0A | (uint64_t)devid << 32, (uint64_t)lpi << 32, 0, 0) &&              /* MAPTI: событие 0 -> LPI, коллекция 0 */
                !its_cmd(0x0D, 0, 0, 0) && !its_cmd(0x05, 0, its_rd, 0)) {                         /* INVALL, SYNC */
                __atomic_store_n(&lpi_used, i + 1, __ATOMIC_RELEASE);
                its_devs[its_ndev].id = devid; its_devs[its_ndev].itt = (uint8_t*)(uintptr_t)itt; its_ndev++;
                *addr = its + 0x10040;                           /* GITS_TRANSLATER */
                *data = 0;                                       /* событие 0 этого устройства */
                r = 0;
            }
        }
    }
    spin_unlock_irqrestore(&msi_lk, f);
    return r;
}

int msi_alloc(const pci_dev_t* d, msi_fn fn, void* ctx, uint64_t* addr, uint32_t* data) {
    if (its) return its_alloc(d, fn, ctx, addr, data);
    if (!v2m) return -1;
    uint64_t f = spin_lock_irqsave(&msi_lk);
    if (v2m_used >= v2m_n || v2m_used >= MSI_MAX) { spin_unlock_irqrestore(&msi_lk, f); return -1; }
    unsigned i = v2m_used, spi = v2m_spi + i;
    msis[i].fn = fn; msis[i].ctx = ctx;
    __atomic_store_n(&v2m_used, i + 1, __ATOMIC_RELEASE);
    uint64_t cfg = gicd + 0xC00 + (spi / 16) * 4;
    mmio_w32(cfg, mmio_r32(cfg) | (2u << ((spi % 16) * 2)));                       /* GICD_ICFGR: по фронту */
    *(volatile uint8_t*)(uintptr_t)(gicd + 0x400 + spi) = 0x80;                    /* приоритет */
    *(volatile uint8_t*)(uintptr_t)(gicd + 0x800 + spi) = *(volatile uint8_t*)(uintptr_t)(gicd + 0x800);   /* этому ядру */
    mmio_w32(gicd + 0x100 + (spi / 32) * 4, 1u << (spi % 32));                     /* GICD_ISENABLER */
    spin_unlock_irqrestore(&msi_lk, f);
    *addr = v2m + 0x40;                                                            /* MSI_SETSPI_NS */
    *data = spi;
    return 0;
}

static int gic_cpu_init(void) {
    if (gic_v3) {
        uint64_t r = my_gicr();
        if (!r) return -1;
        mmio_w32(r + 0x14, mmio_r32(r + 0x14) & ~(1u << 1));     /* GICR_WAKER.ProcessorSleep = 0 */
        for (int i = 0; i < 1000000 && (mmio_r32(r + 0x14) & (1u << 2)); i++) { }   /* ChildrenAsleep */
        uint64_t sgi = r + 0x10000;
        mmio_w32(sgi + 0x80, mmio_r32(sgi + 0x80) | (1u << timer_intid) | (1u << SGI_RESCHED) | (1u << SGI_HALT));   /* IGROUPR0: группа 1 */
        *(volatile uint8_t*)(uintptr_t)(sgi + 0x400 + timer_intid) = 0x80;  /* приоритет */
        *(volatile uint8_t*)(uintptr_t)(sgi + 0x400 + SGI_RESCHED) = 0x80;
        *(volatile uint8_t*)(uintptr_t)(sgi + 0x400 + SGI_HALT) = 0x00;    /* остановка при аварии - важнее всего */
        mmio_w32(sgi + 0x100, (1u << timer_intid) | (1u << SGI_RESCHED) | (1u << SGI_HALT));   /* ISENABLER0 */
        SREG_W(ICC_SRE_EL1, SREG_R(ICC_SRE_EL1) | 1);
        isb();
        SREG_W(ICC_PMR_EL1, 0xFF);
        SREG_W(ICC_IGRPEN1_EL1, 1);
        isb();
    } else {
        *(volatile uint8_t*)(uintptr_t)(gicd + 0x400 + timer_intid) = 0x80;   /* GICD_IPRIORITYR (банк ядра) */
        *(volatile uint8_t*)(uintptr_t)(gicd + 0x400 + SGI_RESCHED) = 0x80;
        *(volatile uint8_t*)(uintptr_t)(gicd + 0x400 + SGI_HALT) = 0x00;
        mmio_w32(gicd + 0x100, (1u << timer_intid) | (1u << SGI_RESCHED) | (1u << SGI_HALT));   /* GICD_ISENABLER0 */
        gic_mask_me = *(volatile uint8_t*)(uintptr_t)(gicd + 0x800);         /* GICD_ITARGETSR0: бит этого ядра */
        mmio_w32(gicc + 0x4, 0xFF);                                          /* GICC_PMR */
        mmio_w32(gicc, 1);                                                   /* GICC_CTLR */
    }
    return 0;
}

static uint32_t gic_ack(void) { return gic_v3 ? (uint32_t)SREG_R(ICC_IAR1_EL1) : mmio_r32(gicc + 0xC); }
static void gic_eoi(uint32_t iar) { if (gic_v3) SREG_W(ICC_EOIR1_EL1, iar); else mmio_w32(gicc + 0x10, iar); }

/* ---------------- ядра и таймер ---------------- */
#define CPU_MAX 256
typedef struct { uint64_t mpidr; volatile uint64_t ticks; volatile uint32_t online; uint32_t gic_ok; uint8_t gic_mask; } cpu_t;
static cpu_t cpus[CPU_MAX];
static unsigned ncpu;
static uint64_t cnt_freq, tick_cycles;

static unsigned my_cpu(void) {
    uint64_t mp = SYSREG_R(mpidr_el1) & 0xFF00FFFFFFull;
    for (unsigned i = 0; i < ncpu; i++) if (cpus[i].mpidr == mp) return i;
    return 0;
}
static uint64_t now_cnt(void) { isb(); return SYSREG_R(cntvct_el0); }

static void timer_start(void) {
    SYSREG_W(cntv_tval_el0, tick_cycles);
    SYSREG_W(cntv_ctl_el0, 1);                                   /* ENABLE, прерывание не замаскировано */
}

/* Общее время в тиках: из счётчика (не из числа прерываний), двигается вперёд сравнением-обменом любым ядром. */
static volatile uint64_t gticks;
static uint64_t anchor_cnt;
static volatile int sched_up, percpu_up, halting;
static unsigned boot_cpu;
/* Общего замка тика на ARM64 нет (время двигается сравнением-обменом): в аварийном отчёте - нули. */
void timer_lock_state(uint32_t* next, uint32_t* owner) { *next = 0; *owner = 0; }

static void ticks_advance(void) {
    uint64_t d = (now_cnt() - anchor_cnt) / tick_cycles, cur = gticks;
    while (d > cur && !__atomic_compare_exchange_n(&gticks, &cur, d, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { }
}
/* Время в тиках - по счётчику, а не по числу обработанных прерываний: запоздавшее прерывание (эмуляция, долгий
   участок с выключенными прерываниями) не делает срок сна короче. До запуска таймера - то, что насчитано. */
uint64_t timer_get_ticks(void) { if (tick_cycles) ticks_advance(); return gticks; }

static void send_sgi(unsigned cpu, unsigned id) {
    uint64_t mp = cpus[cpu].mpidr;
    if (gic_v3) {
        uint64_t aff0 = mp & 0xFF;
        uint64_t v = (((mp >> 32) & 0xFF) << 48) | (((mp >> 16) & 0xFF) << 32) | ((uint64_t)id << 24) |
                     (((mp >> 8) & 0xFF) << 16) | ((aff0 >> 4) << 44) | (1ull << (aff0 & 15));
        __asm__ volatile ("dsb ishst" ::: "memory");
        SREG_W(ICC_SGI1R_EL1, v);
        isb();
    } else {
        dsb();
        mmio_w32(gicd + 0xF00, ((uint32_t)cpus[cpu].gic_mask << 16) | id);   /* GICD_SGIR */
    }
}

/* ---- межъядерные сигналы планировщика (smp.h) ---- */
void smp_resched_cpu(unsigned cpu) {
    if (cpu >= percpu_count()) return;
    percpu_t* p = &percpu_table[cpu];
    if (p == this_cpu() || !p->sched_on || p->need_resched) return;
    send_sgi(cpu, SGI_RESCHED);
}
void smp_kick_idle(cpumask_t affinity) {
    percpu_t* me = this_cpu();
    for (unsigned i = 0; i < percpu_count(); i++) {
        percpu_t* p = &percpu_table[i];
        if (p == me || !p->sched_on || !cm_test(&affinity, i)) continue;
        if (p->current != p->idle || p->need_resched) continue;
        send_sgi(i, SGI_RESCHED);
        return;
    }
}
/* Простаивающее ядро выключает тик (кроме загрузочного - хранителя времени): будят его сигналом. */
void smp_idle_tick_off(void) {
    percpu_t* c = this_cpu();
    if (c->tick_off || c->cpu_id == boot_cpu) return;
    uint64_t f = irq_save();
    if (c->sched_on && c->current == c->idle && !c->need_resched) { SYSREG_W(cntv_ctl_el0, 0); c->tick_off = 1; }
    irq_restore(f);
}
void smp_idle_tick_on(void) {
    percpu_t* c = this_cpu();
    if (!c->tick_off) return;
    c->tick_off = 0;
    timer_start();
}
int smp_halting(void) { return halting; }
/* Авария: остальные ядра встают (сигнал SGI с высшим приоритетом; ядро с выключенными прерываниями его не примет -
   на ARM нет немаскируемого прерывания, как NMI у x86, его роль выполнит псевдо-NMI GICv3 позже). */
void smp_halt_others(void) {
    if (__atomic_exchange_n(&halting, 1, __ATOMIC_ACQ_REL)) for (;;) __asm__ volatile ("msr daifset, #2\n wfi");
    for (unsigned i = 0; i < ncpu; i++) if (cpus[i].online && (!percpu_up || i != this_cpu()->cpu_id)) send_sgi(i, SGI_HALT);
}



const void* fpu_initial_state(void) {
    static uint8_t z[FPU_STATE_SIZE] __attribute__((aligned(16)));   /* FPCR = 0: округление к ближайшему, без ловушек */
    return z;
}

void kernel_panic(const char* reason) __attribute__((noreturn));
void kernel_panic(const char* reason) {
    __asm__ volatile ("msr daifset, #2");
    smp_halt_others();
    mmu_on = 0;                                                  /* вывод без замка: его мог держать упавший код */
    kprintf("\n[KIRA KERNEL PANIC] %s\n", reason);
    if (sched_up) sched_crash_report(uart_print);
    kprintf("[KIRA ARM64] Halting.\n");
    for (;;) __asm__ volatile ("wfi");
}

static const char* ec_name(unsigned ec) {
    switch (ec) {
    case 0x15: return "SVC";
    case 0x20: case 0x21: return "instruction abort";
    case 0x24: case 0x25: return "data abort";
    case 0x22: return "PC alignment";
    case 0x26: return "SP alignment";
    case 0x00: return "unknown instruction";
    case 0x3C: return "BRK";
    default: return "other";
    }
}

/* Проба адреса (адрес дерева в x0 не обязан быть годным: часть загрузчиков оставляет там мусор): ошибка доступа во
   время пробы не авария - команда пропускается, проба считается неудачной. */
static volatile int probing, probe_failed;

/* Синхронное исключение программы: SVC - системный вызов (номер в x8, аргументы x0..x2, результат в x0);
   ошибка доступа в резерве стека - стек растёт; любая другая ошибка - процесс завершается, ядро живёт. */
static void user_exception(uint64_t* frame) {
    uint64_t esr = frame[33], far = frame[34];
    unsigned ec = (unsigned)(esr >> 26) & 0x3F;
    if (ec == 0x15) {
        cpu_irq_on();                                            /* вызов вытесняем, как на x86 */
        frame[0] = syscall_handler(frame[8], frame[0], frame[1], frame[2]);
        cpu_irq_off();
        return;
    }
    if (ec == 0x24) {
        unsigned dfsc = (unsigned)esr & 0x3F;
        uint64_t err = (dfsc & 0x3C) == 0x0C ? 1 : 0;           /* ошибка прав (страница есть) - не рост стека */
        if (proc_stack_fault(far, err)) return;
    }
    cpu_irq_on();
    kprintf("[KIRA PROC] pid %u: %s at PC %x, address %x (ESR %x): process terminated, the kernel keeps running.\n",
            (uint64_t)proc_current_pid(), ec_name(ec), frame[31], far, esr);
    proc_exit(-1);
}

/* ---------------- изоляция сбоев ядра (как exceptions.c на x86-64, решение - общий corefault.c) ----------------
   Синхронный сбой кода ядра в обычном потоке без замков - гибнет поток; SError (асинхронная ошибка памяти/шины -
   отказ «железа» под этим ядром) - ядро выходит из работы, остальные продолжают; иначе - авария системы. */
/* Смещения полей percpu, которые читает вход исключений (boot.S), и размер стека потока, по которому вход
   узнаёт охранную зону (верхние 16 КБ 64-КБ слота - стек). */
_Static_assert(__builtin_offsetof(percpu_t, cpu_id) == 24 && __builtin_offsetof(percpu_t, spare_sp) == 216 &&
               __builtin_offsetof(percpu_t, spare_x1) == 224 && __builtin_offsetof(percpu_t, ovf_sp) == 232, "percpu layout for boot.S");
_Static_assert(SCHED_STACK_SIZE == 16384 && KSTACK_STRIDE == 0x10000 && KSTACK_VA == 0x0000700000000000ull, "kernel stack layout for boot.S");
#define SPARE_STACK 8192
static uint8_t spare_stacks[PERCPU_MAX_CPUS][SPARE_STACK] __attribute__((aligned(16)));
void arm_spare_stack_install(percpu_t* p) { p->spare_sp = (uint64_t)(uintptr_t)(spare_stacks[p - percpu_table] + SPARE_STACK); }

static unsigned other_live_cores(const percpu_t* me) {
    unsigned n = 0;
    for (unsigned i = 0; i < percpu_count(); i++) {
        const percpu_t* p = &percpu_table[i];
        if (p != me && p->sched_on && p->online && !p->failed) n++;
    }
    return n;
}

static const char* corefault_arm(uint64_t kind, unsigned ec, uint64_t pc, uint64_t far) {
    percpu_t* c = this_cpu();
    thread_t* t = c->sched_on ? (thread_t*)sched_current() : 0;
    int serror = kind == 3 || kind == 7;
    corefault_input_t in = {
        .vector = serror ? 18u : 14u,                            /* как на x86: 18 - отказ ядра, 14 - ошибка кода */
        .locks_held = c->locks_held, .irq_depth = c->irq_depth, .in_fault = c->in_fault,
        .mutexes_held = t ? t->mutexes_held : 0, .have_thread = t != 0, .service = t ? (t->flags & THREAD_F_SERVICE) != 0 : 0,
        .other_cores = other_live_cores(c),
        .irq_move_ok = c->cpu_id != 0,                           /* прерывания устройств (SPI, ITS) идут на ядро 0 */
    };
    corefault_action_t act = corefault_decide(&in);
    c->in_fault = 1; c->fail_vector = ec; c->fail_rip = pc;
    if (act == COREFAULT_PANIC) return corefault_why_panic(&in);
    if (act == COREFAULT_KILL_THREAD) {
        kprintf("[KIRA CORE %u] kernel-mode fault (%s) at PC %x, FAR %x, thread '%s'. Thread terminated, the core and the system keep running.\n",
                (uint64_t)c->cpu_id, ec_name(ec), pc, far, t->name);
        c->in_fault = 0;
        __asm__ volatile ("msr daifclr, #2");
        if (proc_in_user_thread()) proc_exit(-1);
        sched_exit();
    }
    kprintf("[KIRA CORE %u] SError (asynchronous hardware error) at PC %x, thread '%s'. This core is taken out of service; the others continue.\n",
            (uint64_t)c->cpu_id, pc, t ? t->name : "-");
    unsigned orphans = sched_core_fail();
    SYSREG_W(cntv_ctl_el0, 2);                                   /* тик этого ядра - замаскирован */
    kprintf("[KIRA CORE %u] out of service: %u pinned thread(s) ended.\n", (uint64_t)c->cpu_id, (uint64_t)orphans);
    for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");
}

void arm_exception(uint64_t* frame, uint64_t kind) {
    if ((kind == 0 || kind == 4) && probing) {
        unsigned ec0 = (unsigned)(frame[33] >> 26) & 0x3F;
        if (ec0 == 0x24 || ec0 == 0x25) { probe_failed = 1; frame[31] += 4; return; }
    }
    if (kind == 1 || kind == 5 || kind == 9) {                   /* IRQ */
        uint32_t iar = gic_ack();
        uint32_t id = iar & 0xFFFFFF;
        if (id >= 1020 && id <= 1023) return;                    /* ложное */
        percpu_t* c = percpu_up ? this_cpu() : 0;
        if (c) { c->irq_rip = frame[31]; c->irq_rbp = frame[29]; c->irq_depth++; }
        if (id == timer_intid) {
            SYSREG_W(cntv_tval_el0, tick_cycles);
            cpus[c ? c->cpu_id : my_cpu()].ticks++;
            ticks_advance();
            if (sched_up) { watchdog_check(); sched_tick(); }
        } else if (its && id >= LPI_BASE && id < LPI_BASE + __atomic_load_n(&lpi_used, __ATOMIC_ACQUIRE)) {
            unsigned i = id - LPI_BASE;                          /* сообщение устройства через ITS (LPI) */
            if (lpis[i].fn) lpis[i].fn(lpis[i].ctx);
        } else if (v2m && id >= v2m_spi && id < v2m_spi + __atomic_load_n(&v2m_used, __ATOMIC_ACQUIRE)) {
            unsigned i = id - v2m_spi;                           /* сообщение устройства (MSI через GICv2m) */
            if (msis[i].fn) msis[i].fn(msis[i].ctx);
        } else if (id == SGI_RESCHED) {
            if (c) c->need_resched = 1;                          /* выбор потока - после EOI */
        } else if (id == SGI_HALT) {
            if (c) { c->halt_rip = frame[31]; c->halt_rbp = frame[29]; __atomic_store_n(&c->halted, 1, __ATOMIC_RELEASE); }
            gic_eoi(iar);
            for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");   /* авария на другом ядре */
        }
        gic_eoi(iar);
        if (c) c->irq_depth--;
        if (kind == 9) proc_return_check();                     /* прервана программа, процесс которой завершается */
        if (sched_up && c && c->sched_on) sched_irq_exit();     /* вытеснение - после EOI, как на x86 */
        return;
    }
    if (kind == 8) { user_exception(frame); return; }           /* синхронное из EL0: вызов или ошибка программы */
    if ((kind == 4 || kind == 0) && uaccess_fixup(frame, frame[31])) return;   /* сбой внутри копирования из программы */
    uint64_t esr = frame[33], elr = frame[31], far = frame[34];
    unsigned ec = (unsigned)(esr >> 26) & 0x3F;
    const char* why = 0;
    if (kind == 0x10) {                                          /* вход заметил переполнение стека: мы на запасном */
        mmu_on = 0;
        kprintf("\n[KIRA ARM64] Kernel thread stack overflow: guard area hit at SP %x (PC %x), now on the spare stack of core %u\n",
                this_cpu()->ovf_sp, elr, (uint64_t)my_cpu());
        why = "kernel thread stack overflow (the thread's stack is unusable)";
    } else if (kind == 0 || kind == 4 || kind == 3 || kind == 7) {
        why = corefault_arm(kind, ec, elr, far);                      /* возврат - изолировать нельзя */
    }
    if (why && sched_up) smp_halt_others();                      /* авария всей системы, как на x86-64 */
    mmu_on = 0;                                                  /* авария: печатать без замка */
    if (why) kprintf("[KIRA ARM64] Not isolated: %s\n", why);
    if (vmm_kstack_guard_hit(far)) kprintf("\n[KIRA ARM64] kernel thread stack overflow (guard area hit)\n");
    kprintf("\n[KIRA ARM64] exception on core %u: %s (class %x, vector %u), ESR %x\n", (uint64_t)my_cpu(), ec_name(ec), (uint64_t)ec, kind, esr);
    kprintf("[KIRA ARM64] ELR %x  FAR %x  SPSR %x  LR %x  SP %x\n", elr, far, frame[32], frame[30], (uint64_t)(uintptr_t)(frame + 36));
    kprintf("[KIRA ARM64] Halting this core.\n");
    for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");
}

/* ---------------- PSCI ---------------- */
static int psci_hvc = -1;                                        /* -1 - нет PSCI */
static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
    if (psci_hvc) __asm__ volatile ("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else __asm__ volatile ("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

void arm_secondary_main(void) {
    mmu_enable_this_cpu();
    unsigned me = my_cpu();
    percpu_install(&percpu_table[me]);
    (void)uaccess_enable_smap();                                 /* PAN на каждом ядре */
    cpus[me].gic_ok = gic_cpu_init() == 0;
    cpus[me].gic_mask = gic_mask_me;
    if (sched_attach_cpu() != 0) kprintf("[KIRA ARM64] core %u: no thread slot for its idle thread\n", (uint64_t)me);
    timer_start();
    __asm__ volatile ("msr daifclr, #2");
    /* первый тик своего таймера - до объявления «в строю»: потом простаивающее ядро тик выключает */
    uint64_t w0 = now_cnt();
    while (!cpus[me].ticks && now_cnt() - w0 < cnt_freq / 10) __asm__ volatile ("wfi");
    __atomic_store_n(&cpus[me].online, 1, __ATOMIC_RELEASE);
    for (;;) {                                                   /* это ядро теперь - его холостой поток */
        smp_idle_tick_off();
        __asm__ volatile ("wfi");
    }
}

/* ---------------- консоль ---------------- */
static int word(const char* l, const char* w) { while (*w && *l == *w) { l++; w++; } return !*w && (!*l || *l == ' '); }
unsigned platform_cpus(void) { return ncpu; }
const char* platform_irq_name(void) { return gic_v3 ? "GICv3" : "GICv2"; }

/* Часы реального времени PL031 (QEMU virt и многие платы): регистр данных - секунды UTC от 1970. */
static void rtc_pl031(const void* fdt) {
    int n = fdt_find_compatible(fdt, -1, "arm,pl031");
    uint64_t base, size;
    if (n < 0 || fdt_reg(fdt, n, 0, &base, &size)) { kprintf("[KIRA RTC] No PL031 real-time clock: date and time are not shown.\n"); return; }
    volatile void* r = vmm_map_mmio(base, size ? size : 4096, 0);
    if (r) rtc_set_epoch((int64_t)mmio_r32((uint64_t)(uintptr_t)r), "PL031, UTC");
}

/* ---------------- общая консоль: то, что есть только у ARM64 ---------------- */
unsigned smp_online(void) { unsigned n = 0; for (unsigned i = 0; i < ncpu; i++) if (cpus[i].online) n++; return n; }
void smp_describe(void (*out)(const char*)) {
    for (unsigned i = 0; i < ncpu; i++) {
        char b[96]; int k = 0;
        const char* parts[8]; char n1[24], n2[24], n3[24];
        int j = 23; uint64_t v = i; n1[j] = 0; do { n1[--j] = (char)('0' + v % 10); v /= 10; } while (v);
        parts[0] = "  core "; parts[1] = n1 + j;
        static const char hx[] = "0123456789ABCDEF";
        n2[0] = '0'; n2[1] = 'x'; for (int q = 0; q < 16; q++) n2[2 + q] = hx[(cpus[i].mpidr >> (60 - 4 * q)) & 15]; n2[18] = 0;
        parts[2] = ": MPIDR "; parts[3] = n2; parts[4] = cpus[i].online ? ", online, ticks " : ", OFF, ticks ";
        j = 23; v = cpus[i].ticks; n3[j] = 0; do { n3[--j] = (char)('0' + v % 10); v /= 10; } while (v);
        parts[5] = n3 + j; parts[6] = "\n"; parts[7] = 0;
        for (int p = 0; parts[p]; p++) for (const char* c = parts[p]; *c && k + 1 < (int)sizeof b; c++) b[k++] = *c;
        b[k] = 0; out(b);
    }
}
int smp_selftest_start(void) { selftest_arm_start(); return (int)smp_online(); }
/* Проверка изоляции (test crash): поток намеренно ломается. 0 - ошибка кода ядра (гибнет поток), 2 - то же под
   спин-замком (изолировать нельзя: ожидаемая авария), 3 - переполнение стека потока (запасной стек ядра).
   1 - отказ ядра: на x86 это вектор машинной проверки, на ARM - SError, вызвать который программно этот процессор не
   умеет (нет FEAT_RAS с вводом ошибок), поэтому режим честно недоступен (-5). */
static spinlock_t crash_lock = SPINLOCK_INIT;
static volatile uint64_t* const crash_addr = (volatile uint64_t*)0x0000600000000000ull;   /* не отображено */
static uint64_t crash_recurse(uint64_t n) { volatile uint8_t pad[512]; pad[0] = (uint8_t)n; if (n > (1u << 30)) return 0; return crash_recurse(n + 1) + pad[0]; }
static void crash_worker(void* arg) {
    unsigned mode = (unsigned)(uintptr_t)arg;
    if (mode == 2) { (void)spin_lock_irqsave(&crash_lock); *crash_addr = 1; }
    else if (mode == 3) (void)crash_recurse(0);
    else *crash_addr = 1;
    for (;;) sched_yield();
}
int smp_crash_test(unsigned mode, unsigned cpu) {
    (void)cpu;
    if (mode == 1) return -5;
    return sched_create_on("smpcrash", crash_worker, (void*)(uintptr_t)mode, PRIO_NORMAL, SCHED_AFFINITY_ALL) ? 0 : -2;   /* обычный поток, не служебный */
}

void platform_status(void (*out)(const char*)) {
    out("interrupts: "); out(platform_irq_name()); out(v2m ? " + GICv2m (MSI)" : ""); out(", generic timer (virtual), PSCI\n");
}
void platform_hw(void (*out)(const char*)) {
    uint64_t midr = SYSREG_R(midr_el1), isar0 = SYSREG_R(id_aa64isar0_el1), pfr0 = SYSREG_R(id_aa64pfr0_el1), mmfr0 = SYSREG_R(id_aa64mmfr0_el1);
    kprintf("[KIRA HW] CPU MIDR %x (implementer %x, part %x), cores %u, RAM %u MB\n", midr, (midr >> 24) & 0xFF, (midr >> 4) & 0xFFF,
            (uint64_t)ncpu, pmm_get_total_memory_kb() / 1024);
    kprintf("[KIRA HW] features: AES %u, SHA2 %u, atomics %u, FP %s, SIMD %s, physical address bits code %u\n",
            (isar0 >> 4) & 15, (isar0 >> 12) & 15, (isar0 >> 20) & 15, ((pfr0 >> 16) & 15) == 15 ? "no" : "yes",
            ((pfr0 >> 20) & 15) == 15 ? "no" : "yes", mmfr0 & 15);
    (void)out;
}
/* PSCI: SYSTEM_OFF / SYSTEM_RESET (те же hvc/smc, что для запуска ядер) */
void platform_poweroff(void) {
    kprintf("[KIRA PSCI] powering off (SYSTEM_OFF)\n");
    psci_call(0x84000008ull, 0, 0, 0);
    kprintf("[KIRA PSCI] SYSTEM_OFF returned: firmware refused to power off.\n");
}
void platform_reboot(void) {
    kprintf("[KIRA PSCI] rebooting (SYSTEM_RESET)\n");
    psci_call(0x84000009ull, 0, 0, 0);
    kprintf("[KIRA PSCI] SYSTEM_RESET returned: firmware refused to reset.\n");
}

int platform_console_cmd(const char* l, const char* a) {
    if (word(l, "mem")) {
        for (unsigned i = 0; i < nram; i++) kprintf("  RAM %x .. %x (%u MB)\n", ram[i].base, ram[i].base + ram[i].size, ram[i].size >> 20);
        kprintf("  kernel %x .. %x, early allocations up to %x\n", (uint64_t)(uintptr_t)__kernel_start, (uint64_t)(uintptr_t)__kernel_end, bump);
        heap_stats_t hs; heap_get_stats(&hs);
        kprintf("  pages: %u KB free of %u KB; kernel heap: %u bytes in %u blocks\n", (uint64_t)pmm_get_free_memory_kb(),
                (uint64_t)pmm_get_total_memory_kb(), (uint64_t)hs.used_bytes, (uint64_t)hs.used_blocks);
        return 1;
    }
    if (word(l, "pci")) { pci_print(); return 1; }
    if (word(l, "fault")) {
        kprintf("reading an address outside the map: the exception vectors must report it\n");
        (void)*(volatile uint64_t*)(uintptr_t)0x0000600000000000ull;
        return 1;
    }
    (void)a;
    return 0;
}

/* ---------------- PCI Express из дерева устройств ----------------
   Узел pci-host-ecam-generic: reg - окно конфигурации ECAM, bus-range - шины, ranges - окна адресов для BAR
   (3 ячейки адреса шины: тип пространства в старшей - 1 порты, 2 память 32 бита, 3 память 64 бита). Прошивки нет,
   поэтому адреса BAR назначает ядро. Драйверы (virtio, NVMe, AHCI, xHCI) - те же, что на x86-64. */
static void arm_pci_init(const void* fdt) {
    int n = fdt_find_compatible(fdt, -1, "pci-host-ecam-generic");
    if (n < 0 || !fdt_node_enabled(fdt, n)) {
        kprintf("[KIRA PCI] no enabled PCI Express host bridge in the device tree\n");
        return;
    }

    uint64_t base, size;
    if (fdt_reg(fdt, n, 0, &base, &size) || !size ||
        (base & ((1ull << 20) - 1u)) != 0) {
        kprintf("[KIRA PCI] invalid or unaligned ECAM register window\n");
        return;
    }

    uint32_t len = 0, bus0 = 0, bus1 = 0;
    const uint8_t* br = (const uint8_t*)fdt_prop(fdt, n, "bus-range", &len);
    int has_bus_range = br != 0;
    if (has_bus_range) {
        if (len != 8u) {
            kprintf("[KIRA PCI] malformed ECAM bus-range\n");
            return;
        }
        bus0 = fdt_u32(br);
        bus1 = fdt_u32(br + 4);
    }

    uint8_t first_bus, last_bus;
    if (pci_ecam_bus_range(size, has_bus_range, bus0, bus1,
                           &first_bus, &last_bus) != 0) {
        kprintf("[KIRA PCI] ECAM aperture does not cover its bus-range\n");
        return;
    }

    volatile void* e = vmm_map_mmio(base, size, 0);
    if (!e) {
        kprintf("[KIRA PCI] cannot map ECAM window\n");
        return;
    }
    pci_ecam_config_t ecam_config = {
        .base = e, .size = size,
        .first_bus = first_bus, .last_bus = last_bus
    };
    if (pci_configure_ecam(&ecam_config) != 0) {
        kprintf("[KIRA PCI] rejected ECAM configuration after mapping\n");
        return;
    }

    pci_window_t m32 = { 0, 0, 0, 0 }, m64 = { 0, 0, 0, 0 };
    uint32_t pac = 2, sc = 2, prop_len = 0;
    int par = fdt_parent(fdt, n);
    if (par >= 0) {
        const void* p = fdt_prop(fdt, par, "#address-cells", &prop_len);
        if (p) {
            if (prop_len != 4u) {
                kprintf("[KIRA PCI] malformed parent #address-cells; BAR assignment disabled\n");
                goto enumerate_only;
            }
            pac = fdt_u32(p);
        }
    }
    {
        const void* p = fdt_prop(fdt, n, "#size-cells", &prop_len);
        if (p) {
            if (prop_len != 4u) {
                kprintf("[KIRA PCI] malformed #size-cells; BAR assignment disabled\n");
                goto enumerate_only;
            }
            sc = fdt_u32(p);
        }
    }
    if (pac < 1u || pac > 2u || sc < 1u || sc > 2u) {
        kprintf("[KIRA PCI] unsupported ranges cell widths; BAR assignment disabled\n");
        goto enumerate_only;
    }

    {
        const uint8_t* r = (const uint8_t*)fdt_prop(fdt, n, "ranges", &len);
        uint32_t stride = 4u * (3u + pac + sc);
        if (!r || !len || (len % stride) != 0) {
            kprintf("[KIRA PCI] missing or malformed PCI ranges; BAR assignment disabled\n");
            goto enumerate_only;
        }

        for (uint32_t o = 0; o < len; o += stride) {
            uint32_t hi = fdt_u32(r + o);
            uint64_t pci = ((uint64_t)fdt_u32(r + o + 4) << 32) |
                           fdt_u32(r + o + 8);
            uint64_t cpu = 0, sz = 0;
            for (uint32_t i = 0; i < pac; i++)
                cpu = (cpu << 32) | fdt_u32(r + o + 12 + 4u * i);
            for (uint32_t i = 0; i < sc; i++)
                sz = (sz << 32) | fdt_u32(r + o + 12 + 4u * pac + 4u * i);

            pci_window_t* w = ((hi >> 24) & 3u) == 2u ? &m32 :
                              ((hi >> 24) & 3u) == 3u ? &m64 : 0;
            if (!w || !sz) continue;
            if (pci > UINT64_MAX - sz || cpu > UINT64_MAX - sz ||
                pci > INT64_MAX || cpu > INT64_MAX) {
                kprintf("[KIRA PCI] overflowing or unrepresentable PCI range; BAR assignment disabled\n");
                m32.size = m64.size = 0;
                goto enumerate_only;
            }
            int64_t cpu_off;
            if (cpu >= pci) cpu_off = (int64_t)(cpu - pci);
            else cpu_off = -(int64_t)(pci - cpu);
            if (sz > w->size) {
                w->pci_base = pci;
                w->next = pci;
                w->size = sz;
                w->cpu_off = cpu_off;
            }
        }
    }

    {
        int nd = pci_init();
        int nb = pci_assign(&m32, &m64);
        kprintf("[KIRA PCI] ECAM at %x, buses %u-%u: %u function(s), %u BAR(s) assigned (windows: 32-bit %x, 64-bit %x)\n",
                base, (uint64_t)first_bus, (uint64_t)last_bus, (uint64_t)nd,
                (uint64_t)nb, m32.pci_base, m64.pci_base);
        int iommu = iommu_arm64_init(fdt, n);
        if (iommu < 0)
            kprintf("[KIRA SMMU] initialization rejected (%d); covered PCI bus mastering stays disabled\n", iommu);
    }
    return;

enumerate_only:
    {
        int nd = pci_init();
        kprintf("[KIRA PCI] ECAM at %x, buses %u-%u: %u function(s); BAR assignment disabled\n",
                base, (uint64_t)first_bus, (uint64_t)last_bus, (uint64_t)nd);
        int iommu = iommu_arm64_init(fdt, n);
        if (iommu < 0)
            kprintf("[KIRA SMMU] initialization rejected (%d); covered PCI bus mastering stays disabled\n", iommu);
    }
}

/* ---------------- вход ---------------- */
static const void* find_fdt(uint64_t x0) {
    if (!x0 || (x0 & 7)) return 0;
    probing = 1; probe_failed = 0;
    int ok = *(volatile uint32_t*)(uintptr_t)x0 == 0xEDFE0DD0u;   /* FDT_MAGIC в памяти (большой порядок байт) */
    if (ok && !probe_failed) ok = fdt_tree_valid((const void*)(uintptr_t)x0, 4u * 1024u * 1024u);
    probing = 0;
    return ok && !probe_failed ? (const void*)(uintptr_t)x0 : 0;
}

void arm_main(uint64_t dtb, uint64_t entry_el) {
    /* До разбора дерева устройств данные ядер ещё не выделены, а замки журнала уже считают себя в this_cpu() (TPIDR_EL1).
       Нулевой указатель раньше молча писал в адрес 0x78 (флеш QEMU; на платах с ОЗУ с нуля - в живую память), ловушка NULL
       это показала. Временно ставим данные нулевого ядра; настоящие устанавливаются ниже, когда известен номер ядра. */
    percpu_install(&percpu_table[0]);
#ifdef KIRA_EARLY_UART
    kprintf("[early] arm_main x0=%x EL%u\n", dtb, (SYSREG_R(CurrentEL) >> 2) & 3);
#endif
    const void* fdt = find_fdt(dtb);
    /* QEMU кладёт дерево в начало ОЗУ, когда ядро - не образ Linux; адрес начала ОЗУ - ниже ядра */
    if (!fdt) {
        uint64_t k = (uint64_t)(uintptr_t)__kernel_start;
        for (uint64_t a = k & ~0x3FFFFFFFull; a < k && !fdt; a += 0x200000) fdt = find_fdt(a);
    }
#ifdef KIRA_EARLY_UART
    { uint64_t k = (uint64_t)(uintptr_t)__kernel_start & ~0x3FFFFFFFull;
      kprintf("[early] fdt=%x, word at RAM base %x = %x\n", (uint64_t)(uintptr_t)fdt, k, (uint64_t)*(volatile uint32_t*)(uintptr_t)k); }
#endif
    if (!fdt) for (;;) __asm__ volatile ("wfe");                 /* без дерева нет даже адреса порта */

    int u = fdt_find_compatible(fdt, -1, "arm,pl011");
    uint64_t sz;
#ifdef KIRA_EARLY_UART
    kprintf("[early] pl011 node %x\n", (uint64_t)(int64_t)u);
#endif
    if (u >= 0) fdt_reg(fdt, u, 0, &uart, &sz);
    kprintf("\n[KIRA ARM64] Kira OS on ARM64: device tree at %x (%u bytes), console PL011 at %x\n",
            (uint64_t)(uintptr_t)fdt, (uint64_t)fdt_total_size(fdt), uart);
    kprintf("[KIRA ARM64] entered at EL%u, running at EL%u\n", entry_el & 3, (SYSREG_R(CurrentEL) >> 2) & 3);

    /* Normalize the ARM handoff once: memory banks, framebuffer and bootargs become
       the same versioned BootInfo ABI consumed by the x86 path and the ARM PMM. */
    uint32_t bootinfo_size = 0;
    uint64_t kernel_base = (uint64_t)(uintptr_t)__kernel_start;
    uint64_t kernel_size = (uint64_t)(uintptr_t)__kernel_end - kernel_base;
    if (kbi_from_fdt(fdt, fdt_total_size(fdt), (uint64_t)(uintptr_t)fdt,
                     kernel_base, kernel_size, arm_bootinfo_blob,
                     sizeof arm_bootinfo_blob, &bootinfo_size))
        kernel_panic("cannot normalize ARM device tree into BootInfo");
    const kbi_header_t* bootinfo = (const kbi_header_t*)arm_bootinfo_blob;
    const kbi_mmap_entry_t* boot_map = kbi_mmap(bootinfo);
    uint64_t top = 4ull << 30, total = 0;
    for (uint32_t i = 0; i < bootinfo->mmap_count; i++) {
        if (boot_map[i].type != KBI_MEM_USABLE || nram >= MAX_RAM)
            kernel_panic("invalid or excessive BootInfo RAM map");
        ram[nram].base = boot_map[i].base;
        ram[nram].size = boot_map[i].length;
        nram++;
        uint64_t end = boot_map[i].base + boot_map[i].length;   /* validated by kbi_validate */
        if (end > top) top = end;
        if (boot_map[i].length > UINT64_MAX - total)
            kernel_panic("BootInfo RAM size sum overflows");
        total += boot_map[i].length;
    }
    if (!nram) kernel_panic("BootInfo contains no usable RAM");
    kprintf("[KIRA BOOT] BootInfo v%u: ARM64 Device Tree, %u normalized RAM range(s), %u bytes\n",
            (uint64_t)bootinfo->version, (uint64_t)nram, (uint64_t)bootinfo_size);
    bump = (uint64_t)(uintptr_t)__kernel_end;
    uint64_t fdt_end = (uint64_t)(uintptr_t)fdt + fdt_total_size(fdt);
    if ((uint64_t)(uintptr_t)fdt >= bump && fdt_end > bump) bump = fdt_end;   /* дерево за ядром - не затирать */
    bump_end = bump; bump_start = bump;
    for (unsigned i = 0; i < nram; i++) if (bump >= ram[i].base && bump < ram[i].base + ram[i].size) bump_end = ram[i].base + ram[i].size;
    kprintf("[KIRA ARM64] RAM: %u MB in %u region(s)\n", total >> 20, (uint64_t)nram);

    if (dma_noncoherent(fdt)) dma_pool_choose();
    int bits = mmu_init(top);
    if (bits < 0) { kprintf("[KIRA ARM64] FATAL: cannot build page tables\n"); for (;;) __asm__ volatile ("wfe"); }
    if (dma_lo) kprintf("[KIRA ARM64] DMA: devices are NOT cache-coherent: %u MB pool at %x without CPU cache for device buffers\n", (dma_hi - dma_lo) >> 20, dma_lo);
    else kprintf("[KIRA ARM64] DMA: devices are cache-coherent (dma-coherent in the device tree)\n");
    kprintf("[KIRA ARM64] MMU on: identity map up to %x, %u-bit physical addresses, RAM cached, devices non-executable\n", top, (uint64_t)bits);
    wx_verify(1);

    /* контроллер прерываний */
    int g = fdt_find_compatible(fdt, -1, "arm,gic-v3");
    if (g >= 0) {
        gic_v3 = 1;
        fdt_reg(fdt, g, 0, &gicd, &sz);
        fdt_reg(fdt, g, 1, &gicr_base, &gicr_size);
    } else {
        static const char* const v2[] = { "arm,cortex-a15-gic", "arm,gic-400", "arm,cortex-a9-gic", "arm,cortex-a7-gic" };
        for (unsigned i = 0; i < 4 && g < 0; i++) g = fdt_find_compatible(fdt, -1, v2[i]);
        if (g < 0) { kprintf("[KIRA ARM64] FATAL: no supported interrupt controller in the device tree\n"); for (;;) __asm__ volatile ("wfe"); }
        fdt_reg(fdt, g, 0, &gicd, &sz);
        fdt_reg(fdt, g, 1, &gicc, &sz);
        int m = fdt_find_compatible(fdt, -1, "arm,gic-v2m-frame");          /* кадр MSI у GICv2 */
        if (m >= 0 && fdt_reg(fdt, m, 0, &v2m, &sz) == 0) {
            uint32_t t = mmio_r32(v2m + 0x8);                                /* MSI_TYPER: первый SPI и их число */
            v2m_spi = (t >> 16) & 0x3FF; v2m_n = t & 0x3FF;
            if (!v2m_n) v2m = 0;
        } else v2m = 0;
    }
    /* таймер: прерывания в порядке secure phys, phys, virt, hyp; берём виртуальный */
    int tn = fdt_find_compatible(fdt, -1, "arm,armv8-timer");
    if (tn < 0) tn = fdt_find_compatible(fdt, -1, "arm,armv7-timer");
    uint32_t ilen = 0;
    const uint8_t* irq = tn >= 0 ? (const uint8_t*)fdt_prop(fdt, tn, "interrupts", &ilen) : 0;
    timer_intid = (irq && ilen >= 36) ? 16 + fdt_u32(irq + 24 + 4) : 27;
    cnt_freq = SYSREG_R(cntfrq_el0);
    tick_cycles = cnt_freq / TIMER_HZ;
    kprintf("[KIRA ARM64] interrupt controller GICv%u: distributor %x, %s %x; generic timer %u Hz, virtual timer INTID %u\n",
            (uint64_t)(gic_v3 ? 3 : 2), gicd, gic_v3 ? "redistributors" : "CPU interface", gic_v3 ? gicr_base : gicc, cnt_freq, (uint64_t)timer_intid);

    /* ядра */
    int cn = fdt_find_path(fdt, "/cpus");
    if (cn >= 0) {
        int d = 0;
        for (int c = fdt_next_node(fdt, cn, &d); c >= 0 && d > 0; c = fdt_next_node(fdt, c, &d)) {
            if (d != 1) continue;
            uint32_t len; const char* t = (const char*)fdt_prop(fdt, c, "device_type", &len);
            uint64_t a;
            if (!t || len < 4 || __builtin_memcmp(t, "cpu", 4) || fdt_reg(fdt, c, 0, &a, 0) || ncpu >= CPU_MAX) continue;
            cpus[ncpu++].mpidr = a;
        }
    }
    if (!ncpu) { cpus[0].mpidr = SYSREG_R(mpidr_el1) & 0xFF00FFFFFFull; ncpu = 1; }
    unsigned me = my_cpu();
    /* данные ядер: номер ядра = позиция в /cpus; загрузочное ставит свои */
    for (unsigned i = 0; i < ncpu; i++) percpu_alloc((uint32_t)cpus[i].mpidr);
    percpu_install(&percpu_table[me]);
    percpu_up = 1;
    boot_cpu = me;
    int ps = fdt_find_compatible(fdt, -1, "arm,psci-1.0");
    if (ps < 0) ps = fdt_find_compatible(fdt, -1, "arm,psci-0.2");
    if (ps < 0) ps = fdt_find_compatible(fdt, -1, "arm,psci");
    if (ps >= 0) {
        uint32_t len; const char* m = (const char*)fdt_prop(fdt, ps, "method", &len);
        if (m && len >= 3) psci_hvc = m[0] == 'h';
    }

    /* менеджер страниц: всё ОЗУ, кроме ядра, дерева устройств и ранних выделений (их граница - после карты) */
    uint64_t lo = ~0ull, hi = 0;
    for (unsigned i = 0; i < nram; i++) {
        if (ram[i].base < lo) lo = ram[i].base;
        if (ram[i].base + ram[i].size > hi) hi = ram[i].base + ram[i].size;
    }
    lo &= ~4095ull;
    void* map = palloc(pmm_arm_map_bytes(lo, hi), 8);
    if (!map) kernel_panic("no memory for the page map");
    /* занято: ядро, дерево устройств, ранние выделения (таблицы MMU, сама карта) */
    pmm_arm_init((const pmm_arm_region_t*)ram, nram, lo, hi, map, (uint64_t)(uintptr_t)__kernel_start, (uint64_t)(uintptr_t)__kernel_end);
    pmm_arm_reserve((uint64_t)(uintptr_t)fdt, (uint64_t)(uintptr_t)fdt + fdt_total_size(fdt));
    pmm_arm_reserve(0, 4096);                                    /* страница 0 не отображена (ловушка NULL) - pmm её не выдаёт */
    pmm_arm_reserve(bump_start, bump);
    /* Firmware memreserve entries are separate from /reserved-memory nodes. */
    for (unsigned i = 0; i < fdt_reserve_count(fdt, 4u * 1024u * 1024u); i++) {
        uint64_t base = 0, size = 0;
        if (fdt_reserve(fdt, 4u * 1024u * 1024u, i, &base, &size) || base > UINT64_MAX - size)
            kernel_panic("invalid FDT memory reservation");
        pmm_arm_reserve(base, base + size);
    }
    if (dma_lo) pmm_arm_reserve(dma_lo, dma_hi);                 /* пул DMA - не для pmm */
    arm_fb_reserve(fdt, pmm_arm_reserve);                        /* кадр simple-framebuffer, /reserved-memory */
    arm_fb_init(fdt);
    bump_end = bump;                                             /* дальше страницы выдаёт только pmm */
    heap_init();
    sched_init(ncpu, total / 1024);
    sched_set_tsc_khz(cnt_freq / 1000);
    kprintf("[KIRA ARM64] memory manager: %u MB free pages; kernel heap and scheduler online (per-core ready queues, %u thread slots)\n",
            (uint64_t)pmm_get_free_memory_kb() / 1024, (uint64_t)sched_thread_capacity());

    gic_dist_init();
    cpus[me].gic_ok = gic_cpu_init() == 0;
    cpus[me].gic_mask = gic_mask_me;
    if (gic_v3 && its_init(fdt) != 0) kprintf("[KIRA GIC] no usable ITS: PCI devices stay on polling\n");
    anchor_cnt = now_cnt();
    sched_up = 1;
    timer_start();
    cpus[me].online = 1;
    __asm__ volatile ("msr daifclr, #2");

    /* проверка тика: 200 мс по счётчику */
    uint64_t t0 = now_cnt(), k0 = cpus[me].ticks;
    while (now_cnt() - t0 < cnt_freq / 5) __asm__ volatile ("wfi");
    uint64_t got = cpus[me].ticks - k0;
    kprintf("[KIRA ARM64] generic timer verified: %u ticks in 200 ms (expected %u)\n", got, (uint64_t)(TIMER_HZ / 5));

    /* остальные ядра через PSCI CPU_ON */
    unsigned started = 1;
    if (psci_hvc >= 0) {
        for (unsigned i = 0; i < ncpu; i++) {
            if (i == me) continue;
            uint8_t* st = (uint8_t*)(uintptr_t)pmm_alloc_contiguous(4);
            if (!st) { kprintf("[KIRA ARM64] no memory for the stack of core %u\n", (uint64_t)i); break; }
            /* ядро пишет в этот стек ещё без MMU (мимо кэша): строки кэша этих страниц - в память и сбросить */
            for (uint64_t x = (uint64_t)(uintptr_t)st; x < (uint64_t)(uintptr_t)st + 16384; x += 64)
                __asm__ volatile ("dc civac, %0" : : "r"(x) : "memory");
            dsb();
            int64_t r = psci_call(0xC4000003ull, cpus[i].mpidr, (uint64_t)(uintptr_t)secondary_entry, (uint64_t)(uintptr_t)(st + 16384));
            if (r != 0) { kprintf("[KIRA ARM64] PSCI CPU_ON for core %u (MPIDR %x) failed: %u\n", (uint64_t)i, cpus[i].mpidr, (uint64_t)-r); continue; }
            uint64_t w = now_cnt();
            while (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE) && now_cnt() - w < cnt_freq) { }
            if (cpus[i].online) started++;
            else kprintf("[KIRA ARM64] core %u did not come up within 1 s\n", (uint64_t)i);
        }
    }
    kprintf("[KIRA ARM64] Processors online: %u of %u (%s)\n", (uint64_t)started, (uint64_t)ncpu,
            psci_hvc < 0 ? "no PSCI in the device tree" : psci_hvc ? "PSCI via hvc" : "PSCI via smc");
    /* каждое ядро тикает своим таймером */
    t0 = now_cnt();
    while (now_cnt() - t0 < cnt_freq / 5) __asm__ volatile ("wfi");
    unsigned ticking = 0;
    for (unsigned i = 0; i < ncpu; i++) if (cpus[i].online && cpus[i].ticks > 0 && cpus[i].gic_ok) ticking++;
    kprintf("[KIRA ARM64] cores with a working timer interrupt: %u of %u (idle cores then switch their tick off)\n", (uint64_t)ticking, (uint64_t)started);
    clock_init(cnt_freq / 1000);
    arm_pci_init(fdt);
    rng_init();              /* генератор случайных чисел: RNDR, virtio-rng, времена прерываний */
    immune_init();
    watchdog_init();
    experience_init();       /* диски (virtio, NVMe, AHCI по PCI) и опыт прошлых жизней */
    fs_init();               /* KiraFS на системном диске */
    life_init();
    proc_init(ncpu, total / 1024);
    kprintf("[KIRA SYSCALL] SVC from EL0: the same calls as on x86-64 (number in x8, arguments x0..x2); PAN %s; user pointers validated.\n",
            uaccess_enable_smap() ? "on (kernel touches programs only via LDTR/STTR)" : "not offered by this CPU (copies still use LDTR/STTR)");
    mind_sys_init();         /* Разум: нормы, модель предвидения, тактика ~40 Гц */
    rtc_pl031(fdt);

    input_init();
    if (display_init(0) != 0) kprintf("[KIRA DISPLAY] No screen (no virtio-gpu, ramfb or simple-framebuffer): serial console only.\n");
    virtio_input_init();
    usb_init();              /* USB: xHCI (клавиатура, мышь, планшет, хабы, флешки) - тот же стек, что у x86-64 */
    if (e1000_probe() != 0) virtio_net_probe();   /* сетевая карта: Intel e1000 или virtio-net */
    net_start();
    hda_probe();
    kprintf("[KIRA ARM64] Ready. Type a command (help: any unknown word).\n");

    console_init();                                              /* общая консоль: те же команды, что у x86-64 */
    for (;;) {                                                   /* поток core: консоль и секундные дела */
        console_poll();
        static uint64_t last_sec, seconds;
        if (timer_get_ticks() - last_sec >= TIMER_HZ) {          /* раз в секунду: Разум, опыт, журнал, сборка стеков */
            last_sec = timer_get_ticks();
            seconds++;
            mind_sys_tick(seconds);
            experience_tick(seconds);
            sched_reap();
        }
        prog_poll();                                             /* запуск, заказанный оболочкой (док, Проводник) */
        kdrv_poll();                                             /* драйверы Ring 3 */
        life_tick();
        watchdog_feed();
        (void)iommu_fault_poll();
        sched_sleep_ticks(1);
    }
}

uint64_t arm_counter_khz(void) { return cnt_freq / 1000; }

/* Данные этого ядра: адрес записи - в TPIDR_EL1 (this_cpu читает его одной командой). */
void percpu_install(percpu_t* p) { arm_spare_stack_install(p); __asm__ volatile ("msr tpidr_el1, %0" : : "r"(p) : "memory"); }
