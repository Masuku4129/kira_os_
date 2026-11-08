/*
 * Intel VT-d register-level unit preparation.
 *
 * This code builds a valid root-entry page and performs the architectural
 * TE transition. Device contexts remain absent, so no device is silently
 * granted translated DMA access by this module.
 */

#include "intel_vtd.h"

#define VTD_REG_VER       0x000u
#define VTD_REG_CAP       0x008u
#define VTD_REG_ECAP      0x010u
#define VTD_REG_GCMD      0x018u
#define VTD_REG_GSTS      0x01cu
#define VTD_REG_RTADDR    0x020u
#define VTD_REG_CCMD      0x028u
#define VTD_REG_FSTS      0x034u
#define VTD_REG_FECTL     0x038u
#define VTD_REG_FEDATA    0x03cu
#define VTD_REG_FEADDR    0x040u
#define VTD_REG_FEUADDR   0x044u
#define VTD_REG_IQH       0x080u
#define VTD_REG_IQT       0x088u
#define VTD_REG_IQA       0x090u
#define VTD_REG_IVA       0x0b8u

#define VTD_GCMD_TE       (1u << 31)
#define VTD_GCMD_SRTP     (1u << 30)
#define VTD_GCMD_QIE      (1u << 26)
#define VTD_GCMD_IRE      (1u << 25)
#define VTD_GSTS_TES      (1u << 31)
#define VTD_GSTS_RTPS     (1u << 30)
#define VTD_GSTS_QIES    (1u << 26)
#define VTD_GSTS_IRES    (1u << 25)

#define VTD_CAP_CM        (1ull << 7)
#define VTD_CAP_PSI       (1ull << 39)
#define VTD_ECAP_QI       (1ull << 1)
#define VTD_ECAP_IR       (1ull << 3)

#define VTD_ROOT_PRESENT  1ull
#define VTD_ROOT_ENTRIES  256u
#define VTD_PAGE_SIZE     4096u
#define VTD_POLL_LIMIT    1000000u

static uint32_t vtd_read32(volatile uint8_t *base, uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    return *reg;
}

static uint64_t vtd_read64(volatile uint8_t *base, uint32_t offset)
{
    volatile uint64_t *reg = (volatile uint64_t *)(base + offset);
    return *reg;
}

static void vtd_write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    *reg = value;
}

static void vtd_write64(volatile uint8_t *base, uint32_t offset, uint64_t value)
{
    volatile uint64_t *reg = (volatile uint64_t *)(base + offset);
    *reg = value;
}

static int vtd_wait_status(struct kira_vtd_unit *unit,
                           uint32_t mask,
                           int set,
                           const struct kira_vtd_platform *platform)
{
    uint32_t i;

    for (i = 0; i < VTD_POLL_LIMIT; ++i) {
        uint32_t status;
        if (platform->read_barrier)
            platform->read_barrier(platform->context);
        status = vtd_read32(unit->registers, VTD_REG_GSTS);
        if (!!(status & mask) == !!set)
            return KIRA_VTD_OK;
    }

    return KIRA_VTD_TIMEOUT;
}

static int vtd_select_address_width(uint64_t cap, uint8_t requested)
{
    uint8_t sagaw = (uint8_t)((cap >> 8) & 0x1fu);
    unsigned int levels;

    if (requested <= 39)
        levels = 3;
    else if (requested <= 48)
        levels = 4;
    else if (requested <= 57)
        levels = 5;
    else
        return 0;

    /* SAGAW bit 0..4 encode 30, 39, 48, 57 and 64-bit walks. */
    if (levels == 3)
        return (sagaw & (1u << 1)) != 0;
    if (levels == 4)
        return (sagaw & (1u << 2)) != 0;
    if (levels == 5)
        return (sagaw & (1u << 3)) != 0;
    return 0;
}

