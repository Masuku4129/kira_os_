#ifndef KIRA_IOMMU_H
#define KIRA_IOMMU_H
#include <stdint.h>
#include "pci.h"

/* PCI DMA isolation backends: Intel VT-d on x86-64 and ARM SMMUv3 on AArch64.
   Drivers must create a per-device domain before bus mastering and explicitly map
   every DMA buffer. A discovered ARM SMMU is a fail-closed gate: failed setup or
   missing iommu-map coverage must not fall back to unrestricted DMA. */

#define IOMMU_R 1u
#define IOMMU_W 2u

enum {
    IOMMU_OK = 0,
    IOMMU_E_NONE = -1,      /* no IOMMU is present/active on this platform */
    IOMMU_E_ARG = -2,
    IOMMU_E_NOCOVER = -3,   /* no DMAR unit translates this device */
    IOMMU_E_RMRR = -4,      /* firmware needs this device to reach a reserved region */
    IOMMU_E_NOMEM = -5,
    IOMMU_E_HW = -6,        /* hardware refused or timed out */
    IOMMU_E_EXIST = -7,     /* overlaps an existing mapping / table full */
    IOMMU_E_NOENT = -8,     /* no such domain or mapping */
    IOMMU_E_RANGE = -9      /* address beyond what the hardware or the domain can express */
};

#if defined(__x86_64__) || defined(__aarch64__)
/* Parse DMAR, build contexts, enable translation. Returns the number of units now
   translating (0: no change in behaviour), negative on a rejected table. Call once,
   after pci_init() and before any driver starts DMA. */
int iommu_init(void);
#if defined(__aarch64__)
/* Discover and initialize the SMMUv3 instance and PCI iommu-map from the validated DTB. */
int iommu_init_fdt(const void *fdt);
#endif
int iommu_enabled(void);

/* Switch the device to its own translated domain with NO mappings. 0 on success
   (also when it already is isolated), else an IOMMU_E_* code and the device stays
   exactly as it was. */
int iommu_isolate_device(const pci_dev_t* dev);
/* Back to pass-through (unrestricted DMA); frees the domain. For a device whose
   driver found the isolation not to be in effect. */
int iommu_release_device(const pci_dev_t* dev);
/* 1 only when the device is in a translated domain of an active unit. */
int iommu_device_isolated(const pci_dev_t* dev);

/* Let the device DMA to [phys, phys+len) at device address iova. iova and phys must
   have the same offset inside a page; the pages covering the range are mapped. A
   range overlapping an earlier mapping is refused. */
int iommu_map(const pci_dev_t* dev, uint64_t iova, uint64_t phys, uint64_t len, unsigned rw);
/* Remove exactly one earlier iommu_map (same set of pages). The IOTLB is
   invalidated before it returns, so the memory may be reused afterwards. */
int iommu_unmap(const pci_dev_t* dev, uint64_t iova, uint64_t len);
void iommu_unmap_all(const pci_dev_t* dev);

/* Collect hardware fault records, log each as one line. Returns the number consumed. */
int iommu_fault_poll(void);
/* Faults attributed to the device since boot (needs iommu_fault_poll to run). */
uint32_t iommu_fault_count(const pci_dev_t* dev);

#else
/* Architectures without a wired IOMMU backend report "not available". */
static inline int iommu_init(void)
{
    return 0;
}
static inline int iommu_enabled(void)
{
    return 0;
}
static inline int iommu_isolate_device(const pci_dev_t* dev)
{
    (void)dev;
    return IOMMU_E_NONE;
}
static inline int iommu_release_device(const pci_dev_t* dev)
{
    (void)dev;
    return IOMMU_E_NONE;
}
static inline int iommu_device_isolated(const pci_dev_t* dev)
{
    (void)dev;
    return 0;
}
static inline int iommu_map(const pci_dev_t* dev, uint64_t iova, uint64_t phys, uint64_t len, unsigned rw)
{
    (void)dev; (void)iova; (void)phys; (void)len; (void)rw;
    return IOMMU_E_NONE;
}
static inline int iommu_unmap(const pci_dev_t* dev, uint64_t iova, uint64_t len)
{
    (void)dev; (void)iova; (void)len;
    return IOMMU_E_NONE;
}
static inline void iommu_unmap_all(const pci_dev_t* dev)
{
    (void)dev;
}
static inline int iommu_fault_poll(void)
{
    return 0;
}
static inline uint32_t iommu_fault_count(const pci_dev_t* dev)
{
    (void)dev;
    return 0;
}
#endif

#endif
