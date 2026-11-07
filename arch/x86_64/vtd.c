#include "vtd.h"
#include "acpi.h"
#include "pmm.h"
#include "vmm.h"
#include "arch/x86_64/vtd_regs.h"
#include "arch/x86_64/vtd_tables.h"

#define VTD_MAX_UNITS 16u
#define VTD_MAX_DOMAINS PCI_MAX_DEVS
#define VTD_REG_BYTES 0x5000u
#define VTD_AGAW_48 2u
#define VTD_CAP_SAGAW_SHIFT 8u
#define VTD_CAP_SAGAW_48 (1ull << (VTD_CAP_SAGAW_SHIFT+VTD_AGAW_48))
#define VTD_CAP_SLLPS_2M (1ull << 34)
#define VTD_CAP_ND_MASK 0x07ull
#define VTD_CAP_MGAW_SHIFT 16u
#define VTD_CAP_MGAW_MASK (0x3full << VTD_CAP_MGAW_SHIFT)
#define VTD_MAX_RMRR_PAGES 16384u

/* Implemented by the KDRV manager; trusted in-kernel drivers receive identity
 * domains, while untrusted Ring 3 drivers start with an empty second-level map. */
extern int kdrv_builtin_owned(const pci_dev_t *device);

typedef struct {
    int valid,enabled,supports_2m;
    uint16_t segment;
    uint32_t next_domain,domain_limit;
    uint64_t cap,ecap,low_mapped_end;
    kira_vtd_mmio_t mmio;
    kira_vtd_root_table_t *root;
    uint64_t root_phys;
    kira_vtd_pt_t *identity_root;
    uint64_t identity_phys;
    kira_vtd_context_table_t *context[256];
    uint64_t context_phys[256];
    pmm_range_t high_ranges[PMM_MAX_HIGH];
    unsigned high_count;
} vtd_unit_t;

typedef struct {
    const pci_dev_t *device;
    uint8_t unit;
    uint8_t isolated;
    uint16_t domain_id;
    kira_vtd_pt_t *root;
    uint64_t root_phys;
} vtd_domain_t;

static vtd_unit_t units[VTD_MAX_UNITS];
static vtd_domain_t domains[VTD_MAX_DOMAINS];
static unsigned unit_count,domain_count;
static int initialized;

static void *vtd_alloc_page(uint64_t *physical) {
    uint64_t pa=pmm_alloc_contiguous(1);
    if(!pa) return 0;
    volatile void *va=vmm_map_mmio(pa,KIRA_VTD_PAGE_SIZE,VMM_MMIO_UC);
    if(!va) { (void)pmm_free_contiguous(pa,1); return 0; }
    volatile uint64_t *q=(volatile uint64_t*)va;
    for(unsigned i=0;i<KIRA_VTD_ENTRIES;i++) q[i]=0;
    if(physical) *physical=pa;
    return (void*)va;
}

static kira_vtd_pt_t *vtd_resolve_pt(void *ctx,uint64_t physical) {
    (void)ctx;
    if(!kira_vtd_phys_page_valid(physical)) return 0;
    return (kira_vtd_pt_t*)vmm_map_mmio(physical,KIRA_VTD_PAGE_SIZE,VMM_MMIO_UC);
}

static kira_vtd_pt_t *vtd_alloc_pt(void *ctx,uint64_t *physical) {
    (void)ctx;
    return (kira_vtd_pt_t*)vtd_alloc_page(physical);
}

static const kira_vtd_pt_ops_t pt_ops={0,vtd_resolve_pt,vtd_alloc_pt};

static int vtd_map_identity_range(vtd_unit_t *u,uint64_t start,uint64_t end) {
    if(!u||start>=end||end>(1ull<<48)) return -1;
    uint64_t step=u->supports_2m?KIRA_VTD_2M_SIZE:KIRA_VTD_PAGE_SIZE;
    start&=~(step-1u);
    if(end>UINT64_MAX-(step-1u)) return -1;
    end=(end+step-1u)&~(step-1u);
    /* Physical page zero is deliberately left unmapped. If 2 MiB leaves are
       supported, split the first 2 MiB into 4 KiB leaves to preserve NULL. */
    if(start==0) {
        uint64_t first_end=end<KIRA_VTD_2M_SIZE?end:KIRA_VTD_2M_SIZE;
        for(uint64_t p=KIRA_VTD_PAGE_SIZE;p<first_end;p+=KIRA_VTD_PAGE_SIZE)
            if(kira_vtd_map_4k_walk(u->identity_root,p,p,1,1,&pt_ops)!=0)
                return -2;
        start=KIRA_VTD_2M_SIZE;
    }
    for(uint64_t p=start;p<end;p+=step) {
        int rc=u->supports_2m?
            kira_vtd_map_2m_walk(u->identity_root,p,p,1,1,&pt_ops):
            kira_vtd_map_4k_walk(u->identity_root,p,p,1,1,&pt_ops);
        if(rc!=0) return -2;
    }
    return 0;
}

