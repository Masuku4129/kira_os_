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
#define VTD_CCMD_ICC  (1ull << 63)
#define VTD_CCMD_GLOBAL (1ull << 61)

typedef struct { volatile uint8_t *base; size_t length; } kira_vtd_mmio_t;
static inline int kira_vtd_valid(const kira_vtd_mmio_t *m, uint32_t off, size_t n) {
    return m && m->base && n && off <= m->length && n <= m->length - off;
}
static inline int kira_vtd_read32(const kira_vtd_mmio_t *m,uint32_t off,uint32_t *v) {
    if (!v || !kira_vtd_valid(m,off,4)) return -1;
    *v=*(volatile uint32_t *)(void *)(m->base+off);
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
    return physical && !(physical & 0xfffu) && bytes >= 4096u &&
           bytes <= (1ull << 32) && physical <= UINT64_MAX - bytes;
}
#endif
