#ifndef KIRA_VTD_REGS_H
#define KIRA_VTD_REGS_H
#include <stdint.h>
#include <stddef.h>

/* Intel VT-d architectural register offsets (Intel VT-d Architecture Spec). */
#define VTD_VER       0x000u
#define VTD_CAP       0x008u
#define VTD_ECAP      0x010u
#define VTD_GCMD      0x018u
#define VTD_GSTS      0x01cu
#define VTD_RTADDR    0x020u
#define VTD_CCMD      0x028u
#define VTD_FSTS      0x034u
#define VTD_FECTL     0x038u
#define VTD_FEDATA    0x03cu
#define VTD_FEADDR    0x040u
#define VTD_FEUADDR   0x044u
#define VTD_IQH       0x080u
#define VTD_IQT       0x088u
#define VTD_IQA       0x090u
#define VTD_IRTA      0x0b8u

#define VTD_GCMD_TE   (1u << 31)
#define VTD_GCMD_SRTP (1u << 30)
#define VTD_GCMD_QIE  (1u << 26)
#define VTD_GCMD_IRE  (1u << 25)
#define VTD_GCMD_WBF  (1u << 27)
#define VTD_GSTS_TES  (1u << 31)
#define VTD_GSTS_RTPS (1u << 30)
#define VTD_GSTS_QIES (1u << 26)
#define VTD_GSTS_IRES (1u << 25)
#define VTD_GSTS_WBFS (1u << 27)
#define VTD_CCMD_ICC  (1ull << 63)
#define VTD_CCMD_GLOBAL (1ull << 60)
#define VTD_IOTLB_IVT (1ull << 63)
#define VTD_IOTLB_GLOBAL (1ull << 60)
#define VTD_IOTLB_DW (1ull << 49)
#define VTD_IOTLB_DR (1ull << 48)
#define VTD_ECAP_IRO_SHIFT 8u
#define VTD_ECAP_IRO_MASK (0x3ffull << VTD_ECAP_IRO_SHIFT)

typedef struct { volatile uint8_t *base; size_t length; } kira_vtd_mmio_t;
static inline int kira_vtd_valid(const kira_vtd_mmio_t *m, uint32_t off, size_t n) {
    return m && m->base && n && off <= m->length && n <= m->length - off;
}
static inline int kira_vtd_read32(const kira_vtd_mmio_t *m,uint32_t off,uint32_t *v) {
    if (!v || !kira_vtd_valid(m,off,4)) return -1;
    *v=*(volatile uint32_t *)(void *)(m->base+off);
    return 0;
}

static inline int kira_vtd_read64(const kira_vtd_mmio_t *m,uint32_t off,uint64_t *v) {
    if (!v || !kira_vtd_valid(m,off,8) || (off & 7u)) return -1;
    *v=*(volatile uint64_t *)(void *)(m->base+off);
    return 0;
}

static inline int kira_vtd_write32(const kira_vtd_mmio_t *m,uint32_t off,uint32_t v) {
    if (!kira_vtd_valid(m,off,4)) return -1;
    *(volatile uint32_t *)(void *)(m->base+off)=v;
    __asm__ volatile("" ::: "memory");
    return 0;
}
static inline int kira_vtd_write64(const kira_vtd_mmio_t *m,uint32_t off,uint64_t v) {
    if (!kira_vtd_valid(m,off,8) || (off & 7u)) return -1;
    *(volatile uint64_t *)(void *)(m->base+off)=v;
    __asm__ volatile("" ::: "memory");
    return 0;
}
static inline int kira_vtd_root_table_valid(uint64_t physical, uint64_t bytes) {
    return physical && !(physical & 0xfffu) &&
           !(physical & ~0x000ffffffffff000ull) && bytes >= 4096u &&
           bytes <= (1ull << 32) && physical <= UINT64_MAX - bytes;
}
#define KIRA_VTD_POLL_LIMIT 1000000u

