#ifndef KIRA_EFI_MEMORY_H
#define KIRA_EFI_MEMORY_H
#include <stdint.h>
#include "kernel/boot/boot_handoff.h"
struct kira_efi_memory_descriptor {
 uint32_t type,pad;uint64_t physical_start,virtual_start;
 uint64_t number_of_pages,attribute;
};
int kira_efi_decode_memory_descriptor(void *context,const void *descriptor,
 uint32_t descriptor_version,struct kira_memory_region *out);
#endif
