#include "pci_host.h"
#include "fdt.h"
#include "pci.h"
#include "vmm.h"

/*
 * AArch64 PCI host bridge backend.
 * This owns FDT ECAM discovery, CPU MMIO mapping, PCI address windows and BAR
 * assignment. Boot orchestration decides when to call it.
 */
void arm_pci_init(const void* fdt, arm_pci_log_fn log) {
    int n = fdt_find_compatible(fdt, -1, "pci-host-ecam-generic");
    if (n < 0) {
        if (log) log("[KIRA PCI] no PCI Express host bridge in the device tree\n");
        return;
    }

    uint64_t base, size;
    if (fdt_reg(fdt, n, 0, &base, &size)) return;

    uint32_t len, bus0 = 0, bus1 = (uint32_t)(size >> 20) - 1;
    const uint8_t* br = (const uint8_t*)fdt_prop(fdt, n, "bus-range", &len);
    if (br && len >= 8) { bus0 = fdt_u32(br); bus1 = fdt_u32(br + 4); }
    if (bus1 > 255) bus1 = 255;

    volatile void* ecam = vmm_map_mmio(base, size, 0);
    if (!ecam) return;
    pci_use_ecam(ecam, (uint8_t)bus0, (uint8_t)bus1);

    pci_window_t mem32 = { 0, 0, 0, 0 }, mem64 = { 0, 0, 0, 0 };
    uint32_t address_cells = 2, size_cells = 2;
    int parent = fdt_parent(fdt, n);
    if (parent >= 0) fdt_prop_u32(fdt, parent, "#address-cells", &address_cells);
    fdt_prop_u32(fdt, n, "#size-cells", &size_cells);

    const uint8_t* ranges = (const uint8_t*)fdt_prop(fdt, n, "ranges", &len);
    uint32_t stride = 4 * (3 + address_cells + size_cells);
    for (uint32_t off = 0; ranges && stride && off + stride <= len; off += stride) {
        uint32_t hi = fdt_u32(ranges + off);
        uint64_t pci_base = (uint64_t)fdt_u32(ranges + off + 4) << 32 |
                            fdt_u32(ranges + off + 8);
        uint64_t cpu_base = 0, window_size = 0;
        for (uint32_t i = 0; i < address_cells; i++)
            cpu_base = cpu_base << 32 | fdt_u32(ranges + off + 12 + 4 * i);
        for (uint32_t i = 0; i < size_cells; i++)
            window_size = window_size << 32 |
                          fdt_u32(ranges + off + 12 + 4 * address_cells + 4 * i);

        pci_window_t* window = ((hi >> 24) & 3) == 2 ? &mem32 :
                               ((hi >> 24) & 3) == 3 ? &mem64 : 0;
        if (window && window_size > window->size) {
            window->pci_base = pci_base;
            window->next = pci_base;
            window->size = window_size;
            window->cpu_off = (int64_t)(cpu_base - pci_base);
        }
    }

    int devices = pci_init();
    int bars = pci_assign(&mem32, &mem64);
    if (log) log("[KIRA PCI] ECAM at %x, buses %u-%u: %u function(s), %u BAR(s) assigned (windows: 32-bit %x, 64-bit %x)\n",
                 base, (uint64_t)bus0, (uint64_t)bus1, (uint64_t)devices, (uint64_t)bars,
                 mem32.pci_base, mem64.pci_base);
}
