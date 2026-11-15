#ifndef KIRA_PCI_ROUTING_H
#define KIRA_PCI_ROUTING_H
#include <stddef.h>
#include <stdint.h>
#include "acpi_prt.h"
#define KIRA_PCI_MAX_ROUTES 256u
struct kira_pci_irq_route {
 uint16_t segment; uint8_t bus, device, function, pin;
 uint32_t gsi; uint16_t flags; uint8_t valid;
};
struct kira_pci_route_ops {
 void *context;
 int (*resolve_link)(void *context,const char *path,uint32_t source_index,
                     uint32_t *gsi,uint16_t *flags);
 int (*isa_irq_to_gsi)(void *context,uint8_t irq,uint32_t *gsi,uint16_t *flags);
};
struct kira_pci_route_table { struct kira_pci_irq_route routes[KIRA_PCI_MAX_ROUTES]; size_t count; };
int kira_pci_routes_build(struct kira_pci_route_table *out,
 const struct kira_acpi_prt_table *prt,uint16_t segment,uint8_t bus,
 const struct kira_pci_route_ops *ops);
int kira_pci_route_lookup(const struct kira_pci_route_table *table,
 uint8_t device,uint8_t function,uint8_t pin,uint32_t *gsi,uint16_t *flags);
#endif
