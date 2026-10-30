#include "mbinfo.h"
#include "bootmap.h"
#include "uart.h"

#define MB2_MAGIC 0x36D76289UL
#define ONE_GB    (1ULL << 30)

static uint8_t info_copy[MBINFO_MAX] __attribute__((aligned(8)));

unsigned long mbinfo_relocate(unsigned long magic, unsigned long addr) {
    if (magic != MB2_MAGIC || addr == 0 || (addr & 7)) return addr;

    if (addr + 8 <= ONE_GB) {
        uint32_t size = *(const uint32_t*)addr;
        if (size >= 16 && addr + size <= ONE_GB) return addr;   // уже внутри identity-карты
        if (size < 16 || size > MBINFO_MAX) return addr;         // повреждена: пусть отвергнет PMM
    }

    if (bootmap_init() != 0) {
        uart_print("[KIRA BOOT] Multiboot2 info is above the 1 GB identity map and the early window cannot be installed.\n");
        return addr;
    }
    const uint8_t* head = (const uint8_t*)bootmap_map(addr, 8, 0);
    if (!head) {
        uart_print("[KIRA BOOT] Cannot map the Multiboot2 info header.\n");
        return addr;
    }
    uint32_t size = *(const uint32_t*)head;
    if (size < 16 || size > MBINFO_MAX) {
        uart_print("[KIRA BOOT] Multiboot2 info above 1 GB has an invalid size: ");
        uart_print_dec(size);
        uart_print(" bytes.\n");
        return addr;
    }
    const uint8_t* src = (const uint8_t*)bootmap_map(addr, size, 0);
    if (!src) {
        uart_print("[KIRA BOOT] Cannot map the Multiboot2 info body.\n");
        return addr;
    }
    for (uint32_t i = 0; i < size; i++) info_copy[i] = src[i];
    uart_print("[KIRA BOOT] Multiboot2 info at ");
    uart_print_hex(addr);
    uart_print(" is above the 1 GB identity map: copied ");
    uart_print_dec(size);
    uart_print(" bytes into the kernel image.\n");
    return (unsigned long)(uintptr_t)info_copy;
}

int mbinfo_cmdline_has(unsigned long mbi, const char* word) {
    const uint32_t* tag = (const uint32_t*)mbinfo_find_tag(mbi, 1);       /* тип 1: командная строка загрузки */
    if (!tag || tag[1] <= 8) return 0;
    const char* s = (const char*)(tag + 2);
    const char* end = (const char*)tag + tag[1];
    unsigned wl = 0; while (word[wl]) wl++;
    for (const char* p = s; p < end && *p; ) {
        while (p < end && *p == ' ') p++;
        const char* b = p;
        while (p < end && *p && *p != ' ') p++;
        if ((unsigned)(p - b) == wl) {
            unsigned k = 0; while (k < wl && b[k] == word[k]) k++;
            if (k == wl) return 1;
        }
    }
    return 0;
}

const void* mbinfo_find_tag(unsigned long mbi, uint32_t type) {
    if (mbi == 0 || (mbi & 7)) return 0;
    uint32_t total = *(const uint32_t*)mbi;
    if (total < 16) return 0;
    const uint8_t* p = (const uint8_t*)mbi + 8;
    const uint8_t* end = (const uint8_t*)mbi + total;
    while (p + 8 <= end) {
        const uint32_t* h = (const uint32_t*)p;
        if (h[0] == 0) return 0;
        if (h[1] < 8 || p + h[1] > end) return 0;
        if (h[0] == type) return p;
        p += (h[1] + 7) & ~7u;
    }
    return 0;
}
