/*
 * Intel VT-d single-device second-level translation domain.
 *
 * The initial implementation supports 4 KiB leaves and a four-level walk.
 * It deliberately supports one explicit requester per domain and refuses
 * unsupported address widths, duplicate mappings and non-page-aligned input.
 */

#include "vtd_domain.h"

#define VTD_ENTRY_PRESENT 1ull
#define VTD_ENTRY_READ    (1ull << 0)
#define VTD_ENTRY_WRITE   (1ull << 1)
#define VTD_ENTRY_LARGE   (1ull << 7)
#define VTD_ENTRY_ADDRESS 0x000ffffffffff000ull
#define VTD_CCMD          0x028u
#define VTD_POLL_LIMIT    1000000u

static uint64_t *vtd_page(void *p)
{
    return (uint64_t *)p;
}

static void vtd_barrier(const struct kira_vtd_platform *p)
{
    if (p->write_barrier)
        p->write_barrier(p->context);
}

static void *vtd_alloc_page(const struct kira_vtd_platform *p,
                            uint8_t address_width, uint64_t *physical)
{
    void *v = p->alloc_dma_page(p->context, physical);
    unsigned int i;

    if (!v || !*physical || (*physical & 0xfffu) ||
        (*physical & ~VTD_ENTRY_ADDRESS) != 0 ||
        (address_width < 64 && (*physical >> address_width) != 0)) {
        if (v && p->free_dma_page)
            p->free_dma_page(p->context, v, *physical);
        *physical = 0;
        return 0;
    }

    for (i = 0; i < 512; ++i)
        vtd_page(v)[i] = 0;
    return v;
}

static int vtd_invalidate_context(struct kira_vtd_unit *unit,
                                  const struct kira_vtd_platform *p)
{
    volatile uint64_t *ccmd;
    uint64_t value;
    unsigned int i;

    if (!unit || !unit->registers)
        return KIRA_VTD_INVALID;

    ccmd = (volatile uint64_t *)(unit->registers + VTD_CCMD);
    value = (1ull << 63) | (1ull << 61); /* ICC | global context invalidation */
    *ccmd = value;
    vtd_barrier(p);

    for (i = 0; i < VTD_POLL_LIMIT; ++i) {
        if (p->read_barrier)
            p->read_barrier(p->context);
        if ((*ccmd & (1ull << 63)) == 0)
            return KIRA_VTD_OK;
    }
    return KIRA_VTD_TIMEOUT;
}

static int vtd_install_leaf(struct kira_vtd_domain *domain,
                            uint64_t iova, uint64_t physical,
                            uint32_t permissions,
                            const struct kira_vtd_platform *p)
{
    uint64_t *table = vtd_page(domain->second_level);
    unsigned int shifts[4] = {39, 30, 21, 12};
    unsigned int level;

    for (level = 0; level < 3; ++level) {
        unsigned int index = (unsigned int)((iova >> shifts[level]) & 0x1ffu);
        uint64_t entry = table[index];

        if (!(entry & VTD_ENTRY_PRESENT)) {
            uint64_t next_physical = 0;
            void *next = vtd_alloc_page(p, domain->unit->host_address_width, &next_physical);
            if (!next)
                return KIRA_VTD_MEMORY_FAILED;
            table[index] = (next_physical & VTD_ENTRY_ADDRESS) |
                           VTD_ENTRY_PRESENT | VTD_ENTRY_READ |
                           VTD_ENTRY_WRITE;
            if (p->clean_cache)
                p->clean_cache(p->context, table, KIRA_VTD_PAGE_SIZE);
            table = vtd_page(next);
        } else {
            if (entry & VTD_ENTRY_LARGE)
                return KIRA_VTD_HARDWARE_ERROR;
            /*
             * Page-table pages are allocated from the platform DMA allocator.
             * The platform must provide a direct physical-to-virtual mapping
             * for table pages via map_mmio's identity/direct-map contract.
             */
            table = (uint64_t *)p->map_mmio(p->context,
                         entry & VTD_ENTRY_ADDRESS, KIRA_VTD_PAGE_SIZE);
            if (!table)
                return KIRA_VTD_MEMORY_FAILED;
        }
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        uint64_t flags = VTD_ENTRY_PRESENT;
        if (table[index] & VTD_ENTRY_PRESENT)
            return KIRA_VTD_HARDWARE_ERROR;
        if (permissions & KIRA_VTD_MAP_READ)
            flags |= VTD_ENTRY_READ;
        if (permissions & KIRA_VTD_MAP_WRITE)
            flags |= VTD_ENTRY_WRITE;
        if (!(permissions & (KIRA_VTD_MAP_READ | KIRA_VTD_MAP_WRITE)))
            return KIRA_VTD_INVALID;
        table[index] = (physical & VTD_ENTRY_ADDRESS) | flags;
        if (p->clean_cache)
            p->clean_cache(p->context, table, KIRA_VTD_PAGE_SIZE);
    }

    return KIRA_VTD_OK;
}