static inline int kira_vtd_wait32(const kira_vtd_mmio_t *m,uint32_t off,
                                  uint32_t mask,int want_set) {
    for(uint32_t i=0;i<KIRA_VTD_POLL_LIMIT;i++) {
        uint32_t value=0;
        if(kira_vtd_read32(m,off,&value)!=0) return -1;
        if(((value&mask)!=0)==(want_set!=0)) return 0;
        __asm__ volatile("pause");
    }
    return -2;
}

static inline int kira_vtd_flush_write_buffer(const kira_vtd_mmio_t *m) {
    uint32_t command=0;
    if(kira_vtd_read32(m,VTD_GCMD,&command)!=0||
       kira_vtd_write32(m,VTD_GCMD,command|VTD_GCMD_WBF)!=0) return -1;
    return kira_vtd_wait32(m,VTD_GSTS,VTD_GSTS_WBFS,0);
}

static inline int kira_vtd_invalidate_context_global(const kira_vtd_mmio_t *m) {
    if(kira_vtd_write64(m,VTD_CCMD,VTD_CCMD_ICC|VTD_CCMD_GLOBAL)!=0) return -1;
    for(uint32_t i=0;i<KIRA_VTD_POLL_LIMIT;i++) {
        uint64_t command=0;
        if(kira_vtd_read64(m,VTD_CCMD,&command)!=0) return -1;
        if(!(command&VTD_CCMD_ICC)) return 0;
        __asm__ volatile("pause");
    }
    return -2;
}

/* Register-based global IOTLB invalidation (no queued-invalidation dependency). */
static inline int kira_vtd_invalidate_iotlb_global(const kira_vtd_mmio_t *m,
                                                    uint64_t ecap) {
    uint32_t iro=(uint32_t)((ecap&VTD_ECAP_IRO_MASK)>>VTD_ECAP_IRO_SHIFT);
    uint32_t base=iro*16u;
    if(!iro||!kira_vtd_valid(m,base,16u)) return -1;
    if(kira_vtd_write64(m,base,0)!=0||
       kira_vtd_write64(m,base+8u,VTD_IOTLB_IVT|VTD_IOTLB_GLOBAL|
                                  VTD_IOTLB_DW|VTD_IOTLB_DR)!=0) return -1;
    for(uint32_t i=0;i<KIRA_VTD_POLL_LIMIT;i++) {
        uint64_t request=0;
        if(kira_vtd_read64(m,base+8u,&request)!=0) return -1;
        if(!(request&VTD_IOTLB_IVT)) return 0;
        __asm__ volatile("pause");
    }
    return -2;
}

/* Installs the root table and enables second-level DMA translation.
 * The caller must have populated all contexts before invoking this helper. */
static inline int kira_vtd_enable_translation(const kira_vtd_mmio_t *m,
                                               uint64_t root_phys,
                                               uint64_t ecap) {
    if(!m||!kira_vtd_root_table_valid(root_phys,4096u)||
       kira_vtd_write64(m,VTD_RTADDR,root_phys)!=0) return -1;
    uint32_t command=0;
    if(kira_vtd_read32(m,VTD_GCMD,&command)!=0||
       kira_vtd_write32(m,VTD_GCMD,command|VTD_GCMD_SRTP)!=0||
       kira_vtd_wait32(m,VTD_GSTS,VTD_GSTS_RTPS,1)!=0) return -2;
    if(kira_vtd_flush_write_buffer(m)!=0||
       kira_vtd_invalidate_context_global(m)!=0||
       kira_vtd_invalidate_iotlb_global(m,ecap)!=0||
       kira_vtd_flush_write_buffer(m)!=0) return -3;
    if(kira_vtd_read32(m,VTD_GCMD,&command)!=0||
       kira_vtd_write32(m,VTD_GCMD,command|VTD_GCMD_TE)!=0||
       kira_vtd_wait32(m,VTD_GSTS,VTD_GSTS_TES,1)!=0) return -4;
    return 0;
}

#endif
