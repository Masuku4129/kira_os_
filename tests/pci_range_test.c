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

static int allow_rescan = 1;
static int model_rescan_guard(void) { return allow_rescan; }

static model_device_t* find_device(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].bus == bus && devices[i].dev == dev && devices[i].fn == fn)
            return &devices[i];
    return 0;
}

static uint32_t model_read(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (bus < 5 || bus > 7) out_of_range_reads++;
    model_device_t* d = find_device(bus, dev, fn);
    return d ? d->cfg[(off & 0xFC) >> 2] : 0xFFFFFFFFu;
}

static void model_write(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t value) {
    model_device_t* d = find_device(bus, dev, fn);
    if (d) d->cfg[(off & 0xFC) >> 2] = value;
}

static model_device_t* add_device(uint8_t bus, uint8_t dev, uint16_t vendor,
                                  uint16_t product, uint32_t class_code,
                                  uint8_t header_type) {
    model_device_t* d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    d->bus = bus; d->dev = dev;
    d->cfg[0] = ((uint32_t)product << 16) | vendor;
    d->cfg[2] = class_code;
    d->cfg[3] = (uint32_t)header_type << 16;
    return d;
}

int main(void) {
    pci_backend_t backend = { model_read, model_write };
    pci_set_backend(&backend);
    pci_set_rescan_guard(model_rescan_guard);

    model_device_t* bridge = add_device(5, 1, 0x1111, 0x2222, 0x06040000u, 1);
    bridge->cfg[6] = 5u | (9u << 8) | (9u << 16); /* secondary bus is outside the host range */
    add_device(6, 2, 0x1234, 0x5678, 0x02000000u, 0);
    add_device(9, 3, 0x9999, 0x8888, 0x02000000u, 0);
    add_device(0, 4, 0x4444, 0x3333, 0x03000000u, 0);

    int count = pci_init_range(5, 7);
    if (count != 1 || pci_count() != 1 ||
        !pci_find(0x1111, 0x2222) || pci_find(0x1234, 0x5678) ||
        pci_find(0x9999, 0x8888) || out_of_range_reads != 0) {
        puts("FAIL: bridge traversal escaped the declared host bus range");
        return 1;
    }

    pci_handle_t first_handle = pci_get_handle(0);
    if (!first_handle.id || !first_handle.generation ||
        pci_resolve(first_handle) != pci_get(0)) {
        puts("FAIL: current PCI handle did not resolve");
        return 1;
    }

    bridge->cfg[6] = 5u | (6u << 8) | (7u << 16);
    model_device_t* nested = add_device(6, 4, 0x1111, 0x3333, 0x06040000u, 1);
    nested->cfg[6] = 6u | (7u << 8) | (7u << 16);
    add_device(7, 5, 0x4321, 0x8765, 0x02000000u, 0);
    out_of_range_reads = 0;
    count = pci_init_range(5, 7);
    const pci_dev_t* endpoint = pci_find(0x1234, 0x5678);
    const pci_dev_t* nested_endpoint = pci_find(0x4321, 0x8765);
    if (count != 4 || pci_count() != 4 || !endpoint || endpoint->bus != 6 ||
        !nested_endpoint || nested_endpoint->bus != 7 ||
        pci_find(0x9999, 0x8888) || out_of_range_reads != 0) {
        puts("FAIL: valid nested bridge traversal inside subordinate windows failed");
        return 1;
    }

    pci_handle_t second_handle = pci_get_handle(0);
    if (pci_resolve(first_handle) || pci_resolve(second_handle) != pci_get(0) ||
        second_handle.id <= first_handle.id ||
        second_handle.generation <= first_handle.generation) {
        puts("FAIL: rescan did not invalidate the old generation-safe handle");
        return 1;
    }

    /* The parent bridge now limits descendants to bus 6; its child must not expose bus 7. */
    bridge->cfg[6] = 5u | (6u << 8) | (6u << 16);
    out_of_range_reads = 0;
    count = pci_init_range(5, 7);
    const pci_dev_t* third_handle_device = pci_get(0);
    pci_handle_t third_handle = pci_get_handle(0);
    if (count != 3 || pci_find(0x4321, 0x8765) || out_of_range_reads != 0 ||
        pci_resolve(second_handle) || pci_resolve(third_handle) != third_handle_device) {
        puts("FAIL: nested bridge escaped its parent's subordinate bus window");
        return 1;
    }

    allow_rescan = 0;
    if (pci_init_range(5, 7) != -1 || pci_count() != 3 ||
        pci_resolve(third_handle) != pci_get(0)) {
        puts("FAIL: denied rescan invalidated the current PCI snapshot");
        return 1;
    }
    allow_rescan = 1;

    if (pci_init_range(8, 7) != -1 || pci_count() != 0 || pci_resolve(second_handle)) {
        puts("FAIL: inverted bus range was not rejected");
        return 1;
    }

    out_of_range_reads = 0;
    count = pci_init();
    if (count != 1 || !pci_find(0x4444, 0x3333) ||
        pci_find(0x1111, 0x2222)) {
        puts("FAIL: legacy pci_init() no longer starts at bus zero");
        return 1;
    }

    puts("PCI range enumeration: host and nested bridge windows, stale handles, rescan guard and legacy wrapper passed");
    return 0;
}
