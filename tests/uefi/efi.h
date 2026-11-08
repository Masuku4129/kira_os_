#ifndef KIRA_TEST_EFI_H
#define KIRA_TEST_EFI_H
#include <stdint.h>
#include <stddef.h>
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef int32_t EFI_STATUS;
typedef struct {
    UINT32 Type;
    UINT32 Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER (-2)
#define EFI_OUT_OF_RESOURCES (-3)
#define EFI_BAD_BUFFER_SIZE (-4)
#define EFI_BUFFER_TOO_SMALL (-5)
#define EFI_LOAD_ERROR (-6)
#define EfiReservedMemoryType 0
#define EfiLoaderCode 1
#define EfiLoaderData 2
#define EfiBootServicesCode 3
#define EfiBootServicesData 4
#define EfiRuntimeServicesCode 5
#define EfiRuntimeServicesData 6
#define EfiConventionalMemory 7
#define EfiUnusableMemory 8
#define EfiACPIReclaimMemory 9
#define EfiACPIMemoryNVS 10
#define EfiMemoryMappedIO 11
#define EfiMemoryMappedIOPortSpace 12
#endif
