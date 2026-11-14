#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

typedef struct {
    uint8_t bus, dev, fn;
    uint32_t cfg[16];
} model_device_t;

static model_device_t devices[8];
static unsigned device_count;
static unsigned out_of_range_reads;

static model_device_t* find_device(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].bus == bus && devices[i].dev == dev && devices[i].fn == fn)
            return &devices[i];
    return 0;
}

static uint32_t model_read(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (bus < 5 || bus > 7) out_of_range_reads++;
    model_device_t* d = find_device(bus, dev, fn);
    return d ? d->cfg[(off & 0xFCu) >> 2] : 0xFFFFFFFFu;
}

static void model_write(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    model_device_t* d = find_device(bus, dev, fn);
    if (d) d->cfg[(off & 0xFCu) >> 2] = value;
}

static model_device_t* add_device(uint8_t bus, uint8_t dev, uint16_t vendor,
                                  uint16_t product, uint32_t class_code,
                                  uint8_t header_type) {
    if (device_count >= sizeof devices / sizeof devices[0]) return 0;
    model_device_t* d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    d->bus = bus; d->dev = dev;
    d->cfg[0] = ((uint32_t)product << 16) | vendor;
    d->cfg[2] = class_code;
    d->cfg[3] = (uint32_t)header_type << 16;
    return d;
}

static void set_bridge_buses(model_device_t* d, uint8_t primary,
                             uint8_t secondary, uint8_t subordinate) {
    d->cfg[6] = (uint32_t)primary | ((uint32_t)secondary << 8) |
                ((uint32_t)subordinate << 16);
}

static unsigned ecam_offset(uint8_t bus, uint8_t bus_start, uint8_t dev,
                            uint8_t fn, uint16_t off) {
    return ((unsigned)(bus - bus_start) << 20) |
           ((unsigned)dev << 15) | ((unsigned)fn << 12) | (off & 0xFFCu);
}

static void ecam_put32(uint8_t* base, uint8_t bus_start, uint8_t bus,
                       uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    *(uint32_t*)(void*)(base + ecam_offset(bus, bus_start, dev, fn, off)) = value;
}

static void ecam_device(uint8_t* base, uint8_t bus_start, uint8_t bus,
                        uint8_t dev, uint16_t vendor, uint16_t product,
                        uint32_t class_code, uint8_t header_type) {
    ecam_put32(base, bus_start, bus, dev, 0, 0, ((uint32_t)product << 16) | vendor);
    ecam_put32(base, bus_start, bus, dev, 0, 0x08, class_code);
    ecam_put32(base, bus_start, bus, dev, 0, 0x0C, (uint32_t)header_type << 16);
}

