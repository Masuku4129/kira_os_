#ifndef KIRA_VTD_H
#define KIRA_VTD_H

#include <stdint.h>
#include "pci.h"

/* Intel VT-d second-level DMA isolation. All addresses passed to map/unmap
 * are physical pages; the device-visible IOVA is identity-mapped for them. */
int vtd_init(void);
int vtd_device_enable(const pci_dev_t *device);
int vtd_device_isolated(const pci_dev_t *device);
int vtd_dma_map(const pci_dev_t *device,uint64_t physical,uint64_t length);
int vtd_dma_unmap(const pci_dev_t *device,uint64_t physical,uint64_t length);
int vtd_interrupt_remapping_enabled(void);

#endif
