#ifndef KIRA_AARCH64_PCI_HOST_H
#define KIRA_AARCH64_PCI_HOST_H

/* Log callback keeps early platform diagnostics on the same serialized console path. */
typedef void (*arm_pci_log_fn)(const char* format, ...);

/* Discover and initialize the FDT-described PCIe ECAM host bridge. */
void arm_pci_init(const void* fdt, arm_pci_log_fn log);

#endif
