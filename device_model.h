#ifndef KIRA_DEVICE_MODEL_H
#define KIRA_DEVICE_MODEL_H

#include <stdint.h>

#define DEVICE_MAX_DEVICES 1024

typedef struct {
    uint64_t id;
    uint64_t generation;
} device_handle_t;

typedef enum {
    DEVICE_BUS_NONE = 0,
    DEVICE_BUS_PCI = 1,
    DEVICE_BUS_USB = 2,
    DEVICE_BUS_VIRTIO = 3,
    DEVICE_BUS_PLATFORM = 4,
    DEVICE_BUS_ACPI = 5
} device_bus_t;

typedef enum {
    DEVICE_STATE_DISCOVERED = 1,
    DEVICE_STATE_VALIDATED = 2,
    DEVICE_STATE_QUARANTINED = 3,
    DEVICE_STATE_FAILED = 4
} device_state_t;

/* A bus-neutral location. PCI uses segment:bus:device:function; other buses
   use a nonzero bus-local key. Location is descriptive, never authority. */
typedef struct {
    uint64_t key;
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} device_location_t;

typedef struct {
    device_handle_t handle;
    device_handle_t parent;
    device_bus_t bus;
    device_state_t state;
    device_location_t location;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
} device_descriptor_t;

enum {
    DEVICE_OK = 0,
    DEVICE_E_INVALID = -1,
    DEVICE_E_FULL = -2,
    DEVICE_E_DUPLICATE = -3,
    DEVICE_E_STALE = -4,
    DEVICE_E_OVERFLOW = -5,
    DEVICE_E_STATE = -6
};

/* Discovery mutation is boot-time only and single-writer. After enumeration,
   descriptors are immutable and safe for concurrent read-only resolution.
   A new snapshot invalidates every handle from the previous snapshot. */
int device_registry_begin_snapshot(void);
int device_registry_add(const device_descriptor_t* candidate, device_handle_t* out);
void device_registry_abort_snapshot(void);
int device_registry_count(void);
uint64_t device_registry_generation(void);
const device_descriptor_t* device_registry_get(int index);
const device_descriptor_t* device_resolve(device_handle_t handle);
/* Lifecycle updates are compare-and-transition operations. Callers must not
   treat a successful state change as proof that hardware resources are safe. */
int device_registry_transition(device_handle_t handle, device_state_t expected,
                               device_state_t next);

#endif