int main(void) {
    pci_backend_t backend = { model_read, model_write };
    pci_set_backend(&backend);

    model_device_t* root = add_device(5, 1, 0x1111, 0x2222, 0x06040000u, 1);
    set_bridge_buses(root, 5, 6, 7);
    add_device(6, 2, 0x1234, 0x5678, 0x02000000u, 0);
    model_device_t* nested = add_device(6, 4, 0x1111, 0x3333, 0x06040000u, 1);
    set_bridge_buses(nested, 6, 7, 7);
    add_device(7, 5, 0x4321, 0x8765, 0x02000000u, 0);
    add_device(9, 3, 0x9999, 0x8888, 0x02000000u, 0); /* outside host range */
    add_device(0, 4, 0x4444, 0x3333, 0x03000000u, 0); /* wrong root bus */

    int count = pci_init_range(5, 7);
    if (count != 4 || pci_count() != 4 ||
        !pci_find(0x1111, 0x2222) || !pci_find(0x1234, 0x5678) ||
        !pci_find(0x1111, 0x3333) || !pci_find(0x4321, 0x8765) ||
        pci_find(0x9999, 0x8888) || pci_find(0x4444, 0x3333) ||
        out_of_range_reads != 0) {
        puts("FAIL: nonzero host root or nested bridge traversal escaped bounds");
        return 1;
    }

    /* Parent subordinate=6: its nested bridge may not expose bus 7. */
    set_bridge_buses(root, 5, 6, 6);
    out_of_range_reads = 0;
    count = pci_init_range(5, 7);
    if (count != 3 || pci_find(0x4321, 0x8765) || out_of_range_reads != 0) {
        puts("FAIL: nested bridge escaped its parent's subordinate window");
        return 1;
    }

    /* Reject backward links before recursion; bus_seen remains the independent cycle guard. */
    set_bridge_buses(root, 5, 6, 7);
    set_bridge_buses(nested, 6, 5, 7);
    count = pci_init_range(5, 7);
    if (count != 3 || pci_find(0x4321, 0x8765) || out_of_range_reads != 0) {
        puts("FAIL: backward bridge link caused a cycle or escaped its bus window");
        return 1;
    }

    /* Secondary bus outside host range and mismatched primary bus must not recurse. */
    set_bridge_buses(root, 5, 9, 9);
    count = pci_init_range(5, 7);
    if (count != 1 || pci_find(0x9999, 0x8888) || out_of_range_reads != 0) {
        puts("FAIL: bridge traversal escaped host bus range");
        return 1;
    }
    set_bridge_buses(root, 4, 6, 7);
    count = pci_init_range(5, 7);
    if (count != 1 || pci_find(0x1234, 0x5678) || out_of_range_reads != 0) {
        puts("FAIL: bridge with wrong primary bus was followed");
        return 1;
    }

    /* Invalid explicit range preserves the last valid PCI snapshot. */
    if (pci_init_range(8, 7) != -1 || pci_count() != 1 ||
        !pci_find(0x1111, 0x2222)) {
        puts("FAIL: invalid host range changed the PCI snapshot");
        return 1;
    }

    /* Legacy port/backend wrapper still begins at bus zero and follows a valid bridge. */
    device_count = 0; out_of_range_reads = 0;
    pci_set_backend(&backend);
    model_device_t* legacy_bridge = add_device(0, 1, 0x8086, 0x1111, 0x06040000u, 1);
    set_bridge_buses(legacy_bridge, 0, 1, 1);
    add_device(0, 4, 0x4444, 0x3333, 0x03000000u, 0);
    add_device(1, 2, 0x1234, 0x5678, 0x02000000u, 0);
    if (pci_init() != 3 || !pci_find(0x4444, 0x3333) ||
        !pci_find(0x1234, 0x5678)) {
        puts("FAIL: legacy pci_init() no longer starts at bus zero");
        return 1;
    }

    /* Exercise the actual ECAM wrapper: pci_init() must start at bus_start=5. */
    _Alignas(4096) static uint8_t ecam[3 * 1024 * 1024];
    memset(ecam, 0xFF, sizeof ecam);
    ecam_device(ecam, 5, 5, 1, 0x1111, 0x2222, 0x06040000u, 1);
    ecam_put32(ecam, 5, 5, 1, 0, 0x18, 5u | (6u << 8) | (6u << 16));
    ecam_device(ecam, 5, 6, 2, 0x1234, 0x5678, 0x02000000u, 0);
    pci_use_ecam(ecam, 5, 7);
    if (pci_init() != 2 || !pci_find(0x1111, 0x2222) ||
        !pci_find(0x1234, 0x5678) || pci_find(0x4444, 0x3333)) {
        puts("FAIL: ECAM pci_init() ignored nonzero bus_start");
        return 1;
    }

    /* Invalid ECAM range is rejected without silently falling back to bus zero. */
    if (pci_count() != 2) return 1;
    pci_use_ecam(ecam, 8, 7);
    if (pci_init() != -1 || pci_count() != 2) {
        puts("FAIL: invalid ECAM host range was accepted or cleared the snapshot");
        return 1;
    }

    puts("PCI host range: nonzero root, nested subordinate bounds, invalid ranges, legacy and ECAM paths passed");
    return 0;
}
