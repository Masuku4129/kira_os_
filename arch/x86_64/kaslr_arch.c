// KASLR x86-64: самоперемещение образа ядра при загрузке. Подробно (альтернативы, почему так) - docs/architecture/KASLR_WX.md.
//
// Порядок: boot.s входит в long mode на ранней карте с W^X (образ "O" по адресу 1 МБ), затем вызывает kaslr_setup ДО любого
// другого кода ядра: тот выбирает 2-МБ позицию в свободной ОЗУ первого гигабайта, копирует в неё файловую часть образа
// ("N"), прибавляет сдвиг ко всем словам из таблицы перемещений (.kreloc), обнуляет bss копии и строит для неё свою карту
// страниц (earlymap.c; в ней страница-"мост" со старого места исполняема). Дальше boot.s через мост переключает CR3 и стек
// и продолжает в N; kaslr_finish закрывает мост и стирает старый образ.
// Здесь нельзя печатать и трогать данные ядра: percpu и UART ещё не готовы; результат записывается в kaslr_state
// (и в копию), а в журнал его выводит kaslr_log из kernel_main.
#include <stdint.h>
#include "kaslr.h"
#include "kaslr_arch.h"
#include "earlymap.h"
#include "wx.h"
#include "bootinfo.h"
#include "kptr.h"

extern char kernel_start[], kernel_end[], kernel_loaded_end[], kreloc_start[], kreloc_end[];
extern char wx_ro_start[], wx_ro_end[], wx_text_start[], wx_text_end[], wx_data_start[], wx_data_end[];
extern char kaslr_bounce[];
extern uint64_t pml4_table[], pdpt_table[], pd_table[], early_pt_pool[];

#define TRAMP_BASE 0x8000ULL                    // smp.h: SMP_TRAMP_BASE (здесь smp.h не нужен)
#define MB2_MAGIC  0x36D76289UL

kaslr_state_t kaslr_state;
static kaslr_mem_t kaslr_mem;                    // в bss: на раннем 16-КБ стеке 6 КБ лишние

const kaslr_state_t* kaslr_info(void) { return &kaslr_state; }

