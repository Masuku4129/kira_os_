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
} model_t;

static volatile void *map_mmio(void *opaque, uint64_t physical, size_t length)
{
    model_t *m = opaque;
    assert(length == 4096);
    if (physical == 0xfed90000ull) return m->good;
    if (physical == 0xfeda0000ull) return m->bad;
    return 0;
}
static void *alloc_page(void *opaque, uint64_t *physical)
{
    model_t *m = opaque;
    unsigned n = m->allocations++;
    if (n >= 2) return 0;
    *physical = 0x100000ull + (uint64_t)n * 4096ull;
    return m->pages[n].bytes;
}
static void free_page(void *opaque, void *virtual_address, uint64_t physical)
{
    model_t *m = opaque;
    assert(virtual_address == m->pages[m->frees].bytes);
    assert(physical == 0x100000ull + (uint64_t)m->frees * 4096ull);
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
    ((uint32_t *)m.good)[2] = (1u << 10) | (16u << 24); /* 48-bit SAGAW + valid FRO */
    ((uint32_t *)m.bad)[0] = 0x10;
    ((uint32_t *)m.bad)[2] = (16u << 24); /* lacks 48-bit second-level walk support */

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

    puts("IOMMU discovery: partial VT-d prepare rolls back DMA root pages");
    return 0;
}
