#include "device_model.h"

static device_descriptor_t devices[DEVICE_MAX_DEVICES];
static uint32_t device_count;
static uint64_t snapshot_generation;
static uint64_t next_device_id = 1;
static int snapshot_active;

static int handle_is_zero(device_handle_t h) {
    return h.id == 0 && h.generation == 0;
}

static int bus_supported(device_bus_t bus) {
    return bus >= DEVICE_BUS_PCI && bus <= DEVICE_BUS_ACPI;
}

static int same_location(const device_descriptor_t* a, const device_descriptor_t* b) {
    if (a->bus != b->bus) return 0;
    if (a->bus == DEVICE_BUS_PCI) {
        return a->location.segment == b->location.segment &&
               a->location.bus == b->location.bus &&
               a->location.device == b->location.device &&
               a->location.function == b->location.function;
    }
    return a->location.key == b->location.key;
}

int device_registry_begin_snapshot(void) {
    if (snapshot_generation == UINT64_MAX || !next_device_id) return DEVICE_E_OVERFLOW;
    snapshot_generation++;
    device_count = 0;
    snapshot_active = 1;
    return DEVICE_OK;
}

int device_registry_add(const device_descriptor_t* candidate, device_handle_t* out) {
    if (out) { out->id = 0; out->generation = 0; }
    if (!snapshot_active || !candidate || !out || !bus_supported(candidate->bus))
        return DEVICE_E_INVALID;
    if (candidate->bus == DEVICE_BUS_PCI) {
        if (candidate->location.device >= 32 || candidate->location.function >= 8 ||
            candidate->vendor_id == 0xFFFFu)
            return DEVICE_E_INVALID;
    } else if (!candidate->location.key) {
        return DEVICE_E_INVALID;
    }
    if (!handle_is_zero(candidate->parent) && !device_resolve(candidate->parent))
        return DEVICE_E_STALE;
    if (device_count >= DEVICE_MAX_DEVICES) return DEVICE_E_FULL;
    for (uint32_t i = 0; i < device_count; i++)
        if (same_location(&devices[i], candidate)) return DEVICE_E_DUPLICATE;
    if (!next_device_id) return DEVICE_E_OVERFLOW;

    device_descriptor_t* dst = &devices[device_count];
    *dst = *candidate;
    dst->handle.id = next_device_id++;
    dst->handle.generation = snapshot_generation;
    dst->state = DEVICE_STATE_DISCOVERED;
    device_count++;
    *out = dst->handle;
    return DEVICE_OK;
}

void device_registry_abort_snapshot(void) {
    device_count = 0;
    snapshot_active = 0;
}

int device_registry_count(void) {
    return snapshot_active ? (int)device_count : 0;
}

uint64_t device_registry_generation(void) {
    return snapshot_active ? snapshot_generation : 0;
}

const device_descriptor_t* device_registry_get(int index) {
    if (!snapshot_active || index < 0 || (uint32_t)index >= device_count) return 0;
    return &devices[index];
}

const device_descriptor_t* device_resolve(device_handle_t handle) {
    if (!snapshot_active || !handle.id || !handle.generation ||
        handle.generation != snapshot_generation) return 0;
    for (uint32_t i = 0; i < device_count; i++)
        if (devices[i].handle.id == handle.id &&
            devices[i].handle.generation == handle.generation)
            return &devices[i];
    return 0;
}
