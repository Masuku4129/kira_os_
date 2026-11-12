#include "pci_host_parse.h"

#include <limits.h>

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int signed_offset(uint64_t cpu, uint64_t pci, int64_t* out) {
    if (cpu >= pci) {
        uint64_t delta = cpu - pci;
        if (delta > (uint64_t)INT64_MAX) return -1;
        *out = (int64_t)delta;
    } else {
        uint64_t delta = pci - cpu;
        if (delta > (uint64_t)INT64_MAX + 1u) return -1;
        *out = delta == (uint64_t)INT64_MAX + 1u
             ? INT64_MIN : -(int64_t)delta;
    }
    return 0;
}

static int add_fits(uint64_t base, uint64_t size) {
    return size && size <= UINT64_MAX - base;
}

static void clear_window(pci_window_t* w) {
    w->pci_base = 0;
    w->size = 0;
    w->next = 0;
    w->cpu_off = 0;
}

int arm_pci_parse_ranges(const uint8_t* ranges, uint32_t len,
                         uint32_t child_address_cells,
                         uint32_t parent_address_cells,
                         uint32_t size_cells,
                         pci_window_t* mem32, pci_window_t* mem64) {
    if (!mem32 || !mem64) return -1;
    clear_window(mem32);
    clear_window(mem64);

    /* PCI bus addresses are exactly three cells: flags + 64-bit address. */
    if (child_address_cells != 3 ||
        parent_address_cells < 1 || parent_address_cells > 2 ||
        size_cells < 1 || size_cells > 2)
        return -1;

    const uint32_t stride = 4u * (child_address_cells +
                                  parent_address_cells + size_cells);
    if ((!ranges && len) || (stride && len % stride)) return -1;

    for (uint32_t off = 0; ranges && off < len; off += stride) {
        uint32_t hi = be32(ranges + off);
        uint64_t pci_base = ((uint64_t)be32(ranges + off + 4) << 32) |
                            be32(ranges + off + 8);
        uint64_t cpu_base = 0, window_size = 0;
        uint32_t pos = off + 12;

        for (uint32_t i = 0; i < parent_address_cells; i++, pos += 4)
            cpu_base = (cpu_base << 32) | be32(ranges + pos);
        for (uint32_t i = 0; i < size_cells; i++, pos += 4)
            window_size = (window_size << 32) | be32(ranges + pos);

        if (!window_size) continue;
        if (!add_fits(pci_base, window_size) ||
            !add_fits(cpu_base, window_size))
            return -1;

        pci_window_t* target = ((hi >> 24) & 3u) == 2u ? mem32 :
                               ((hi >> 24) & 3u) == 3u ? mem64 : 0;
        if (!target || window_size <= target->size) continue;

        int64_t offset;
        if (signed_offset(cpu_base, pci_base, &offset)) return -1;
        target->pci_base = pci_base;
        target->next = pci_base;
        target->size = window_size;
        target->cpu_off = offset;
    }
    return 0;
}
