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
#define VTD_ENTRY_ANY     (VTD_ENTRY_READ | VTD_ENTRY_WRITE)   /* second-level entry is in use when R or W is set */
#define VTD_ENTRY_LARGE   (1ull << 7)
#define VTD_ENTRY_ADDRESS 0x000ffffffffff000ull
#define VTD_CTX_TT_SHIFT  2
#define VTD_CTX_TT_MASK   (3ull << VTD_CTX_TT_SHIFT)
#define VTD_CTX_TT_PASS   (2ull << VTD_CTX_TT_SHIFT)
#define VTD_CTX_AW_48     2ull           /* 4-level walk */
#define VTD_CTX_DID_SHIFT 8
#define VTD_CAP_SAGAW_48  (1ull << 10)   /* CAP.SAGAW bit 2 */
#define VTD_ECAP_PT       (1ull << 6)

static uint64_t *vtd_page(void *p)
{
    return (uint64_t *)p;
}

/* Table pages are allocated by this driver; the platform tells how to reach them. */
static void *vtd_table_virt(const struct kira_vtd_platform *p, uint64_t physical)
{
    if (p->phys_to_virt)
        return p->phys_to_virt(p->context, physical);
    return (void *)p->map_mmio(p->context, physical, KIRA_VTD_PAGE_SIZE);
}

static void vtd_barrier(const struct kira_vtd_platform *p)
{
    if (p->write_barrier)
        p->write_barrier(p->context);
}