static int vtd_identity_mapped(const vtd_unit_t *u,uint64_t base,uint64_t end) {
    if(end<=u->low_mapped_end) return 1;
    for(unsigned i=0;i<u->high_count;i++)
        if(base>=u->high_ranges[i].start&&end<=u->high_ranges[i].end) return 1;
    return 0;
}

static int vtd_build_identity_map(vtd_unit_t *u) {
    uint64_t top=pmm_managed_top();
    if(!top||top>(1ull<<48)) return -1;
    uint64_t low_end=top<(1ull<<30)?top:(1ull<<30);
    if(vtd_map_identity_range(u,0,low_end)!=0) return -2;
    u->low_mapped_end=(low_end+KIRA_VTD_PAGE_SIZE-1u)&
                      ~(uint64_t)(KIRA_VTD_PAGE_SIZE-1u);
    u->high_count=pmm_high_ranges(u->high_ranges,PMM_MAX_HIGH);
    for(unsigned i=0;i<u->high_count;i++)
        if(vtd_map_identity_range(u,u->high_ranges[i].start,
                                  u->high_ranges[i].end)!=0) return -3;
    return 0;
}

static int vtd_scope_matches(const acpi_info_t *acpi,unsigned scope_index,
                             const pci_dev_t *device) {
    if(!acpi||!device||scope_index>=acpi->dmar_scope_count) return 0;
    const acpi_dmar_scope_t *scope=&acpi->dmar_scope[scope_index];
    if((scope->type!=1&&scope->type!=2)||!scope->path_count||
       scope->path_count>16) return 0;
    uint8_t bus=scope->start_bus;
    for(uint8_t i=0;i<scope->path_count;i++) {
        uint8_t devfn=scope->devfn[i];
        int last=(i+1u==scope->path_count);
        if(last&&scope->type==1)
            return bus==device->bus&&
                   devfn==(uint8_t)((device->dev<<3)|device->fn);
        pci_dev_t bridge={0};
        bridge.bus=bus; bridge.dev=(uint8_t)(devfn>>3);
        bridge.fn=(uint8_t)(devfn&7u);
        if(pci_read8(&bridge,0x0B)!=0x06||
           pci_read8(&bridge,0x0A)!=0x04) return 0;
        uint8_t secondary=pci_read8(&bridge,0x19);
        uint8_t subordinate=pci_read8(&bridge,0x1A);
        if(!secondary||secondary>subordinate) return 0;
        if(last) return scope->type==2&&device->bus>=secondary&&
                        device->bus<=subordinate;
        bus=secondary;
    }
    return 0;
}

static int vtd_select_unit(const pci_dev_t *device) {
    const acpi_info_t *acpi=acpi_get();
    if(!acpi||!device) return -1;
    int include_all=-1, scoped=-1;
    for(unsigned i=0;i<unit_count;i++) {
        vtd_unit_t *u=&units[i];
        if(!u->valid||u->segment!=0) continue;
        if(u->root&&u->root_phys) {
            for(unsigned s=0;s<acpi->dmar_scope_count;s++) {
                const acpi_dmar_scope_t *scope=&acpi->dmar_scope[s];
                if(scope->owner_type==0&&scope->owner_index==i&&
                   (scope->type==1||scope->type==2)&&
                   vtd_scope_matches(acpi,s,device)) {
                    if(scoped>=0&&scoped!=(int)i) return -1;
                    scoped=(int)i;
                }
            }
        }
        if(acpi->dmar_unit[i].include_all) {
            if(include_all>=0) return -1;
            include_all=(int)i;
        }
    }
    return scoped>=0?scoped:include_all;
}

static int vtd_alloc_domain_id(vtd_unit_t *u,uint16_t *out) {
    if(!u||!out||u->next_domain>=u->domain_limit||u->next_domain>0xffffu)
        return -1;
    *out=(uint16_t)u->next_domain++;
    if(!*out) return -1;
    return 0;
}

