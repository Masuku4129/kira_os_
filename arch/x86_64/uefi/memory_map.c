/*
 * UEFI memory descriptor conversion to KIRA physical-memory categories.
 * Conventional memory is usable only after ExitBootServices. Boot-services
 * memory is also reclaimed only after the kernel has taken over firmware.
 */
#include "memory_map.h"
#define EFI_RESERVED_MEMORY_TYPE 0u
#define EFI_LOADER_CODE 1u
#define EFI_LOADER_DATA 2u
#define EFI_BOOT_SERVICES_CODE 3u
#define EFI_BOOT_SERVICES_DATA 4u
#define EFI_RUNTIME_SERVICES_CODE 5u
#define EFI_RUNTIME_SERVICES_DATA 6u
#define EFI_CONVENTIONAL_MEMORY 7u
#define EFI_UNUSABLE_MEMORY 8u
#define EFI_ACPI_RECLAIM_MEMORY 9u
#define EFI_ACPI_MEMORY_NVS 10u
#define EFI_MEMORY_MAPPED_IO 11u
#define EFI_MEMORY_MAPPED_IO_PORT 12u
#define EFI_PAL_CODE 13u
#define EFI_PERSISTENT_MEMORY 14u
int kira_efi_decode_memory_descriptor(void *context,const void *descriptor,
 uint32_t version,struct kira_memory_region *out)
{
 const struct kira_efi_memory_descriptor*d=descriptor;uint64_t bytes;
 (void)context;
 if(!d||!out||version>1||!d->number_of_pages||
    d->number_of_pages>UINT64_MAX/4096u)return -1;
 bytes=d->number_of_pages*4096u;
 if(d->physical_start>UINT64_MAX-bytes)return -1;
 out->base=d->physical_start;out->length=bytes;out->attributes=(uint32_t)d->attribute;
 switch(d->type){
 case EFI_CONVENTIONAL_MEMORY:case EFI_PERSISTENT_MEMORY:
  out->kind=KIRA_MEM_USABLE;break;
 case EFI_ACPI_RECLAIM_MEMORY:out->kind=KIRA_MEM_ACPI;break;
 case EFI_ACPI_MEMORY_NVS:out->kind=KIRA_MEM_NVS;break;
 case EFI_UNUSABLE_MEMORY:out->kind=KIRA_MEM_BAD;break;
 case EFI_RESERVED_MEMORY_TYPE:case EFI_LOADER_CODE:case EFI_LOADER_DATA:
 case EFI_BOOT_SERVICES_CODE:case EFI_BOOT_SERVICES_DATA:
 case EFI_RUNTIME_SERVICES_CODE:case EFI_RUNTIME_SERVICES_DATA:
 case EFI_MEMORY_MAPPED_IO:case EFI_MEMORY_MAPPED_IO_PORT:case EFI_PAL_CODE:
 default:out->kind=KIRA_MEM_RESERVED;break;
 }
 return 0;
}