static void *vtd_alloc_page(const struct kira_vtd_platform *p,
                            uint64_t *physical)
{
    void *v = p->alloc_dma_page(p->context, physical);
    unsigned int i;

    if (!v || !*physical || (*physical & 0xfffu)) {
        if (v && p->free_dma_page)
            p->free_dma_page(p->context, v, *physical);
        *physical = 0;
        return 0;
    }

    for (i = 0; i < 512; ++i)
        vtd_page(v)[i] = 0;
    return v;
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

        if (!(entry & VTD_ENTRY_ANY)) {
            uint64_t next_physical = 0;
            void *next = vtd_alloc_page(p, &next_physical);
            if (!next)
                return KIRA_VTD_MEMORY_FAILED;
            table[index] = (next_physical & VTD_ENTRY_ADDRESS) | VTD_ENTRY_ANY;
            if (p->clean_cache)
                p->clean_cache(p->context, table, KIRA_VTD_PAGE_SIZE);
            table = vtd_page(next);
        } else {
            if (entry & VTD_ENTRY_LARGE)
                return KIRA_VTD_HARDWARE_ERROR;
            table = (uint64_t *)vtd_table_virt(p, entry & VTD_ENTRY_ADDRESS);
            if (!table)
                return KIRA_VTD_MEMORY_FAILED;
        }
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        uint64_t flags = 0;
        if (table[index] & VTD_ENTRY_ANY)
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

/*
 * Locate (allocating the context page of the bus when needed) the 128-bit
 * context entry of one requester. The root entry is linked by the caller only
 * after the entry has been written, so the hardware never sees a half-built path.
 */
static int vtd_context_slot(struct kira_vtd_unit *unit, uint8_t bus,
                            uint8_t device, uint8_t function,
                            const struct kira_vtd_platform *p,
                            void **context, uint64_t *context_physical,
                            int *is_new, uint64_t **entry)
{
    uint64_t *root_entries = vtd_page(unit->root_table);

    if (root_entries[bus] & VTD_ENTRY_PRESENT) {
        *context_physical = root_entries[bus] & VTD_ENTRY_ADDRESS;
        *context = vtd_table_virt(p, *context_physical);
        if (!*context)
            return KIRA_VTD_MEMORY_FAILED;
        *is_new = 0;
    } else {
        *context = vtd_alloc_page(p, context_physical);
        if (!*context)
            return KIRA_VTD_MEMORY_FAILED;
        *is_new = 1;
    }
    *entry = &vtd_page(*context)[(((unsigned int)device << 3) | function) * 2u];
    return KIRA_VTD_OK;
}

static void vtd_context_publish(struct kira_vtd_unit *unit, uint8_t bus,
                                void *context, uint64_t context_physical,
                                int is_new,
                                const struct kira_vtd_platform *p)
{
    if (p->clean_cache)
        p->clean_cache(p->context, context, KIRA_VTD_PAGE_SIZE);
    if (is_new) {
        vtd_page(unit->root_table)[bus] = (context_physical & VTD_ENTRY_ADDRESS) |
                                          VTD_ENTRY_PRESENT;
        if (p->clean_cache)
            p->clean_cache(p->context, unit->root_table, KIRA_VTD_PAGE_SIZE);
    }
    vtd_barrier(p);
}

static void vtd_discard_context(int is_new, void *context,
                                uint64_t context_physical,
                                const struct kira_vtd_platform *p)
{
    if (is_new && p->free_dma_page)
        p->free_dma_page(p->context, context, context_physical);
}

static int vtd_unit_usable(const struct kira_vtd_unit *unit,
                           const struct kira_vtd_platform *p,
                           uint16_t segment, uint8_t device, uint8_t function)
{
    return unit && p && p->alloc_dma_page && (p->phys_to_virt || p->map_mmio) &&
           unit->root_table && unit->root_table_physical &&
           device <= 31 && function <= 7 && segment == 0;
}

int kira_vtd_context_passthrough(struct kira_vtd_unit *unit,
                                 uint16_t segment, uint8_t bus,
                                 uint8_t device, uint8_t function,
                                 const struct kira_vtd_platform *platform)
{
    uint64_t context_physical = 0;
    uint64_t *entry;
    void *context;
    int is_new;
    int rc;

    if (!vtd_unit_usable(unit, platform, segment, device, function))
        return KIRA_VTD_INVALID;
    if (!(unit->extended_capabilities & VTD_ECAP_PT))
        return KIRA_VTD_UNSUPPORTED;

    rc = vtd_context_slot(unit, bus, device, function, platform,
                          &context, &context_physical, &is_new, &entry);
    if (rc != KIRA_VTD_OK)
        return rc;
    if (entry[0] & VTD_ENTRY_PRESENT) {
        int same = (entry[0] & VTD_CTX_TT_MASK) == VTD_CTX_TT_PASS;
        vtd_discard_context(is_new, context, context_physical, platform);
        return same ? KIRA_VTD_OK : KIRA_VTD_HARDWARE_ERROR;
    }

    entry[1] = VTD_CTX_AW_48;
    entry[0] = VTD_CTX_TT_PASS | VTD_ENTRY_PRESENT;
    vtd_context_publish(unit, bus, context, context_physical, is_new, platform);
    if (unit->translation_enabled)
        return kira_vtd_invalidate_all(unit, platform);
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
    uint64_t *entry;
    void *context;
    void *slpt;
    uint32_t did;
    uint32_t did_limit;
    int is_new;
    int rc;

    if (!domain || !vtd_unit_usable(unit, platform, segment, device, function) ||
        !(unit->capabilities & VTD_CAP_SAGAW_48))
        return KIRA_VTD_INVALID;

    /* CAP.ND: 2^(4+2*ND) domain identifiers; ND values above 6 are reserved. */
    did_limit = (unit->capabilities & 7u) > 6u ? 0u :
                (16u << (2u * (unsigned int)(unit->capabilities & 7u)));
    did = unit->next_domain_id ? unit->next_domain_id : 1u;
    if (did >= did_limit)
        return KIRA_VTD_UNSUPPORTED;

    rc = vtd_context_slot(unit, bus, device, function, platform,
                          &context, &context_physical, &is_new, &entry);
    if (rc != KIRA_VTD_OK)
        return rc;
    /* Only an empty slot or a pass-through context may be turned into a domain. */
    if ((entry[0] & VTD_ENTRY_PRESENT) &&
        (entry[0] & VTD_CTX_TT_MASK) != VTD_CTX_TT_PASS) {
        vtd_discard_context(is_new, context, context_physical, platform);
        return KIRA_VTD_HARDWARE_ERROR;
    }

    slpt = vtd_alloc_page(platform, &slpt_physical);
    if (!slpt) {
        vtd_discard_context(is_new, context, context_physical, platform);
        return KIRA_VTD_MEMORY_FAILED;
    }
    if (platform->clean_cache)
        platform->clean_cache(platform->context, slpt, KIRA_VTD_PAGE_SIZE);

    /*
     * 128-bit context entry. Low qword: present, translation type 00
     * (second-level only), SLPTPTR. High qword: address width (2 = 4-level,
     * 48-bit) and the domain identifier. The entry is invalidated in the
     * hardware caches below, after the whole entry is visible.
     */
    entry[0] = 0;                                   /* not present while rewriting */
    entry[1] = VTD_CTX_AW_48 | ((uint64_t)did << VTD_CTX_DID_SHIFT);
    entry[0] = (slpt_physical & VTD_ENTRY_ADDRESS) | VTD_ENTRY_PRESENT;
    vtd_context_publish(unit, bus, context, context_physical, is_new, platform);

    unit->next_domain_id = did + 1u;
    domain->unit = unit;
    domain->root_context = context;
    domain->root_context_physical = context_physical;
    domain->second_level = slpt;
    domain->second_level_physical = slpt_physical;
    domain->id = did;
    domain->segment = segment;
    domain->bus = bus;
    domain->device = device;
    domain->function = function;
    domain->active = 1;

    if (unit->translation_enabled)
        return kira_vtd_invalidate_all(unit, platform);
    return KIRA_VTD_OK;
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
        (physical & ~VTD_ENTRY_ADDRESS) != 0)
        return KIRA_VTD_INVALID;

    rc = vtd_install_leaf(domain, iova, physical, permissions, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    vtd_barrier(platform);
    return kira_vtd_invalidate_iotlb(domain->unit, platform);
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
        if (!(entry & VTD_ENTRY_ANY) || (entry & VTD_ENTRY_LARGE))
            return KIRA_VTD_HARDWARE_ERROR;
        table = (uint64_t *)vtd_table_virt(platform, entry & VTD_ENTRY_ADDRESS);
        if (!table)
            return KIRA_VTD_MEMORY_FAILED;
    }

    {
        unsigned int index = (unsigned int)((iova >> 12) & 0x1ffu);
        /* A previous attempt may have cleared the PTE but timed out waiting
           for IOTLB invalidation. Repeating unmap must still issue the flush. */
        if (table[index] & VTD_ENTRY_ANY) {
            table[index] = 0;
            if (platform->clean_cache)
                platform->clean_cache(platform->context, table,
                                      KIRA_VTD_PAGE_SIZE);
        }
    }

    vtd_barrier(platform);
    return kira_vtd_invalidate_iotlb(domain->unit, platform);
}

