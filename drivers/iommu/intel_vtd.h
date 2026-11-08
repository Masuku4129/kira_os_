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
    /* Virtual address of a physical page that this driver allocated (page-table
       and context pages). When absent, map_mmio is used for that translation. */
    void *(*phys_to_virt)(void *context, uint64_t physical);
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
    uint32_t next_domain_id;        /* 0 is reserved for pass-through contexts */
    uint32_t fault_offset;          /* fault recording registers (CAP.FRO * 16) */
    uint32_t fault_records;         /* CAP.NFR + 1 */
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

/* One hardware fault record (VT-d spec 10.4.14), decoded. */
struct kira_vtd_fault {
    uint16_t source_id;             /* bus << 8 | device << 3 | function */
    uint8_t reason;                 /* FR field */
    uint8_t is_read;
    uint64_t address;               /* faulting DMA address (page granularity) */
};
typedef void (*kira_vtd_fault_fn)(void *context, const struct kira_vtd_fault *fault);

const char *kira_vtd_fault_reason(uint8_t reason);

int kira_vtd_invalidate_iotlb(struct kira_vtd_unit *unit,
                              const struct kira_vtd_platform *platform);

/* Global context-cache and IOTLB invalidation through the register interface
   (queued invalidation is never enabled by this driver). */
int kira_vtd_invalidate_all(struct kira_vtd_unit *unit,
                            const struct kira_vtd_platform *platform);

/* Drain the fault recording registers: every valid record is passed to fn and
   cleared. Returns the number of records consumed, or a negative result code.
   Never blocks and never panics; safe to call with translation disabled. */
int kira_vtd_fault_poll(struct kira_vtd_unit *unit, kira_vtd_fault_fn fn, void *context);

int kira_vtd_unit_prepare(struct kira_vtd_unit *unit,
                          uint64_t register_base,
                          uint32_t register_bytes,
                          uint8_t host_address_width,
                          const struct kira_vtd_platform *platform);
int kira_vtd_unit_enable_translation(struct kira_vtd_unit *unit,
                                     const struct kira_vtd_platform *platform);
int kira_vtd_unit_disable_translation(struct kira_vtd_unit *unit,
                                      const struct kira_vtd_platform *platform);

#endif