static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint64_t rdtsc_(void) { uint32_t lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static inline void cpuid_(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static int hw_random(int seed, uint64_t* out) {
    for (int i = 0; i < 32; i++) {              // команды могут временно отказать: Intel советует до 10 повторов
        uint64_t v; unsigned char ok;
        if (seed) __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        else      __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
        __asm__ volatile ("pause");
    }
    return 0;
}

// Энтропия: RDSEED и RDRAND, если процессор их даёт (проверяется CPUID), плюс всегда отсчёты TSC и счётчика PIT.
// Без аппаратного генератора остаются только отсчёты времени (слабее, но не нуль): это честно пишется в журнал.
static uint64_t collect_entropy(uint32_t* src) {
    uint64_t acc = 0, v;
    uint32_t a, b, c, d;
    *src = 0;
    cpuid_(0, 0, &a, &b, &c, &d);
    uint32_t maxleaf = a;
    if (maxleaf >= 7) {
        cpuid_(7, 0, &a, &b, &c, &d);
        if (b & (1u << 18))
            for (int i = 0; i < 2; i++) if (hw_random(1, &v)) { acc = kaslr_mix(acc, v); *src |= KASLR_SRC_RDSEED; }
    }
    if (maxleaf >= 1) {
        cpuid_(1, 0, &a, &b, &c, &d);
        if (c & (1u << 30))
            for (int i = 0; i < 2; i++) if (hw_random(0, &v)) { acc = kaslr_mix(acc, v); *src |= KASLR_SRC_RDRAND; }
    }
    for (int i = 0; i < 24; i++) {              // PIT: канал 0 защёлкивается командой 0x00, читаются младший и старший байты
        outb(0x43, 0x00);
        uint32_t lo = inb(0x40), hi = inb(0x40);
        acc = kaslr_mix(acc, rdtsc_() ^ ((uint64_t)((hi << 8) | lo) << 40) ^ (uint64_t)i);
        for (volatile int k = 0; k < 40 + (int)(acc & 31); k++) __asm__ volatile ("pause");   // разброс между отсчётами
    }
    *src |= KASLR_SRC_TSC_PIT;
    return acc ? acc : 0x4B49524Bu;             // нуль не нужен никому: выбор позиции работает и без него, но нуль - признак сбоя
}

static uint64_t* ident(uint64_t pa) { return (uint64_t*)(uintptr_t)pa; }
static const wx_env_t ident_env = { ident, 0 };

static void fail(int reason) { kaslr_state.reason = reason; }

uint64_t kaslr_setup(uint64_t info, uint32_t size, uint32_t uefi, uint32_t magic) {
    kaslr_state_t* st = &kaslr_state;
    st->link_base = st->base = (uint64_t)(uintptr_t)kernel_start;
    st->slide = 0; st->slots = 0; st->entropy_src = 0; st->relocs = 0;
    st->reason = KASLR_NO_ENV;

    uint64_t image = (uint64_t)(uintptr_t)(kernel_end - kernel_start);          // с bss
    uint64_t loaded = (uint64_t)(uintptr_t)(kernel_loaded_end - kernel_start);  // файловая часть без таблицы
    uint64_t tbl_len = (uint64_t)(uintptr_t)(kreloc_end - kreloc_start);
    if (tbl_len < sizeof(kreloc_hdr_t)) { fail(KASLR_NO_TABLE); return 0; }
    if (kreloc_check(kreloc_start, (uint32_t)tbl_len, (uint32_t)loaded) != 0) { fail(KASLR_BAD_TABLE); return 0; }
    if (((const kreloc_hdr_t*)kreloc_start)->n64 + ((const kreloc_hdr_t*)kreloc_start)->n32 == 0) { fail(KASLR_NO_TABLE); return 0; }

    const void* blob = 0; uint32_t blob_size = 0;
    uint64_t xlo = 0, xhi = 0;
    if (uefi) {
        blob = (const void*)(uintptr_t)info; blob_size = size;
    } else {
        if (magic != MB2_MAGIC || !info || info >= KASLR_CEIL || (info & 7)) { fail(KASLR_BAD_BOOTINFO); return 0; }
        uint32_t total = *(const uint32_t*)(uintptr_t)info;
        if (total < 16 || total > 65536 || info + total > KASLR_CEIL) { fail(KASLR_BAD_BOOTINFO); return 0; }
        if (kbi_from_multiboot2((const void*)(uintptr_t)info, total, st->link_base, image, &blob, &blob_size) != 0) {
            fail(KASLR_BAD_BOOTINFO); return 0;
        }
        xlo = info; xhi = info + total;
    }
    if (kbi_validate(blob, blob_size) != 0) { fail(KASLR_BAD_BOOTINFO); return 0; }
    const kbi_header_t* h = (const kbi_header_t*)blob;
    if ((h->flags & KBI_F_CMDLINE) && h->cmdline_len && h->cmdline_off < blob_size &&
        h->cmdline_len <= blob_size - h->cmdline_off &&
        kaslr_cmdline_has((const char*)blob + h->cmdline_off, h->cmdline_len, "kira.nokaslr")) {
        fail(KASLR_OFF_CMDLINE); return 0;
    }
    if (kaslr_collect(blob, blob_size, xlo, xhi, &kaslr_mem) != 0) { fail(KASLR_BAD_BOOTINFO); return 0; }

    // За образом нужен запас: PMM кладёт свои битовые карты в первую свободную область выше kernel_end.
    uint64_t pmm_store = (kaslr_mem.ram_top >> 14) + (1ULL << 20);               // 2 бита на страницу = ram_top / 16 КБ
    kaslr_env_t env = { kaslr_mem.usable, kaslr_mem.n_usable, kaslr_mem.avoid, kaslr_mem.n_avoid,
                        image + pmm_store, KASLR_FLOOR, KASLR_CEIL };
    uint32_t src = 0;
    uint64_t entropy = collect_entropy(&src);
    st->entropy_src = src;
    uint64_t base = 0, slots = 0;
    if (kaslr_pick(&env, entropy, &base, &slots) != 0 || base <= st->link_base) { st->slots = slots; fail(KASLR_NO_SLOT); return 0; }
    uint64_t slide = base - st->link_base;

    // Копия: файловая часть образа, bss обнулён (его состояние до этого момента ядру не нужно: boot.s переносит
    // переменные входа сам).
    uint8_t* dst = (uint8_t*)(uintptr_t)base;
    for (uint64_t i = 0; i < loaded; i++) dst[i] = ((const uint8_t*)kernel_start)[i];
    for (uint64_t i = loaded; i < image; i++) dst[i] = 0;
    int64_t n = kreloc_apply(dst, (uint32_t)loaded, kreloc_start, (uint32_t)tbl_len, slide);
    if (n < 0) {                                // копия испорчена (до перехода ничего не использовалось) - остаёмся на месте
        for (uint64_t i = 0; i < image; i++) dst[i] = 0;
        fail(KASLR_BAD_TABLE); return 0;
    }

    // Карта страниц копии: секции N по правам, трамплин AP, мост (страница со старого места).
    earlymap_layout_t l;
    l.w.text_lo = (uint64_t)(uintptr_t)wx_text_start + slide; l.w.text_hi = (uint64_t)(uintptr_t)wx_text_end + slide;
    l.w.ro_lo = (uint64_t)(uintptr_t)wx_ro_start + slide;     l.w.ro_hi = (uint64_t)(uintptr_t)wx_ro_end + slide;
    l.w.data_lo = (uint64_t)(uintptr_t)wx_data_start + slide; l.w.data_hi = (uint64_t)(uintptr_t)wx_data_end + slide;
    l.w.exec_lo = TRAMP_BASE; l.w.exec_hi = TRAMP_BASE + 4096;
    l.image_lo = base; l.image_hi = base + ((image + 4095) & ~4095ULL);
    l.exec2_lo = (uint64_t)(uintptr_t)kaslr_bounce; l.exec2_hi = l.exec2_lo + 4096;
    earlymap_pa_t pa;
    pa.pml4 = (uint64_t)(uintptr_t)pml4_table + slide;
    pa.pdpt = (uint64_t)(uintptr_t)pdpt_table + slide;
    pa.pd = (uint64_t)(uintptr_t)pd_table + slide;
    pa.pt_pool = (uint64_t)(uintptr_t)early_pt_pool + slide;
    pa.pt_pool_n = EARLYMAP_PT_MAX;
    if (earlymap_build(&ident_env, &pa, &l) != 0) {
        for (uint64_t i = 0; i < image; i++) dst[i] = 0;
        fail(KASLR_NO_SLOT); return 0;
    }

    st->base = base; st->slide = slide; st->slots = slots; st->reason = KASLR_OK; st->relocs = (uint64_t)n;
    kaslr_state_t* ns = (kaslr_state_t*)((uintptr_t)&kaslr_state + slide);
    *ns = *st;
    return slide;
}

// Первая функция копии: мост (страница старого образа) больше не нужен.
void kaslr_finish(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t old_bridge = (uint64_t)(uintptr_t)kaslr_bounce - kaslr_state.slide;
    uint64_t old_base = kaslr_state.link_base;
    if (earlymap_make_data(&ident_env, cr3 & ~0xFFFULL, old_bridge) != 0) {
        kaslr_state.reason = KASLR_BAD_TABLE;   // мост остался исполняемым: kaslr_log скажет об этом
        return;
    }
    __asm__ volatile ("invlpg (%0)" : : "r"(old_bridge) : "memory");
    // Старый образ - неперемещённая копия кода по известному адресу: стираем (его память потом отдаёт PMM).
    volatile uint8_t* o = (volatile uint8_t*)(uintptr_t)old_base;
    uint64_t len = (uint64_t)(uintptr_t)(kernel_end - kernel_start);
    for (uint64_t i = 0; i < len; i++) o[i] = 0;
}

void kaslr_log(void) {
    extern void uart_print(const char*);
    extern void uart_print_hex(uint64_t);
    extern void uart_print_dec(uint64_t);
    const kaslr_state_t* s = &kaslr_state;
    if (s->slide == 0) {
        uart_print("[KIRA KASLR] off: "); uart_print(kaslr_reason(s->reason));
        uart_print(". The kernel stays at its link address (no address randomization on this boot).\n");
        return;
    }
    uint64_t bits = 0; for (uint64_t n = s->slots; n > 1; n >>= 1) bits++;
    uart_print("[KIRA KASLR] on: kernel image moved to a random 2 MB slot (1 of "); uart_print_dec(s->slots);
    uart_print(" free slots, ~"); uart_print_dec(bits); uart_print(" bits), "); uart_print_dec(s->relocs);
    uart_print(" words relocated; entropy: ");
    if (s->entropy_src & KASLR_SRC_RDSEED) uart_print("rdseed+");
    if (s->entropy_src & KASLR_SRC_RDRAND) uart_print("rdrand+");
    uart_print("tsc/pit");
    if (!(s->entropy_src & (KASLR_SRC_RDSEED | KASLR_SRC_RDRAND))) uart_print(" only (this CPU has no RDSEED/RDRAND: weaker randomness)");
    uart_print(".\n");
    if (s->reason != KASLR_OK) uart_print("[KIRA KASLR] WARNING: the old bridge page could not be closed.\n");
    if (kptr_debug()) {
        uart_print("[KIRA KASLR] kernel base "); uart_print_hex(s->base);
        uart_print(" (link base "); uart_print_hex(s->link_base); uart_print(", slide "); uart_print_hex(s->slide); uart_print(").\n");
    } else {
        uart_print("[KIRA KASLR] kernel base hidden by the kptr policy (kira.debug on the kernel command line or 'kptr debug on' shows it).\n");
    }
}