int kira_vtd_unit_prepare(struct kira_vtd_unit *unit,
                          uint64_t register_base,
                          uint8_t host_address_width,
                          const struct kira_vtd_platform *platform)
{
    uint32_t version;
    uint32_t offset;
    uint64_t cap;
    uint64_t ecap;
    uint64_t root_physical = 0;
    void *root_virtual;
    unsigned int i;

    if (!unit || !platform || !platform->map_mmio ||
        !platform->alloc_dma_page || register_base == 0 ||
        (register_base & 0xfffu) != 0)
        return KIRA_VTD_INVALID;

    unit->registers = (volatile uint8_t *)
        platform->map_mmio(platform->context, register_base, VTD_PAGE_SIZE);
    if (!unit->registers)
        return KIRA_VTD_MMIO_FAILED;

    version = vtd_read32(unit->registers, VTD_REG_VER);
    cap = vtd_read64(unit->registers, VTD_REG_CAP);
    ecap = vtd_read64(unit->registers, VTD_REG_ECAP);

    /* Version register's low byte is the minor revision, next byte major. */
    if (((version >> 4) & 0xfu) == 0 && ((version >> 8) & 0xffu) == 0)
        return KIRA_VTD_UNSUPPORTED;

    if (!vtd_select_address_width(cap, host_address_width))
        return KIRA_VTD_UNSUPPORTED;

    offset = (uint32_t)(((cap >> 24) & 0x3ffu) * 16u);
    if (offset < 0x100u || offset >= VTD_PAGE_SIZE)
        return KIRA_VTD_HARDWARE_ERROR;

    root_virtual = platform->alloc_dma_page(platform->context, &root_physical);
    if (!root_virtual || (root_physical & 0xfffu) != 0 ||
        root_physical == 0) {
        if (root_virtual && platform->free_dma_page)
            platform->free_dma_page(platform->context, root_virtual,
                                    root_physical);
        return KIRA_VTD_MEMORY_FAILED;
    }

    for (i = 0; i < VTD_PAGE_SIZE / sizeof(uint64_t); ++i)
        ((volatile uint64_t *)root_virtual)[i] = 0;

    unit->physical_base = register_base;
    unit->capabilities = cap;
    unit->extended_capabilities = ecap;
    unit->version = version;
    unit->register_size = VTD_PAGE_SIZE;
    unit->host_address_width = host_address_width;
    unit->queued_invalidation = (ecap & VTD_ECAP_QI) != 0;
    unit->interrupt_remapping = (ecap & VTD_ECAP_IR) != 0;
    unit->translation_enabled = 0;
    unit->root_table = root_virtual;
    unit->root_table_physical = root_physical;

    if (platform->write_barrier)
        platform->write_barrier(platform->context);

    if (platform->log)
        platform->log(platform->context, "VT-d unit prepared", register_base);

    return KIRA_VTD_OK;
}

int kira_vtd_unit_enable_translation(struct kira_vtd_unit *unit,
                                     const struct kira_vtd_platform *platform)
{
    uint32_t command;
    int rc;

    if (!unit || !platform || !unit->registers ||
        !unit->root_table || !unit->root_table_physical)
        return KIRA_VTD_INVALID;

    if (unit->translation_enabled)
        return KIRA_VTD_OK;

    /*
     * Root table address is programmed before SRTP. All entries are zero:
     * devices without explicit context entries remain blocked. On
     * non-coherent platforms the hardware must see the cleared table first.
     */
    if (platform->clean_cache)
        platform->clean_cache(platform->context, unit->root_table,
                              VTD_PAGE_SIZE);
    if (platform->write_barrier)
        platform->write_barrier(platform->context);

    vtd_write64(unit->registers, VTD_REG_RTADDR,
                unit->root_table_physical & ~0xfffull);
    if (platform->write_barrier)
        platform->write_barrier(platform->context);

    command = vtd_read32(unit->registers, VTD_REG_GCMD);
    vtd_write32(unit->registers, VTD_REG_GCMD, command | VTD_GCMD_SRTP);

    rc = vtd_wait_status(unit, VTD_GSTS_RTPS, 1, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    command = vtd_read32(unit->registers, VTD_REG_GCMD);
    vtd_write32(unit->registers, VTD_REG_GCMD, command | VTD_GCMD_TE);

    rc = vtd_wait_status(unit, VTD_GSTS_TES, 1, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    unit->translation_enabled = 1;
    if (platform->log)
        platform->log(platform->context, "VT-d translation enabled",
                      unit->physical_base);

    return KIRA_VTD_OK;
}

int kira_vtd_unit_disable_translation(struct kira_vtd_unit *unit,
                                      const struct kira_vtd_platform *platform)
{
    uint32_t command;
    int rc;

    if (!unit || !platform || !unit->registers)
        return KIRA_VTD_INVALID;

    command = vtd_read32(unit->registers, VTD_REG_GCMD);
    vtd_write32(unit->registers, VTD_REG_GCMD, command & ~VTD_GCMD_TE);

    rc = vtd_wait_status(unit, VTD_GSTS_TES, 0, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    unit->translation_enabled = 0;
    return KIRA_VTD_OK;
}
