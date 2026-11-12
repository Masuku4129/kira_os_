#include "iommu_discovery.h"
#include "dmar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t good[512];
    uint64_t bad[512];
    union { uint64_t align; uint8_t bytes[4096]; } pages[2];
    unsigned allocations;
    unsigned frees;
    int bad_physical;
    int firmware_enabled;
} model_t;

static volatile void *map_mmio(void *opaque, uint64_t physical, size_t length)
{
    model_t *m = opaque;
    assert(length == 4096);
    if (physical == 0xfed90000ull) {
        if (m->firmware_enabled) ((uint32_t *)m->good)[7] = 1u << 31;
        return m->good;
    }
    if (physical == 0xfeda0000ull) return m->bad;
    return 0;
}
static void *alloc_page(void *opaque, uint64_t *physical)
{
    model_t *m = opaque;
    unsigned n = m->allocations++;
    if (n >= 2) return 0;
    *physical = m->bad_physical ? (1ull << 52) :
                0x100000ull + (uint64_t)n * 4096ull;
    return m->pages[n].bytes;
}
static void free_page(void *opaque, void *virtual_address, uint64_t physical)
{
    model_t *m = opaque;
    assert(virtual_address == m->pages[m->frees].bytes);
    assert(physical == (m->bad_physical ? (1ull << 52) :
                        0x100000ull + (uint64_t)m->frees * 4096ull));
    ++m->frees;
}
static void write_barrier(void *opaque) { (void)opaque; }
static void read_barrier(void *opaque) { (void)opaque; }

int main(void)
{
    model_t m;
    struct kira_dmar_inventory dmar;
    struct kira_vtd_unit units[2];
    struct kira_vtd_platform platform;
    size_t prepared = 99;

    memset(&m, 0, sizeof m);
    memset(&dmar, 0, sizeof dmar);
    memset(units, 0xa5, sizeof units);
    memset(&platform, 0, sizeof platform);

    ((uint32_t *)m.good)[0] = 0x10; /* non-zero VT-d version */
    ((uint32_t *)m.good)[2] = (1u << 10) | (48u << 16) | (16u << 24); /* 48-bit SAGAW + valid FRO */
    ((uint32_t *)m.bad)[0] = 0x10;
    ((uint32_t *)m.bad)[2] = (48u << 16) | (16u << 24); /* lacks 48-bit second-level walk support */

    dmar.host_address_width = 48;
    dmar.unit_count = 2;
    dmar.units[0].segment = 0;
    dmar.units[0].register_base = 0xfed90000ull;
    dmar.units[1].segment = 0;
    dmar.units[1].register_base = 0xfeda0000ull;

    platform.context = &m;
    platform.map_mmio = map_mmio;
    platform.alloc_dma_page = alloc_page;
    platform.free_dma_page = free_page;
    platform.write_barrier = write_barrier;
    platform.read_barrier = read_barrier;

    assert(kira_iommu_prepare_intel_units(&dmar, units, 2, &prepared,
                                          &platform, 0) == KIRA_VTD_UNSUPPORTED);
    assert(prepared == 0);
    assert(m.allocations == 1 && m.frees == 1);
    for (size_t i = 0; i < sizeof units; ++i)
        assert(((const uint8_t *)units)[i] == 0);

    /* A DMA allocator address outside the hardware PTE address mask is freed. */
    memset(&m, 0, sizeof m);
    memset(&platform, 0, sizeof platform);
    m.bad_physical = 1;
    m.good[0] = 0x10;
    ((uint32_t *)m.good)[2] = (1u << 10) | (16u << 24);
    platform.context = &m;
    platform.map_mmio = map_mmio;
    platform.alloc_dma_page = alloc_page;
    platform.free_dma_page = free_page;
    assert(kira_vtd_unit_prepare(&units[0], 0xfed90000ull, 48,
                                 &platform) == KIRA_VTD_MEMORY_FAILED);
    assert(m.allocations == 1 && m.frees == 1);
    for (size_t i = 0; i < sizeof units[0]; ++i)
        assert(((const uint8_t *)&units[0])[i] == 0);

    /* MGAW is a separate capability from SAGAW and must cover the DMAR HAW. */
    memset(&m, 0, sizeof m);
    m.good[0] = 0x10;
    ((uint32_t *)m.good)[2] = (1u << 10) | (47u << 16) | (16u << 24);
    platform.context = &m;
    assert(kira_vtd_unit_prepare(&units[0], 0xfed90000ull, 48,
                                 &platform) == KIRA_VTD_UNSUPPORTED);
    assert(m.allocations == 0 && m.frees == 0);

    /* Firmware-owned active translation is never overwritten. */
    memset(&m, 0, sizeof m);
    m.firmware_enabled = 1;
    m.good[0] = 0x10;
    ((uint32_t *)m.good)[2] = (1u << 10) | (16u << 24);
    platform.context = &m;
    assert(kira_vtd_unit_prepare(&units[0], 0xfed90000ull, 48,
                                 &platform) == KIRA_VTD_UNSUPPORTED);
    assert(m.allocations == 0 && m.frees == 0);

    puts("IOMMU discovery: rollback, DMA width and firmware ownership checks passed");
    return 0;
}
