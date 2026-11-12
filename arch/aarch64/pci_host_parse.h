#ifndef KIRA_AARCH64_PCI_HOST_PARSE_H
#define KIRA_AARCH64_PCI_HOST_PARSE_H

#include <stdint.h>
#include "pci.h"

/*
 * Parse the "ranges" property of a PCI host bridge.
 * PCI child addresses always occupy three cells; parent addresses and sizes
 * use the cell counts declared by the corresponding Device Tree buses.
 * Returns 0 for a well-formed property, -1 for malformed/overflowing data.
 */
int arm_pci_parse_ranges(const uint8_t* ranges, uint32_t len,
                         uint32_t child_address_cells,
                         uint32_t parent_address_cells,
                         uint32_t size_cells,
                         pci_window_t* mem32, pci_window_t* mem64);

#endif
