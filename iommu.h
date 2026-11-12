#ifndef KIRA_IOMMU_H
#define KIRA_IOMMU_H
#include <stdint.h>
#include "pci.h"

/* DMA isolation through Intel VT-d (x86-64). Without a usable DMAR table every
   call below reports "not available" and the kernel behaves as before.

   Model: iommu_init() gives EVERY enumerated PCI function a pass-through context
   (unchanged, unrestricted DMA - logged as such) and turns translation on. A
   driver that wants isolation calls iommu_isolate_device() BEFORE the device can
   master the bus; from then on the device reaches only memory granted with
   iommu_map(). Devices that no DMAR unit covers, devices named by an RMRR and
   drivers that never ask stay unrestricted, and iommu_device_isolated() says so. */

#define IOMMU_R 1u
#define IOMMU_W 2u

enum {
    IOMMU_OK = 0,
    IOMMU_E_NONE = -1,      /* VT-d not active (no DMAR, parse error, no usable unit) */
    IOMMU_E_ARG = -2,
    IOMMU_E_NOCOVER = -3,   /* no DMAR unit translates this device */
    IOMMU_E_RMRR = -4,      /* firmware needs this device to reach a reserved region */
    IOMMU_E_NOMEM = -5,
    IOMMU_E_HW = -6,        /* hardware refused or timed out */
    IOMMU_E_EXIST = -7,     /* overlaps an existing mapping / table full */
    IOMMU_E_NOENT = -8,     /* no such domain or mapping */
    IOMMU_E_RANGE = -9      /* address beyond what the hardware or the domain can express */
};

#if defined(__x86_64__)
/* Parse DMAR, build contexts, enable translation. Returns the number of units now
   translating (0: no change in behaviour), negative on a rejected table. Call once,
   after pci_init() and before any driver starts DMA. */
int iommu_init(void);
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
/* Retry every tracked unmap; nonzero means at least one mapping remains pinned. */
int iommu_unmap_all(const pci_dev_t* dev);

/* Collect hardware fault records, log each as one line. Returns the number consumed. */
int iommu_fault_poll(void);
/* Faults attributed to the device since boot (needs iommu_fault_poll to run). */
uint32_t iommu_fault_count(const pci_dev_t* dev);

#else
/* Other architectures have no VT-d: the same calls report "not available" (ARM64 SMMU is separate work). */
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
static inline int iommu_unmap_all(const pci_dev_t* dev)
{
    (void)dev;
    return IOMMU_E_NONE;
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
