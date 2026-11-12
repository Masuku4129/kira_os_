#include "pci_host.h"
#include "pci_host_parse.h"
#include "fdt.h"
#include "pci.h"
#include "vmm.h"

/*
 * AArch64 PCI host bridge backend.
 * This owns FDT ECAM discovery, CPU MMIO mapping, PCI address windows and BAR
 * assignment. Boot orchestration decides when to call it.
 */
void arm_pci_init(const void* fdt, arm_pci_log_fn log) {
    if (!fdt_valid(fdt, 0)) {
        if (log) log("[KIRA PCI] invalid device tree; PCI initialization skipped\n");
        return;
    }

    int n = fdt_find_compatible(fdt, -1, "pci-host-ecam-generic");
    if (n < 0 || !fdt_node_enabled(fdt, n)) {
        if (log) log("[KIRA PCI] no enabled PCI Express host bridge in the device tree\n");
        return;
    }

    uint64_t base, size;
    if (fdt_reg(fdt, n, 0, &base, &size) || !size ||
        base > UINT64_MAX - size || size < (1ULL << 20) ||
        (base & ((1ULL << 20) - 1))) {
        if (log) log("[KIRA PCI] invalid ECAM region; PCI initialization skipped\n");
        return;
    }

    uint32_t len = 0, bus0 = 0, bus1 = 0;
    const uint8_t* br = (const uint8_t*)fdt_prop(fdt, n, "bus-range", &len);
    if (br) {
        if (len != 8) {
            if (log) log("[KIRA PCI] malformed bus-range; PCI initialization skipped\n");
            return;
        }
        bus0 = fdt_u32(br);
        bus1 = fdt_u32(br + 4);
        if (bus0 != 0 || bus0 > bus1 || bus1 > 255) {
            if (log) log("[KIRA PCI] bus-range must start at bus 0 and end within 0..255\n");
            return;
        }
    } else {
        uint64_t bus_count = size >> 20;
        if (!bus_count || bus_count > 256) {
            if (log) log("[KIRA PCI] ECAM size cannot define a valid bus range\n");
            return;
        }
        bus1 = (uint32_t)bus_count - 1;
    }

    uint64_t required_size = ((uint64_t)bus1 - bus0 + 1) << 20;
    if (size < required_size || base > UINT64_MAX - required_size) {
        if (log) log("[KIRA PCI] ECAM region does not cover the declared bus range\n");
        return;
    }

    uint32_t child_address_cells = 3;
    uint32_t parent_address_cells = 2;
    uint32_t size_cells = 2;
    int parent = fdt_parent(fdt, n);
    if (parent < 0 ||
        fdt_prop_u32(fdt, n, "#address-cells", &child_address_cells) ||
        fdt_prop_u32(fdt, parent, "#address-cells", &parent_address_cells) ||
        fdt_prop_u32(fdt, n, "#size-cells", &size_cells)) {
        if (log) log("[KIRA PCI] missing PCI ranges cell geometry\n");
        return;
    }

    pci_window_t mem32 = { 0, 0, 0, 0 }, mem64 = { 0, 0, 0, 0 };
    const uint8_t* ranges = (const uint8_t*)fdt_prop(fdt, n, "ranges", &len);
    if (arm_pci_parse_ranges(ranges, ranges ? len : 0,
                             child_address_cells, parent_address_cells,
                             size_cells, &mem32, &mem64)) {
        if (log) log("[KIRA PCI] malformed or overflowing ranges; PCI initialization skipped\n");
        return;
    }

    volatile void* ecam = vmm_map_mmio(base, required_size, VMM_MMIO_UC);
    if (!ecam) {
        if (log) log("[KIRA PCI] ECAM mapping failed\n");
        return;
    }
    pci_use_ecam(ecam, (uint8_t)bus0, (uint8_t)bus1);

    int devices = pci_init();
    int bars = pci_assign(&mem32, &mem64);
    if (log) log("[KIRA PCI] ECAM at %x, buses %u-%u: %u function(s), %u BAR(s) assigned (windows: 32-bit %x, 64-bit %x)\n",
                 base, (uint64_t)bus0, (uint64_t)bus1, (uint64_t)devices, (uint64_t)bars,
                 mem32.pci_base, mem64.pci_base);
}