static int vtd_ensure_context(vtd_unit_t *u,uint8_t bus) {
    if(u->context[bus]) return 0;
    uint64_t pa=0;
    kira_vtd_context_table_t *ct=(kira_vtd_context_table_t*)vtd_alloc_page(&pa);
    if(!ct||!kira_vtd_phys_page_valid(pa)) return -1;
    if(kira_vtd_root_set(u->root,bus,pa)!=0) return -2;
    u->context[bus]=ct;
    u->context_phys[bus]=pa;
    return 0;
}

static int vtd_install_context(vtd_unit_t *u,const pci_dev_t *d,
                               uint64_t slpt_phys,uint16_t did) {
    if(!u||!d||vtd_ensure_context(u,d->bus)!=0) return -1;
    uint8_t devfn=(uint8_t)((d->dev<<3)|d->fn);
    return kira_vtd_context_set(u->context[d->bus],devfn,slpt_phys,did,VTD_AGAW_48);
}

static int vtd_invalidate(vtd_unit_t *u) {
    if(!u||!u->enabled) return -1;
    if(kira_vtd_flush_write_buffer(&u->mmio)!=0||
       kira_vtd_invalidate_context_global(&u->mmio)!=0||
       kira_vtd_invalidate_iotlb_global(&u->mmio,u->ecap)!=0||
       kira_vtd_flush_write_buffer(&u->mmio)!=0) return -1;
    return 0;
}

static int vtd_map_4k_range(kira_vtd_pt_t *root,uint64_t base,uint64_t length) {
    if(!root||!length||(base&0xfffu)||(length&0xfffu)||
       base>UINT64_MAX-length||base+length>(1ull<<48)) return -1;
    for(uint64_t p=base;p<base+length;p+=KIRA_VTD_PAGE_SIZE)
        if(kira_vtd_map_4k_walk(root,p,p,1,1,&pt_ops)!=0) return -2;
    return 0;
}

static int vtd_map_rmrr(vtd_unit_t *u,kira_vtd_pt_t *root,
                        const pci_dev_t *device,int identity_domain) {
    const acpi_info_t *acpi=acpi_get();
    if(!acpi||!root||!device) return -1;
    for(unsigned r=0;r<acpi->dmar_rmrr_count;r++) {
        const acpi_dmar_rmrr_t *rmrr=&acpi->dmar_rmrr[r];
        if(rmrr->segment!=0) continue;
        if(rmrr->limit<rmrr->base||rmrr->limit>UINT64_MAX-0x1000u)
            return -2;
        int matched=0;
        for(unsigned s=0;s<acpi->dmar_scope_count;s++) {
            const acpi_dmar_scope_t *scope=&acpi->dmar_scope[s];
            if(scope->owner_type==1&&scope->owner_index==r&&
               (scope->type==1||scope->type==2)&&
               vtd_scope_matches(acpi,s,device)) { matched=1; break; }
        }
        if(!matched) continue;
        uint64_t base=rmrr->base&~(uint64_t)0xfffu;
        uint64_t end=(rmrr->limit+1u+0xfffu)&~(uint64_t)0xfffu;
        if(end<=base||end-base>(uint64_t)VTD_MAX_RMRR_PAGES*KIRA_VTD_PAGE_SIZE)
            return -2;
        if(identity_domain&&vtd_identity_mapped(u,base,end)) continue;
        if(vtd_map_4k_range(root,base,end-base)!=0) return -3;
    }
    return 0;
}

static vtd_domain_t *vtd_find_domain(const pci_dev_t *d) {
    if(!d) return 0;
    for(unsigned i=0;i<domain_count;i++) {
        const pci_dev_t *x=domains[i].device;
        if(x&&x->bus==d->bus&&x->dev==d->dev&&x->fn==d->fn)
            return &domains[i];
    }
    return 0;
}

static int vtd_add_domain(const pci_dev_t *d,unsigned unit,uint16_t did,
                          kira_vtd_pt_t *root,uint64_t root_phys,int isolated) {
    if(domain_count>=VTD_MAX_DOMAINS) return -1;
    domains[domain_count++]=(vtd_domain_t){
        .device=d,.unit=(uint8_t)unit,.isolated=(uint8_t)(isolated!=0),
        .domain_id=did,.root=root,.root_phys=root_phys
    };
    return 0;
}

