/*
 * VT-d second-level domain against a small hardware model.
 *
 * The model implements what matters for DMA isolation: an IOTLB that caches
 * translations and is emptied only by a completed IOTLB invalidation command.
 * A map/unmap that forgets IOTLB invalidation leaves a stale translation, so a
 * device keeps reaching memory the domain no longer owns - the test fails then.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/iommu/vtd_domain.h"

#define CHECK(x) do { if (!(x)) { printf("vtd_domain FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)

#define IRO_BYTES   0x100u              /* ECAP.IRO = 0x10 -> IOTLB block at 0x100, command at 0x108 */
#define CCMD_OFF    0x28u
#define IOTLB_OFF   (IRO_BYTES + 8u)

static uint8_t *regs;
static int ccmd_count, iotlb_count;
static uint64_t last_iotlb_cmd;
static int hw_never_completes;          /* model: invalidation never finishes */
static int hw_rejects;                  /* model: IAIG = 0, request rejected */

/* IOTLB: one cached translation, filled by a DMA access, emptied by invalidation. */
static struct { int valid; uint64_t iova, pa; } iotlb;
static void *tables[64];
static unsigned table_count;

static uint64_t *reg64(uint32_t off) { return (uint64_t *)(regs + off); }

/* Called as the write barrier: the hardware "observes" the command write. */
static void hw_observe(void *ctx)
{
    (void)ctx;
    if (hw_never_completes)
        return;
    if (*reg64(CCMD_OFF) & (1ull << 63)) {
        *reg64(CCMD_OFF) &= ~(1ull << 63);
        ccmd_count++;
    }
    if (*reg64(IOTLB_OFF) & (1ull << 63)) {
        uint64_t v = *reg64(IOTLB_OFF);
        last_iotlb_cmd = v;
        iotlb_count++;
        if (hw_rejects) {
            v &= ~(1ull << 63);
            v &= ~(3ull << 57);
        } else {
            v &= ~(1ull << 63);
            v = (v & ~(3ull << 57)) | (1ull << 57);   /* IAIG = global */
            iotlb.valid = 0;                          /* global invalidation */
        }
        *reg64(IOTLB_OFF) = v;
    }
}

static void *alloc_page(void *ctx, uint64_t *phys)
{
    void *p;
    (void)ctx;
    if (table_count >= 64) { *phys = 0; return 0; }
    p = aligned_alloc(4096, 4096);
    if (!p) { *phys = 0; return 0; }
    memset(p, 0, 4096);
    tables[table_count++] = p;
    *phys = (uint64_t)(uintptr_t)p;     /* identity: physical == host pointer */
    return p;
}

static void free_page(void *ctx, void *v, uint64_t phys) { (void)ctx; (void)phys; (void)v; }

static volatile void *map_mmio(void *ctx, uint64_t phys, size_t len)
{
    (void)ctx; (void)len;
    return (volatile void *)(uintptr_t)phys;
}

/* Device DMA through the domain: IOTLB hit wins, otherwise walk the tables. */
static int dma_translate(const struct kira_vtd_domain *d, uint64_t iova, uint64_t *pa)
{
    unsigned int shifts[4] = {39, 30, 21, 12};
    uint64_t *table = (uint64_t *)d->second_level;
    unsigned int level;

    if (iotlb.valid && iotlb.iova == iova) { *pa = iotlb.pa; return 0; }
    for (level = 0; level < 4; ++level) {
        uint64_t e = table[(iova >> shifts[level]) & 0x1ff];
        if (!(e & 1)) return -1;                         /* DMA fault */
        if (level == 3) {
            *pa = e & 0x000ffffffffff000ull;
            iotlb.valid = 1; iotlb.iova = iova; iotlb.pa = *pa;
            return 0;
        }
        table = (uint64_t *)(uintptr_t)(e & 0x000ffffffffff000ull);
    }
    return -1;
}

int main(void)
{
    struct kira_vtd_platform plat;
    struct kira_vtd_unit unit;
    struct kira_vtd_domain dom;
    uint64_t pa = 0;
    int before;

    regs = aligned_alloc(4096, 4096);
    CHECK(regs);
    memset(regs, 0, 4096);

    memset(&plat, 0, sizeof(plat));
    plat.map_mmio = map_mmio;
    plat.alloc_dma_page = alloc_page;
    plat.free_dma_page = free_page;
    plat.write_barrier = hw_observe;

    memset(&unit, 0, sizeof(unit));
    unit.registers = regs;
    unit.register_size = 4096;
    unit.capabilities = (1ull << 10) | (1ull << 54) | (1ull << 55);  /* 48-bit walk, DWD, DRD */
    unit.extended_capabilities = (uint64_t)(IRO_BYTES / 16u) << 8;
    unit.host_address_width = 48;
    unit.translation_enabled = 1;
    unit.root_table = alloc_page(0, &unit.root_table_physical);
    CHECK(unit.root_table);

    /* Attaching a device publishes a context entry: context cache AND IOTLB must be flushed. */
    CHECK(kira_vtd_domain_create(&dom, &unit, 0, 0, 3, 0, &plat) == KIRA_VTD_OK);
    CHECK(ccmd_count == 1);
    CHECK(iotlb_count == 1);
    CHECK(((last_iotlb_cmd >> 60) & 3) == 1);          /* IIRG = global */
    CHECK(last_iotlb_cmd & (1ull << 49));              /* read drain, DRD supported */
    CHECK(last_iotlb_cmd & (1ull << 48));              /* write drain, DWD supported */

    /* map -> device can translate; the command is issued after the PTE is written. */
    before = iotlb_count;
    CHECK(kira_vtd_domain_map_4k(&dom, 0x10000, 0x200000, KIRA_VTD_MAP_READ | KIRA_VTD_MAP_WRITE, &plat) == KIRA_VTD_OK);
    CHECK(iotlb_count == before + 1);
    CHECK(dma_translate(&dom, 0x10000, &pa) == 0 && pa == 0x200000);

    /* The device now has a cached translation. unmap must invalidate it. */
    CHECK(iotlb.valid);
    before = iotlb_count;
    CHECK(kira_vtd_domain_unmap_4k(&dom, 0x10000, &plat) == KIRA_VTD_OK);
    CHECK(iotlb_count == before + 1);
    CHECK(!iotlb.valid);
    CHECK(dma_translate(&dom, 0x10000, &pa) != 0);     /* no stale access after unmap */

    /* Hardware that rejects the request, or never completes it, must surface as an error. */
    hw_rejects = 1;
    CHECK(kira_vtd_domain_map_4k(&dom, 0x20000, 0x300000, KIRA_VTD_MAP_READ, &plat) == KIRA_VTD_HARDWARE_ERROR);
    hw_rejects = 0;
    hw_never_completes = 1;
    CHECK(kira_vtd_domain_map_4k(&dom, 0x30000, 0x400000, KIRA_VTD_MAP_READ, &plat) == KIRA_VTD_TIMEOUT);
    hw_never_completes = 0;
    *reg64(IOTLB_OFF) = 0; *reg64(CCMD_OFF) = 0;

    /* A missing or out-of-page IOTLB register block is refused, not written blindly. */
    unit.extended_capabilities = (uint64_t)0x3ff << 8;   /* IRO = 0x3ff0, beyond the 4 KiB page */
    CHECK(kira_vtd_domain_map_4k(&dom, 0x40000, 0x500000, KIRA_VTD_MAP_READ, &plat) == KIRA_VTD_HARDWARE_ERROR);

    puts("vtd_domain: PASS");
    return 0;
}
