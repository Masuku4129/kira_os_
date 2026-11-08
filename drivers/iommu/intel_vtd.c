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

#define VTD_FSTS_PFO      (1u << 0)
#define VTD_FSTS_PPF      (1u << 1)
#define VTD_FSTS_FRI(v)   (((v) >> 8) & 0xffu)
#define VTD_FRCD_F        (1ull << 63)
#define VTD_FRCD_T_READ   (1ull << 62)
#define VTD_CCMD_ICC      (1ull << 63)
#define VTD_CCMD_GLOBAL   (1ull << 61)
#define VTD_ECAP_IRO(e)   ((unsigned int)(((e) >> 8) & 0x3ffu) * 16u)
#define VTD_CAP_DWD       (1ull << 54)
#define VTD_CAP_DRD       (1ull << 55)
#define VTD_IOTLB_IVT     (1ull << 63)
#define VTD_IOTLB_GLOBAL  (1ull << 60)
#define VTD_IOTLB_DR      (1ull << 49)
#define VTD_IOTLB_DW      (1ull << 48)
#define VTD_IOTLB_IAIG(v) (((v) >> 57) & 3u)

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

/* GCMD reads are not defined to return the state; the persistent command bits
   are taken from GSTS (mask clears the one-shot status bits: RTPS, FLS, WBFS, IRTPS). */
