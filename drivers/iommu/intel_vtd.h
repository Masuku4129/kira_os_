#ifndef KIRA_INTEL_VTD_H
#define KIRA_INTEL_VTD_H

#include <stddef.h>
#include <stdint.h>

struct kira_vtd_platform {
    void *context;
    volatile void *(*map_mmio)(void *context, uint64_t physical, size_t length);
    void *(*alloc_dma_page)(void *context, uint64_t *physical);
    void (*free_dma_page)(void *context, void *virtual_address, uint64_t physical);
    void (*write_barrier)(void *context);
    void (*read_barrier)(void *context);
    void (*log)(void *context, const char *message, uint64_t value);
    void (*clean_cache)(void *context, const void *address, size_t length);
};

struct kira_vtd_unit {
    volatile uint8_t *registers;
    uint64_t physical_base;
    uint64_t capabilities;
    uint64_t extended_capabilities;
    uint32_t version;
    uint32_t register_size;
    uint8_t host_address_width;
    uint8_t queued_invalidation;
    uint8_t interrupt_remapping;
    uint8_t translation_enabled;
    void *root_table;
    uint64_t root_table_physical;
};

enum kira_vtd_result {
    KIRA_VTD_OK = 0,
    KIRA_VTD_INVALID = -1,
    KIRA_VTD_MMIO_FAILED = -2,
    KIRA_VTD_UNSUPPORTED = -3,
    KIRA_VTD_TIMEOUT = -4,
    KIRA_VTD_MEMORY_FAILED = -5,
    KIRA_VTD_HARDWARE_ERROR = -6
};

int kira_vtd_unit_prepare(struct kira_vtd_unit *unit,
                          uint64_t register_base,
                          uint8_t host_address_width,
                          const struct kira_vtd_platform *platform);
int kira_vtd_unit_enable_translation(struct kira_vtd_unit *unit,
                                     const struct kira_vtd_platform *platform);
int kira_vtd_unit_disable_translation(struct kira_vtd_unit *unit,
                                      const struct kira_vtd_platform *platform);

#endif
