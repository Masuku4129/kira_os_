#ifndef KIRA_BIOS_BOOTINFO_H
#define KIRA_BIOS_BOOTINFO_H
#include <stdint.h>
#include "kira/bootinfo.h"
#define KIRA_BIOS_E820_MAX 128u
struct kira_bios_e820_entry { uint64_t base,length;uint32_t type,attributes; } __attribute__((packed));
struct kira_bios_handoff_v1 {
 uint32_t magic,version,size;
 uint32_t e820_count,reserved0;
 uint64_t e820_address;
 uint64_t framebuffer_address,framebuffer_size;
 uint32_t framebuffer_width,framebuffer_height,framebuffer_pitch,framebuffer_format;
 uint64_t rsdp_address,kernel_base,kernel_size,initrd_base,initrd_size;
 uint64_t command_line_address;uint32_t command_line_length,reserved1;
} __attribute__((packed));
int kira_bios_convert_handoff(const struct kira_bios_handoff_v1 *,
 uint64_t available,const struct kira_bios_e820_entry *,size_t,
 struct kira_bootinfo *);
#endif