int kira_vtd_domain_create(struct kira_vtd_domain *domain,
                           struct kira_vtd_unit *unit,
                           uint16_t segment, uint8_t bus,
                           uint8_t device, uint8_t function,
                           const struct kira_vtd_platform *platform)
{
    uint64_t slpt_physical = 0;
    uint64_t context_physical = 0;
    uint64_t *root_entries;
    uint64_t *context_entries;
    uint64_t *entry;
    void *context;
    void *slpt;
    unsigned int requester;
    int new_context = 0;
    int rc;

    if (!domain || !unit || !platform || !platform->alloc_dma_page ||
        !platform->map_mmio || !unit->translation_enabled ||
        !unit->root_table || !unit->root_table_physical ||
        unit->host_address_width < 48 ||
        !(unit->capabilities & (1ull << 10)) ||
        device > 31 || function > 7 || segment != 0)
        return KIRA_VTD_INVALID;

    /*
     * RTADDR was installed once by kira_vtd_unit_enable_translation().
     * A device domain must add its context to that shared root table; it
     * must never replace RTADDR with a private one-device root table.
     */
    root_entries = vtd_page(unit->root_table);
    if (root_entries[bus] & VTD_ENTRY_PRESENT) {
        context_physical = root_entries[bus] & VTD_ENTRY_ADDRESS;
        context = (void *)platform->map_mmio(platform->context,
                                             context_physical,
                                             KIRA_VTD_PAGE_SIZE);
        if (!context)
            return KIRA_VTD_MEMORY_FAILED;
    } else {
        context = vtd_alloc_page(platform, unit->host_address_width, &context_physical);
        if (!context)
            return KIRA_VTD_MEMORY_FAILED;
        new_context = 1;
    }

    context_entries = vtd_page(context);
    requester = ((unsigned int)device << 3) | function;
    entry = &context_entries[requester * 2u];
    if (entry[0] & VTD_ENTRY_PRESENT) {
        if (new_context && platform->free_dma_page)
            platform->free_dma_page(platform->context, context,
                                    context_physical);
        return KIRA_VTD_HARDWARE_ERROR;
    }

    slpt = vtd_alloc_page(platform, unit->host_address_width, &slpt_physical);
    if (!slpt) {
        if (new_context && platform->free_dma_page)
            platform->free_dma_page(platform->context, context,
                                    context_physical);
        return KIRA_VTD_MEMORY_FAILED;
    }

    /*
     * VT-d context entry is 128 bits:
     *   low:  present + second-level translation type (TT=00) + SLPTPTR
     *   high: address-width encoding (AW=2 means a 4-level, 48-bit walk).
     * Present belongs to the low qword; AW belongs to the high qword.
     */
    entry[0] = (slpt_physical & VTD_ENTRY_ADDRESS) | VTD_ENTRY_PRESENT;
    entry[1] = 2ull;

    if (platform->clean_cache) {
        platform->clean_cache(platform->context, context, KIRA_VTD_PAGE_SIZE);
        platform->clean_cache(platform->context, slpt, KIRA_VTD_PAGE_SIZE);
    }

    if (new_context) {
        root_entries[bus] = (context_physical & VTD_ENTRY_ADDRESS) |
                            VTD_ENTRY_PRESENT;
        if (platform->clean_cache)
            platform->clean_cache(platform->context, unit->root_table,
                                  KIRA_VTD_PAGE_SIZE);
    }

    vtd_barrier(platform);

    domain->unit = unit;
    domain->root_context = context;
    domain->root_context_physical = context_physical;
    domain->second_level = slpt;
    domain->second_level_physical = slpt_physical;
    domain->segment = segment;
    domain->bus = bus;
    domain->device = device;
    domain->function = function;
    domain->active = 1;

    /*
     * Do not rewrite RTADDR or issue SRTP here: the unit is already enabled
     * and owns a single shared root table. Invalidate cached contexts after
     * publishing the new entry.
     */
    rc = vtd_invalidate_context(unit, platform);
    return rc;
}
int kira_vtd_domain_map_4k(struct kira_vtd_domain *domain,
                           uint64_t iova, uint64_t physical,
                           uint32_t permissions,
                           const struct kira_vtd_platform *platform)
{
    int rc;

    if (!domain || !domain->active || !platform ||
        (iova & 0xfffu) || (physical & 0xfffu) ||
        iova > 0x0000ffffffffffffull ||
        (physical & ~VTD_ENTRY_ADDRESS) != 0 ||
        (domain->unit->host_address_width < 64 &&
         (physical >> domain->unit->host_address_width) != 0))
        return KIRA_VTD_INVALID;

    rc = vtd_install_leaf(domain, iova, physical, permissions, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    vtd_barrier(platform);
    return vtd_invalidate_context(domain->unit, platform);
}

int kira_vtd_domain_unmap_4k(struct kira_vtd_domain *domain,
                             uint64_t iova,
                             const struct kira_vtd_platform *platform)
{
    uint64_t *table;
    unsigned int shifts[4] = {39, 30, 21, 12};
    unsigned int level;

    if (!domain || !domain->active || !platform ||
        (iova & 0xfffu) || iova > 0x0000ffffffffffffull)
        return KIRA_VTD_INVALID;

    table = vtd_page(domain->second_level);
    for (level = 0; level < 3; ++level) {
        unsigned int index = (unsigned int)((iova >> shifts[level]) & 0x1ffu);
        uint64_t entry = table[index];
        if (!(entry & VTD_ENTRY_PRESENT) || (entry & VTD_ENTRY_LARGE))
            return KIRA_VTD_HARDWARE_ERROR;
        table = (uint64_t *)platform->map_mmio(platform->context,
                    entry & VTD_ENTRY_ADDRESS, KIRA_VTD_PAGE_SIZE);
        if (!table)
            return KIRA_VTD_MEMORY_FAILED;
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        if (!(table[index] & VTD_ENTRY_PRESENT))
            return KIRA_VTD_HARDWARE_ERROR;
        table[index] = 0;
        if (platform->clean_cache)
            platform->clean_cache(platform->context, table,
                                  KIRA_VTD_PAGE_SIZE);
    }

    vtd_barrier(platform);
    return vtd_invalidate_context(domain->unit, platform);
}