static int vtd_disable_firmware_modes(vtd_unit_t *u) {
    uint32_t command=0;
    if(kira_vtd_read32(&u->mmio,VTD_GCMD,&command)!=0) return -1;
    if(command&VTD_GCMD_TE) {
        if(kira_vtd_write32(&u->mmio,VTD_GCMD,command&~VTD_GCMD_TE)!=0||
           kira_vtd_wait32(&u->mmio,VTD_GSTS,VTD_GSTS_TES,0)!=0) return -2;
        if(kira_vtd_read32(&u->mmio,VTD_GCMD,&command)!=0) return -1;
    }
    if(command&VTD_GCMD_QIE) {
        if(kira_vtd_write32(&u->mmio,VTD_GCMD,command&~VTD_GCMD_QIE)!=0||
           kira_vtd_wait32(&u->mmio,VTD_GSTS,VTD_GSTS_QIES,0)!=0) return -3;
        if(kira_vtd_read32(&u->mmio,VTD_GCMD,&command)!=0) return -1;
    }
    if(command&VTD_GCMD_IRE) {
        if(kira_vtd_write32(&u->mmio,VTD_GCMD,command&~VTD_GCMD_IRE)!=0||
           kira_vtd_wait32(&u->mmio,VTD_GSTS,VTD_GSTS_IRES,0)!=0) return -4;
    }
    return 0;
}

static int vtd_unit_prepare(unsigned index,const acpi_info_t *acpi) {
    if(index>=acpi->dmar_unit_count||index>=VTD_MAX_UNITS) return -1;
    vtd_unit_t *u=&units[index];
    u->segment=acpi->dmar_unit[index].segment;
    if(u->segment!=0||!acpi->dmar_unit[index].reg_base||
       (acpi->dmar_unit[index].reg_base&0xfffu)) return -2;
    u->mmio.base=(volatile uint8_t*)vmm_map_mmio(
        acpi->dmar_unit[index].reg_base,VTD_REG_BYTES,VMM_MMIO_UC);
    u->mmio.length=VTD_REG_BYTES;
    if(!u->mmio.base||kira_vtd_read64(&u->mmio,VTD_CAP,&u->cap)!=0||
       kira_vtd_read64(&u->mmio,VTD_ECAP,&u->ecap)!=0) return -3;
    if(vtd_disable_firmware_modes(u)!=0) return -3;
    if(!(u->cap&VTD_CAP_SAGAW_48)||
       ((u->cap&VTD_CAP_MGAW_MASK)>>VTD_CAP_MGAW_SHIFT)<48u||
       !((u->ecap&VTD_ECAP_IRO_MASK)>>VTD_ECAP_IRO_SHIFT)) return -4;
    uint32_t nd=(uint32_t)(u->cap&VTD_CAP_ND_MASK);
    uint32_t bits=4u+nd;
    u->domain_limit=bits>=16u?65536u:(1u<<bits);
    if(u->domain_limit<2u) return -5;
    u->next_domain=1;
    u->supports_2m=(u->cap&VTD_CAP_SLLPS_2M)!=0;

    u->root=(kira_vtd_root_table_t*)vtd_alloc_page(&u->root_phys);
    u->identity_root=(kira_vtd_pt_t*)vtd_alloc_page(&u->identity_phys);
    if(!u->root||!u->identity_root||
       !kira_vtd_root_table_valid(u->root_phys,KIRA_VTD_PAGE_SIZE)||
       !kira_vtd_phys_page_valid(u->identity_phys)) return -6;
    if(vtd_build_identity_map(u)!=0) return -7;
    u->valid=1;
    return 0;
}

int vtd_init(void) {
    if(initialized) {
        int n=0; for(unsigned i=0;i<unit_count;i++) if(units[i].enabled) n++;
        return n;
    }
    initialized=1;
    /* Stop firmware-started bus mastering before translation tables are installed. */
    for(int i=0;i<pci_count();i++) {
        const pci_dev_t *d=pci_get(i);
        if(!d) continue;
        uint16_t command=pci_read16(d,0x04);
        pci_write16(d,0x04,(uint16_t)(command&~PCI_CMD_MASTER));
    }
    const acpi_info_t *acpi=acpi_get();
    if(!acpi||!acpi->dmar_valid||!acpi->dmar_unit_count) return -1;
    unit_count=acpi->dmar_unit_count<VTD_MAX_UNITS?
               acpi->dmar_unit_count:VTD_MAX_UNITS;
    for(unsigned i=0;i<unit_count;i++) (void)vtd_unit_prepare(i,acpi);

    /* Trusted in-kernel devices receive a broad identity domain. Unknown
     * devices have no context entry and therefore cannot DMA after TE. */
    for(int i=0;i<pci_count();i++) {
        const pci_dev_t *d=pci_get(i);
        if(!d||d->class_code==0x06||!kdrv_builtin_owned(d)) continue;
        int ui=vtd_select_unit(d);
        if(ui<0||!units[ui].valid) continue;
        vtd_unit_t *u=&units[ui];
        uint16_t did=0;
        if(vtd_map_rmrr(u,u->identity_root,d,1)!=0||
           vtd_alloc_domain_id(u,&did)!=0||
           vtd_install_context(u,d,u->identity_phys,did)!=0||
           vtd_add_domain(d,(unsigned)ui,did,u->identity_root,
                          u->identity_phys,0)!=0) continue;
    }

    int enabled=0;
    for(unsigned i=0;i<unit_count;i++) {
        vtd_unit_t *u=&units[i];
        if(!u->valid) continue;
        if(kira_vtd_enable_translation(&u->mmio,u->root_phys,u->ecap)==0) {
            u->enabled=1;
            enabled++;
        }
    }
    return enabled?enabled:-1;
}

