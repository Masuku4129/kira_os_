/*
 * Convert the KIRA BIOS stage-2 handoff into the common kernel boot ABI.
 * E820 entries are consumed by the early physical-memory code; their original
 * table remains referenced through memory.address with descriptor_version=0.
 */
#include "bootinfo.h"
#define KIRA_BIOS_HANDOFF_MAGIC 0x3149424bu
#define E820_USABLE 1u
#define E820_RESERVED 2u
#define E820_ACPI_RECLAIM 3u
#define E820_ACPI_NVS 4u
#define E820_BAD 5u
int kira_bios_convert_handoff(const struct kira_bios_handoff_v1 *src,
 uint64_t available,const struct kira_bios_e820_entry *map,size_t count,
 struct kira_bootinfo *out)
{
 if(!src||!out||available<sizeof(*src)||src->magic!=KIRA_BIOS_HANDOFF_MAGIC||
    src->version!=1||src->size<sizeof(*src)||src->size>available||
    !map||!count||count>KIRA_BIOS_E820_MAX)return -1;
 out->magic=KIRA_BOOTINFO_MAGIC;out->version=KIRA_BOOTINFO_VERSION;
 out->structure_size=sizeof(*out);out->boot_source=KIRA_BOOT_BIOS;out->flags=0;
 out->memory.address=(uint64_t)(uintptr_t)map;
 out->memory.byte_size=count*sizeof(*map);out->memory.descriptor_size=sizeof(*map);
 out->memory.descriptor_version=0;
 out->framebuffer.address=src->framebuffer_address;
 out->framebuffer.byte_size=src->framebuffer_size;
 out->framebuffer.width=src->framebuffer_width;
 out->framebuffer.height=src->framebuffer_height;
 out->framebuffer.pixels_per_scanline=src->framebuffer_pitch;
 out->framebuffer.format=src->framebuffer_format;
 out->rsdp_address=src->rsdp_address;out->kernel_physical_base=src->kernel_base;
 out->kernel_image_size=src->kernel_size;out->initrd_physical_base=src->initrd_base;
 out->initrd_size=src->initrd_size;out->command_line_physical=src->command_line_address;
 out->command_line_length=src->command_line_length;out->reserved=0;
 return 0;
}
