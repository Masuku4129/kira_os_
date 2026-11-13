#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "device_model.h"

static device_descriptor_t pci_candidate(uint8_t bus, uint8_t dev, uint8_t fn) {
    device_descriptor_t d;
    memset(&d, 0, sizeof d);
    d.bus = DEVICE_BUS_PCI;
    d.location.segment = 0;
    d.location.bus = bus;
    d.location.device = dev;
    d.location.function = fn;
    d.vendor_id = 0x1234;
    d.product_id = 0x5678;
    d.class_code = 0x02;
    d.subclass = 0x00;
    return d;
}

int main(void) {
    device_handle_t first = {0, 0};
    device_handle_t child = {0, 0};
    device_descriptor_t d;

    assert(device_registry_begin_snapshot() == DEVICE_OK);
    d = pci_candidate(0, 3, 1);
    assert(device_registry_add(&d, &first) == DEVICE_OK);
    assert(first.id != 0 && first.generation != 0);
    assert(device_registry_count() == 1);
    const device_descriptor_t* found = device_resolve(first);
    assert(found && found->bus == DEVICE_BUS_PCI);
    assert(found->location.bus == 0 && found->location.device == 3 &&
           found->location.function == 1);
    assert(found->vendor_id == 0x1234 && found->product_id == 0x5678);
    assert(found->state == DEVICE_STATE_DISCOVERED);
    assert(device_registry_get(0) == found);
    assert(device_registry_get(-1) == 0 && device_registry_get(1) == 0);

    /* Duplicate bus locations and malformed PCI locations fail closed. */
    assert(device_registry_add(&d, &child) == DEVICE_E_DUPLICATE);
    d.location.device = 32;
    assert(device_registry_add(&d, &child) == DEVICE_E_INVALID);
    d = pci_candidate(0, 4, 0);
    d.vendor_id = 0xFFFF;
    assert(device_registry_add(&d, &child) == DEVICE_E_INVALID);

    /* The registry accepts another bus type without leaking PCI-specific identity. */
    memset(&d, 0, sizeof d);
    d.bus = DEVICE_BUS_USB;
    d.location.key = 0x10001;
    d.parent = first;
    assert(device_registry_add(&d, &child) == DEVICE_OK);
    assert(child.id > first.id && child.generation == first.generation);
    found = device_resolve(child);
    assert(found && found->bus == DEVICE_BUS_USB && found->parent.id == first.id);

    /* A new discovery snapshot invalidates every old handle and never reuses IDs. */
    assert(device_registry_begin_snapshot() == DEVICE_OK);
    assert(device_registry_count() == 0);
    assert(device_resolve(first) == 0 && device_resolve(child) == 0);
    d = pci_candidate(0, 3, 1);
    assert(device_registry_add(&d, &first) == DEVICE_OK);
    assert(first.id > child.id && first.generation > child.generation);
    memset(&d, 0, sizeof d);
    d.bus = DEVICE_BUS_USB;
    d.location.key = 0x20001;
    d.parent = child;
    assert(device_registry_add(&d, &child) == DEVICE_E_STALE);

    /* Capacity is bounded; exhaustion does not overwrite a live descriptor. */
    assert(device_registry_begin_snapshot() == DEVICE_OK);
    for (uint32_t i = 0; i < DEVICE_MAX_DEVICES; i++) {
        memset(&d, 0, sizeof d);
        d.bus = DEVICE_BUS_PLATFORM;
        d.location.key = (uint64_t)i + 1;
        assert(device_registry_add(&d, &first) == DEVICE_OK);
    }
    assert(device_registry_count() == DEVICE_MAX_DEVICES);
    memset(&d, 0, sizeof d);
    d.bus = DEVICE_BUS_PLATFORM;
    d.location.key = (uint64_t)DEVICE_MAX_DEVICES + 1;
    assert(device_registry_add(&d, &child) == DEVICE_E_FULL);
    assert(device_registry_count() == DEVICE_MAX_DEVICES);
    assert(device_resolve(first) != 0);

    device_registry_abort_snapshot();
    assert(device_registry_count() == 0);
    assert(device_resolve(first) == 0);
    puts("Device Model: typed bus identity, bounded registry, duplicate rejection, generation invalidation and fail-closed capacity passed");
    return 0;
}