int vtd_device_enable(const pci_dev_t *device) {
    if(!initialized||!device) return -1;
    vtd_domain_t *existing=vtd_find_domain(device);
    if(existing) return existing->isolated?0:-2; /* identity domains are kernel-only */
    int ui=vtd_select_unit(device);
    if(ui<0||!units[ui].enabled) return -3;
    vtd_unit_t *u=&units[ui];
    uint16_t did=0;
    if(vtd_alloc_domain_id(u,&did)!=0) return -4;
    uint64_t root_phys=0;
    kira_vtd_pt_t *root=(kira_vtd_pt_t*)vtd_alloc_page(&root_phys);
    if(!root||!kira_vtd_phys_page_valid(root_phys)) return -5;
    if(vtd_map_rmrr(u,root,device,0)!=0) return -6;
    if(vtd_install_context(u,device,root_phys,did)!=0) return -7;
    if(vtd_invalidate(u)!=0) {
        u->context[device->bus]->entry[(device->dev<<3)|device->fn][0]=0;
        u->context[device->bus]->entry[(device->dev<<3)|device->fn][1]=0;
        (void)vtd_invalidate(u);
        return -8;
    }
    if(vtd_add_domain(device,(unsigned)ui,did,root,root_phys,1)!=0) return -9;
    return 0;
}

int vtd_device_isolated(const pci_dev_t *device) {
    vtd_domain_t *d=vtd_find_domain(device);
    return d&&d->isolated&&units[d->unit].enabled;
}

int vtd_dma_map(const pci_dev_t *device,uint64_t physical,uint64_t length) {
    vtd_domain_t *d=vtd_find_domain(device);
    if(!d||!d->isolated||!units[d->unit].enabled||
       !length||(physical&0xfffu)||(length&0xfffu)||
       physical>UINT64_MAX-length||physical+length>(1ull<<48)) return -1;
    uint64_t mapped=0;
    for(;mapped<length;mapped+=KIRA_VTD_PAGE_SIZE) {
        if(kira_vtd_map_4k_walk(d->root,physical+mapped,physical+mapped,
                                1,1,&pt_ops)!=0) {
            /* Return -3 only when stale IOTLB entries may still reference the
               range. The caller must pin it and retry unmap before page reuse. */
            if(mapped&&vtd_dma_unmap(device,physical,mapped)!=0) return -3;
            return -2;
        }
    }
    if(vtd_invalidate(&units[d->unit])!=0) {
        if(vtd_dma_unmap(device,physical,length)!=0) return -3;
        return -2;
    }
    return 0;
}

int vtd_dma_unmap(const pci_dev_t *device,uint64_t physical,uint64_t length) {
    vtd_domain_t *d=vtd_find_domain(device);
    if(!d||!d->isolated||!units[d->unit].enabled||!length||
       (physical&0xfffu)||(length&0xfffu)||physical>UINT64_MAX-length)
        return -1;
    int rc=0;
    for(uint64_t off=0;off<length;off+=KIRA_VTD_PAGE_SIZE) {
        int urc=kira_vtd_unmap_4k_walk(d->root,physical+off,
                                       vtd_resolve_pt,0,0);
        /* -2 means a previous attempt already cleared the PTE; still flush
           the IOTLB because the hardware may retain the old translation. */
        if(urc!=0&&urc!=-2) rc=-2;
    }
    if(vtd_invalidate(&units[d->unit])!=0) return -3;
    return rc;
}

int vtd_interrupt_remapping_enabled(void) {
    return 0; /* IRTA/interrupt-remapping tables are not enabled by this backend. */
}
