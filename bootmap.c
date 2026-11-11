#include "bootmap.h"
#include "cpu.h"

#define P_PRESENT 1ULL
#define P_WRITE   2ULL
#define P_PCD     0x10ULL       // с PAT по умолчанию (PWT=0, PCD=1) - индекс 2: UC-
#define P_HUGE    0x80ULL
#define P_NX      (1ULL << 63)
#define MSR_EFER  0xC0000080u
#define EFER_NXE  (1ULL << 11)
#define PHYS_LIMIT (1ULL << 52)

#ifdef KIRA_HOST_TEST
extern uint64_t bootmap_test_cr3;
extern int bootmap_test_nxe;
extern unsigned bootmap_test_flushes;
static uint64_t rd_cr3(void) { return bootmap_test_cr3; }
static int nxe_on(void) { return bootmap_test_nxe; }
static void tlb_flush(void) { bootmap_test_flushes++; }
#else
static uint64_t rd_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}
static int nxe_on(void) { return (cpu_rdmsr(MSR_EFER) & EFER_NXE) != 0; }
static void tlb_flush(void) {
    uint64_t v = rd_cr3();
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}
#endif

static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[512] __attribute__((aligned(4096)));
static uint64_t slot_phys[BOOTMAP_SLOTS];
static uint8_t slot_used[BOOTMAP_SLOTS];
static uint8_t slot_uc[BOOTMAP_SLOTS];
static int ready;

int bootmap_init(void) {
    if (ready) return 0;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(rd_cr3() & ~0xFFFULL);
    if (pml4[511] & P_PRESENT) return -1;
    for (unsigned i = 0; i < 512; i++) { pdpt[i] = 0; pd[i] = 0; }
    for (unsigned i = 0; i < BOOTMAP_SLOTS; i++) { slot_used[i] = 0; slot_uc[i] = 0; slot_phys[i] = 0; }
    pdpt[0] = (uint64_t)(uintptr_t)pd | P_PRESENT | P_WRITE;
    pml4[511] = (uint64_t)(uintptr_t)pdpt | P_PRESENT | P_WRITE;
    ready = 1;
    return 0;
}

void* bootmap_map(uint64_t phys, uint64_t len, int uncached) {
    if (!ready || len == 0 || phys + len < phys) return 0;
    uint64_t first = phys & ~(BOOTMAP_SLOT - 1);
    uint64_t last = (phys + len - 1) & ~(BOOTMAP_SLOT - 1);
    if (last >= PHYS_LIMIT) return 0;
    /* The bootstrap identity map covers the first GiB as WB. Never create a UC
       alias for any part of that range before the normal MMIO mapper takes over. */
    if (uncached && first < (1ULL << 30)) return 0;
    uint64_t n64 = (last - first) / BOOTMAP_SLOT + 1;
    if (n64 > BOOTMAP_SLOTS) return 0;
    unsigned n = (unsigned)n64;
    unsigned uc = uncached ? 1 : 0;
    int start = -1;

    /* A physical 2 MiB page must never be aliased with different cache types.
       Such WB/UC aliases are architecturally unsafe even when virtual addresses differ. */
    for (unsigned i = 0; i < BOOTMAP_SLOTS; i++) {
        if (slot_used[i] && slot_uc[i] != uc &&
            slot_phys[i] >= first && slot_phys[i] <= last) return 0;
    }

    // Тот же диапазон уже отображён: используем повторно (адрес не меняется между вызовами).
    for (unsigned s = 0; s + n <= BOOTMAP_SLOTS && start < 0; s++) {
        int same = 1;
        for (unsigned k = 0; k < n; k++) {
            if (!slot_used[s + k] || slot_phys[s + k] != first + (uint64_t)k * BOOTMAP_SLOT || slot_uc[s + k] != uc) { same = 0; break; }
        }
        if (same) start = (int)s;
    }
    if (start < 0) {
        for (unsigned s = 0; s + n <= BOOTMAP_SLOTS; s++) {
            int free_run = 1;
            for (unsigned k = 0; k < n; k++) if (slot_used[s + k]) { free_run = 0; break; }
            if (free_run) { start = (int)s; break; }
        }
        if (start < 0) return 0;
        uint64_t nx = nxe_on() ? P_NX : 0;
        for (unsigned k = 0; k < n; k++) {
            uint64_t pa = first + (uint64_t)k * BOOTMAP_SLOT;
            slot_used[start + k] = 1;
            slot_uc[start + k] = (uint8_t)uc;
            slot_phys[start + k] = pa;
            pd[start + k] = pa | P_PRESENT | P_WRITE | P_HUGE | (uc ? P_PCD : 0) | nx;
        }
    }
    return (void*)(uintptr_t)(BOOTMAP_BASE + (uint64_t)start * BOOTMAP_SLOT + (phys - first));
}

void bootmap_release_all(void) {
    if (!ready) return;
    for (unsigned i = 0; i < BOOTMAP_SLOTS; i++) { pd[i] = 0; slot_used[i] = 0; }
    tlb_flush();
}

unsigned bootmap_used_slots(void) {
    unsigned c = 0;
    for (unsigned i = 0; i < BOOTMAP_SLOTS; i++) c += slot_used[i];
    return c;
}
