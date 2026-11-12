#ifndef KIRA_IOMMU_H
#define KIRA_IOMMU_H
#include <stdint.h>
#include "pci.h"

/* Architecture-neutral PCI DMA isolation API.
   x86-64 uses Intel VT-d; ARM64 uses SMMUv3. Each supported IOMMU starts with
   explicit pass-through contexts for mapped requesters. A driver that requests
   isolation receives a private translated domain with no mappings; DMA is
   enabled only after the driver grants each queue and data buffer with
   iommu_map(). Uncovered or deliberately pass-through devices remain explicitly
   unrestricted and iommu_device_isolated() reports that state. */

#define IOMMU_R 1u
#define IOMMU_W 2u

enum {
    IOMMU_OK = 0,
    IOMMU_E_NONE = -1,      /* no active IOMMU for this platform/device */
    IOMMU_E_ARG = -2,
    IOMMU_E_NOCOVER = -3,   /* no IOMMU mapping covers this requester */
    IOMMU_E_RMRR = -4,      /* firmware needs this device to reach a reserved region */
    IOMMU_E_NOMEM = -5,
    IOMMU_E_HW = -6,        /* hardware refused or timed out */
    IOMMU_E_EXIST = -7,     /* overlaps an existing mapping / table full */
    IOMMU_E_NOENT = -8,     /* no such domain or mapping */
    IOMMU_E_RANGE = -9      /* address beyond what the hardware or the domain can express */
};

#if defined(__aarch64__)
/* ARM64 SMMUv3 is initialized from the PCI host's FDT iommu-map before any
   PCI driver starts. Mapped devices begin with explicit per-stream bypass
   STEs; these bypass domains are not DMA-safe for bus mastering. PCI bus
   mastering is denied until a driver replaces the bypass with a private stage-1
   domain and grants the required queue/data mappings. */
int iommu_arm64_init(const void* fdt, int pci_host_node);
int iommu_init(void);
int iommu_enabled(void);
int iommu_device_ready(const pci_dev_t* dev);
int iommu_isolate_device(const pci_dev_t* dev);
int iommu_release_device(const pci_dev_t* dev);
int iommu_device_isolated(const pci_dev_t* dev);
int iommu_map(const pci_dev_t* dev, uint64_t iova, uint64_t phys,
              uint64_t len, unsigned rw);
int iommu_unmap(const pci_dev_t* dev, uint64_t iova, uint64_t len);
int iommu_unmap_all(const pci_dev_t* dev);
int iommu_fault_poll(void);
uint32_t iommu_fault_count(const pci_dev_t* dev);

#elif defined(__x86_64__)
/* Parse DMAR, build contexts, enable translation. Returns the number of units now
   translating (0: no change in behaviour), negative on a rejected table. Call once,
   after pci_init() and before any driver starts DMA. */
int iommu_init(void);
int iommu_enabled(void);
/* Covered requesters may bus-master only after a live private domain exists. */
int iommu_device_ready(const pci_dev_t* dev);

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
int iommu_unmap_all(const pci_dev_t* dev);

/* Collect hardware fault records, log each as one line. Returns the number consumed. */
int iommu_fault_poll(void);
/* Faults attributed to the device since boot (needs iommu_fault_poll to run). */
uint32_t iommu_fault_count(const pci_dev_t* dev);

#else
/* Other architectures have no hardware IOMMU implementation. */
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
    return IOMMU_OK;
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
