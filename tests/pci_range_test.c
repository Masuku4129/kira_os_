#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci.h"

typedef struct {
    uint8_t bus, dev, fn;
    uint32_t cfg[16];
} model_device_t;

static model_device_t devices[4];
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

    bridge->cfg[6] = 5u | (6u << 8) | (7u << 16);
    out_of_range_reads = 0;
    count = pci_init_range(5, 7);
    const pci_dev_t* endpoint = pci_find(0x1234, 0x5678);
    if (count != 2 || pci_count() != 2 || !endpoint ||
        endpoint->bus != 6 || pci_find(0x9999, 0x8888) ||
        out_of_range_reads != 0) {
        puts("FAIL: nonzero root bus or in-range bridge traversal failed");
        return 1;
    }

    if (pci_init_range(8, 7) != -1 || pci_count() != 0) {
        puts("FAIL: inverted bus range was not rejected");
        return 1;
    }

    out_of_range_reads = 0;
    count = pci_init();
    if (count != 1 || !pci_find(0x4444, 0x3333) ||
        pci_find(0x1111, 0x2222) || out_of_range_reads != 0) {
        puts("FAIL: legacy pci_init() no longer starts at bus zero");
        return 1;
    }

    puts("PCI range enumeration: nonzero root, bridge bounds, invalid range and legacy wrapper passed");
    return 0;
}
