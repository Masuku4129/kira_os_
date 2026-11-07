#include "pci_routing.h"

/*
 * Resolve ACPI _PRT entries into immutable PCI interrupt routes.
 * A NullName source uses SourceIndex as an ISA IRQ; named sources are
 * resolved by the AML/link-device resource layer supplied by the platform.
 */
int kira_pci_routes_build(struct kira_pci_route_table *out,
 const struct kira_acpi_prt_table *prt,uint16_t segment,uint8_t bus,
 const struct kira_pci_route_ops *ops)
{
 size_t i;
 if(!out||!prt||!ops)return -1;
 out->count=0;
 for(i=0;i<prt->count;i++){
  const struct kira_acpi_prt_entry *p=&prt->entries[i];
  struct kira_pci_irq_route r;
  int rc;
  if(out->count==KIRA_PCI_MAX_ROUTES)return -2;
  if(p->device>31||p->pin>3)continue;
  r.segment=segment;r.bus=bus;r.device=(uint8_t)p->device;
  r.function=p->function;r.pin=p->pin;r.flags=0;r.gsi=0;r.valid=0;
  if(p->link_device){
   if(!ops->resolve_link)continue;
   rc=ops->resolve_link(ops->context,p->source,p->source_index,&r.gsi,&r.flags);
  }else{
   if(p->source_index>15||!ops->isa_irq_to_gsi)continue;
   rc=ops->isa_irq_to_gsi(ops->context,(uint8_t)p->source_index,&r.gsi,&r.flags);
  }
  if(rc!=0)continue;
  r.valid=1;out->routes[out->count++]=r;
 }
 return 0;
}
int kira_pci_route_lookup(const struct kira_pci_route_table *t,
 uint8_t device,uint8_t function,uint8_t pin,uint32_t *gsi,uint16_t *flags)
{
 size_t i;
 if(!t||!gsi||!flags||device>31||pin>3)return -1;
 for(i=0;i<t->count;i++){
  const struct kira_pci_irq_route *r=&t->routes[i];
  if(r->valid&&r->device==device&&r->pin==pin&&
     (r->function==function||r->function==0xff)){
   *gsi=r->gsi;*flags=r->flags;return 0;
  }
 }
 return -2;
}