static uint32_t vtd_gcmd_base(struct kira_vtd_unit *unit)
{
    return vtd_read32(unit->registers, VTD_REG_GSTS) & 0x96ffffffu;
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

int kira_vtd_invalidate_iotlb(struct kira_vtd_unit *unit,
                              const struct kira_vtd_platform *platform)
{
    volatile uint64_t *iotlb;
    unsigned int offset;
    unsigned int i;
    uint64_t value;

    if (!unit || !unit->registers || !platform)
        return KIRA_VTD_INVALID;

    /* The IOTLB command register is the second qword of the block at ECAP.IRO.
       Offsets below 0xE0 collide with architectural registers (QEMU puts it at 0xE0). */
    offset = VTD_ECAP_IRO(unit->extended_capabilities);
    if (offset < 0xE0u || offset + 16u > unit->register_size)
        return KIRA_VTD_HARDWARE_ERROR;
    iotlb = (volatile uint64_t *)(unit->registers + offset + 8u);
    value = VTD_IOTLB_IVT | VTD_IOTLB_GLOBAL;
    if (unit->capabilities & VTD_CAP_DRD)
        value |= VTD_IOTLB_DR;
    if (unit->capabilities & VTD_CAP_DWD)
        value |= VTD_IOTLB_DW;
    *iotlb = value;
    if (platform->write_barrier)
        platform->write_barrier(platform->context);
    for (i = 0; i < VTD_POLL_LIMIT; ++i) {
        uint64_t done;
        if (platform->read_barrier)
            platform->read_barrier(platform->context);
        done = *iotlb;
        if ((done & VTD_IOTLB_IVT) == 0)
            /* IAIG == 0: the hardware rejected the request. */
            return VTD_IOTLB_IAIG(done) ? KIRA_VTD_OK : KIRA_VTD_HARDWARE_ERROR;
    }
    return KIRA_VTD_TIMEOUT;
}

int kira_vtd_invalidate_all(struct kira_vtd_unit *unit,
                            const struct kira_vtd_platform *platform)
{
    volatile uint64_t *ccmd;
    unsigned int i;

    if (!unit || !unit->registers || !platform)
        return KIRA_VTD_INVALID;

    ccmd = (volatile uint64_t *)(unit->registers + VTD_REG_CCMD);
    *ccmd = VTD_CCMD_ICC | VTD_CCMD_GLOBAL;
    if (platform->write_barrier)
        platform->write_barrier(platform->context);
    for (i = 0; i < VTD_POLL_LIMIT; ++i) {
        if (platform->read_barrier)
            platform->read_barrier(platform->context);
        if ((*ccmd & VTD_CCMD_ICC) == 0)
            return kira_vtd_invalidate_iotlb(unit, platform);
    }
    return KIRA_VTD_TIMEOUT;
}

const char *kira_vtd_fault_reason(uint8_t reason)
{
    switch (reason) {
    case 0x01: return "root entry not present";
    case 0x02: return "context entry not present";
    case 0x03: return "context entry invalid";
    case 0x04: return "address beyond the supported width";
    case 0x05: return "write not permitted by the page tables";
    case 0x06: return "read not permitted by the page tables";
    case 0x07: return "page-table walk: bad paging-structure address";
    case 0x08: return "root table read failed";
    case 0x09: return "context entry read failed";
    case 0x0a: return "reserved bit set in the context entry";
    case 0x0b: return "translation type blocks the request";
    case 0x0c: return "reserved bit set in the root entry";
    case 0x0d: return "reserved bit set in a paging-structure entry";
    default: return "other reason";
    }
}

int kira_vtd_fault_poll(struct kira_vtd_unit *unit, kira_vtd_fault_fn fn, void *context)
{
    uint32_t status;
    uint32_t index;
    uint32_t seen;
    int consumed = 0;

    if (!unit || !unit->registers || !unit->fault_records ||
        unit->fault_offset < 0x100u ||
        unit->fault_offset + unit->fault_records * 16u > unit->register_size)
        return KIRA_VTD_INVALID;

    status = vtd_read32(unit->registers, VTD_REG_FSTS);
    if (status & VTD_FSTS_PPF) {
        index = VTD_FSTS_FRI(status) % unit->fault_records;
        /* The hardware fills records in order starting at FRI; stop at the
           first record whose F bit is clear. */
        for (seen = 0; seen < unit->fault_records; ++seen) {
            uint32_t at = unit->fault_offset + index * 16u;
            uint64_t low = vtd_read64(unit->registers, at);
            uint64_t high = vtd_read64(unit->registers, at + 8u);
            struct kira_vtd_fault f;

            if (!(high & VTD_FRCD_F))
                break;
            f.source_id = (uint16_t)(high & 0xffffu);
            f.reason = (uint8_t)((high >> 32) & 0xffu);
            f.is_read = (high & VTD_FRCD_T_READ) != 0;
            f.address = low & ~0xfffull;
            if (fn)
                fn(context, &f);
            vtd_write32(unit->registers, at + 12u, 1u << 31);   /* write 1 to F: clear */
            ++consumed;
            index = (index + 1u) % unit->fault_records;
        }
    }
    if (status & (VTD_FSTS_PFO | VTD_FSTS_PPF))
        vtd_write32(unit->registers, VTD_REG_FSTS, VTD_FSTS_PFO);   /* PFO is write-1-to-clear; PPF follows the records */
    return consumed;
}

int kira_vtd_unit_prepare(struct kira_vtd_unit *unit,
                          uint64_t register_base,
                          uint32_t register_bytes,
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
        (register_base & 0xfffu) != 0 ||
        register_bytes < VTD_PAGE_SIZE || (register_bytes & 0xfffu) != 0)
        return KIRA_VTD_INVALID;

    unit->registers = (volatile uint8_t *)
        platform->map_mmio(platform->context, register_base, register_bytes);
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
    if (offset < 0x100u || offset >= register_bytes)
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
    unit->register_size = register_bytes;
    unit->host_address_width = host_address_width;
    unit->queued_invalidation = (ecap & VTD_ECAP_QI) != 0;
    unit->interrupt_remapping = (ecap & VTD_ECAP_IR) != 0;
    unit->translation_enabled = 0;
    unit->next_domain_id = 1;
    unit->fault_offset = offset;
    unit->fault_records = (uint32_t)((cap >> 40) & 0xffu) + 1u;
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

    command = vtd_gcmd_base(unit);
    vtd_write32(unit->registers, VTD_REG_GCMD, command | VTD_GCMD_SRTP);

    rc = vtd_wait_status(unit, VTD_GSTS_RTPS, 1, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    /* VT-d spec 6.6: after the root table pointer changes, the context cache
       and the IOTLB must be invalidated globally before translation is enabled. */
    rc = kira_vtd_invalidate_all(unit, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    command = vtd_gcmd_base(unit);
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

    command = vtd_gcmd_base(unit);
    vtd_write32(unit->registers, VTD_REG_GCMD, command & ~VTD_GCMD_TE);

    rc = vtd_wait_status(unit, VTD_GSTS_TES, 0, platform);
    if (rc != KIRA_VTD_OK)
        return rc;

    unit->translation_enabled = 0;
    return KIRA_VTD_OK;
}