/* Free a paging structure and everything below it. Level 3 is the leaf table
   whose entries point at caller-owned memory and are not followed. */
static void vtd_free_tables(uint64_t physical, unsigned int level,
                            const struct kira_vtd_platform *p)
{
    uint64_t *table = (uint64_t *)vtd_table_virt(p, physical);
    unsigned int i;

    if (!table)
        return;
    if (level < 3) {
        for (i = 0; i < 512; ++i)
            if (table[i] & VTD_ENTRY_ANY)
                vtd_free_tables(table[i] & VTD_ENTRY_ADDRESS, level + 1u, p);
    }
    if (p->free_dma_page)
        p->free_dma_page(p->context, table, physical);
}

int kira_vtd_domain_release(struct kira_vtd_domain *domain,
                            const struct kira_vtd_platform *platform)
{
    uint64_t *entry;
    uint64_t root;
    int rc;

    if (!domain || !domain->active || !platform || !domain->unit ||
        !(domain->unit->extended_capabilities & VTD_ECAP_PT))
        return KIRA_VTD_INVALID;

    entry = &vtd_page(domain->root_context)[
        (((unsigned int)domain->device << 3) | domain->function) * 2u];
    root = domain->second_level_physical;
    entry[0] = 0;
    entry[1] = VTD_CTX_AW_48;
    entry[0] = VTD_CTX_TT_PASS | VTD_ENTRY_PRESENT;
    if (platform->clean_cache)
        platform->clean_cache(platform->context, domain->root_context,
                              KIRA_VTD_PAGE_SIZE);
    vtd_barrier(platform);
    rc = kira_vtd_invalidate_all(domain->unit, platform);
    if (rc != KIRA_VTD_OK)
        return rc;                      /* tables stay allocated: the hardware may still walk them */

    domain->active = 0;
    domain->second_level = 0;
    domain->second_level_physical = 0;
    vtd_free_tables(root, 0, platform);
    return KIRA_VTD_OK;
}
