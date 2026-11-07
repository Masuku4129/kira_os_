#ifndef KIRA_BOOT_HANDOFF_H
#define KIRA_BOOT_HANDOFF_H
#include <stddef.h>
#include <stdint.h>
#include "kira/bootinfo.h"

#define KIRA_BOOT_MAX_REGIONS 256u
enum kira_memory_kind { KIRA_MEM_USABLE=1, KIRA_MEM_RESERVED=2, KIRA_MEM_ACPI=3, KIRA_MEM_NVS=4, KIRA_MEM_BAD=5 };
struct kira_memory_region { uint64_t base, length; uint32_t kind, attributes; };
struct kira_boot_handoff {
 const struct kira_bootinfo *raw;
 struct kira_memory_region regions[KIRA_BOOT_MAX_REGIONS];
 size_t region_count;
 uint64_t usable_bytes;
 uint8_t has_framebuffer;
 uint8_t has_rsdp;
};
struct kira_boot_map_ops {
 void *context;
 int (*decode_descriptor)(void *context, const void *descriptor,
                          uint32_t version, struct kira_memory_region *out);
};
int kira_boot_handoff_init(struct kira_boot_handoff *out,
                           const struct kira_bootinfo *info,
                           uint64_t available_size,
                           const struct kira_boot_map_ops *ops);
#endif
