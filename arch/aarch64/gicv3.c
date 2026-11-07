/*
 * ARM Generic Interrupt Controller v3 driver.
 *
 * Distributor and redistributor accesses are memory-mapped. CPU interface
 * access uses ICC system registers. The platform must map the GICR region
 * containing the current PE's redistributor frame before CPU initialization.
 */

#include "gicv3.h"

#define GICD_CTLR       0x0000u
#define GICD_TYPER      0x0004u
#define GICD_IGROUPR    0x0080u
#define GICD_ISENABLER  0x0100u
#define GICD_ICENABLER  0x0180u
#define GICD_ICPENDR    0x0280u
#define GICD_IPRIORITYR 0x0400u
#define GICD_ICFGR      0x0c00u
#define GICD_IROUTER    0x6000u
#define GICR_CTLR       0x0000u
#define GICR_TYPER      0x0008u
#define GICR_WAKER      0x0014u
#define GICR_IGROUPR0   0x10080u
#define GICR_ISENABLER0 0x10100u
#define GICR_ICENABLER0 0x10180u
#define GICR_IPRIORITYR 0x10400u
#define GICR_ICFGR0     0x10c00u
#define GICR_SGI_BASE   0x10000u
#define GICR_FRAME_SIZE 0x20000u
#define GICR_WAKER_PROCESSOR_SLEEP (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1u << 2)
#define GICD_CTLR_ENABLE_G1NS (1u << 1)
#define GICD_CTLR_ENABLE_G1S  (1u << 2)

static inline void mmio_write32(volatile uint8_t *base,
                                uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static inline void mmio_write64(volatile uint8_t *base,
                                uint32_t offset, uint64_t value)
{
    *(volatile uint64_t *)(base + offset) = value;
}

static inline uint64_t mmio_read64(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint64_t *)(base + offset);
}

static inline void gic_dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void gic_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline uint64_t read_mpidr(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
    return value;
}

static volatile uint8_t *g_current_gicr;

int kira_gicv3_init(struct kira_gicv3 *gic,
                    uint64_t distributor_base,
                    uint64_t redistributor_base,
                    uint32_t redistributor_stride,
                    const struct kira_gicv3_platform *platform)
{
    uint32_t typer;
    uint32_t lines;

    if (!gic || !platform || !platform->map_device ||
        !distributor_base || !redistributor_base)
        return -1;

    gic->gicd = (volatile uint8_t *)platform->map_device(
        platform->context, distributor_base, 0x10000u);
    gic->gicr = (volatile uint8_t *)platform->map_device(
        platform->context, redistributor_base,
        redistributor_stride ? redistributor_stride : GICR_FRAME_SIZE);
    if (!gic->gicd || !gic->gicr)
        return -2;

    gic->gicd_physical = distributor_base;
    gic->gicr_physical = redistributor_base;
    gic->redistributor_stride = redistributor_stride ?
                                redistributor_stride : GICR_FRAME_SIZE;

    /* Disable the distributor before changing group and routing state. */
    mmio_write32(gic->gicd, GICD_CTLR, 0);
    gic_dsb_sy();

    typer = mmio_read32(gic->gicd, GICD_TYPER);
    lines = ((typer & 0x1fu) + 1u) * 32u;
    if (lines > KIRA_GIC_MAX_IRQS)
        lines = KIRA_GIC_MAX_IRQS;
    gic->irq_count = lines;

    /* All implemented shared interrupts are Group 1 Non-secure. */
    for (uint32_t irq = 32; irq < lines; irq += 32)
        mmio_write32(gic->gicd, GICD_IGROUPR + (irq / 32u) * 4u,
                     0xffffffffu);

    /* Mask every SPI before enabling the distributor. */
    for (uint32_t irq = 32; irq < lines; irq += 32)
        mmio_write32(gic->gicd, GICD_ICENABLER + (irq / 32u) * 4u,
                     0xffffffffu);

    gic_dsb_sy();
    mmio_write32(gic->gicd, GICD_CTLR, GICD_CTLR_ENABLE_G1NS);
    gic_dsb_sy();
    gic_isb();
    gic->initialized = 1;
    return 0;
}

int kira_gicv3_cpu_init(struct kira_gicv3 *gic,
                        const struct kira_gicv3_platform *platform)
{
    volatile uint8_t *rd;
    uint64_t typer;
    uint32_t waker;
    uint32_t timeout = 1000000u;

    if (!gic || !gic->initialized || !platform)
        return -1;

    rd = gic->gicr;
    if (platform->read_mpidr) {
        uint64_t mpidr = platform->read_mpidr(platform->context);
        (void)mpidr; /* Redistributor frame discovery uses affinity below. */
    } else {
        (void)read_mpidr();
    }

    /*
     * Walk redistributor frames until affinity matches this PE. The mapping
     * callback must have mapped the complete redistributor region; a stride
     * of zero selects the architectural 128 KiB frame size.
     */
    for (;;) {
        uint64_t affinity = mmio_read64(rd, GICR_TYPER);
        uint64_t mpidr = platform->read_mpidr ?
            platform->read_mpidr(platform->context) : read_mpidr();
        uint64_t target = ((mpidr >> 32) & 0xffu) << 32 |
                          ((mpidr >> 16) & 0xffu) << 16 |
                          ((mpidr >> 8) & 0xffu) << 8 |
                          (mpidr & 0xffu);
        uint64_t frame_affinity = affinity >> 32;

        if (frame_affinity == target)
            break;
        if (affinity & (1ull << 4))
            return -2; /* Last redistributor frame. */
        rd += gic->redistributor_stride;
    }

    waker = mmio_read32(rd, GICR_WAKER);
    waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
    mmio_write32(rd, GICR_WAKER, waker);
    gic_dsb_sy();

    while (timeout--) {
        waker = mmio_read32(rd, GICR_WAKER);
        if (!(waker & GICR_WAKER_CHILDREN_ASLEEP))
            break;
        if (platform->wait_event)
            platform->wait_event(platform->context);
    }
    if (!timeout)
        return -3;

    /* SGIs and PPIs are Group 1 Non-secure, disabled until explicitly armed. */
    mmio_write32(rd, GICR_IGROUPR0, 0xffffffffu);
    mmio_write32(rd, GICR_ICENABLER0, 0xffffffffu);
    g_current_gicr = rd;

    __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(0xffu) : "memory");
    __asm__ volatile("msr icc_bpr1_el1, %0" :: "r"(0u) : "memory");
    __asm__ volatile("msr icc_igrpen1_el1, %0" :: "r"(1u) : "memory");
    gic_dsb_sy();
    gic_isb();
    return 0;
}

int kira_gicv3_enable_irq(struct kira_gicv3 *gic, uint32_t irq)
{
    volatile uint8_t *base;
    uint32_t offset;
    if (!gic || !gic->initialized || irq >= gic->irq_count)
        return -1;
    if (irq < 32) {
        if (!g_current_gicr)
            return -2;
        base = g_current_gicr;
        offset = GICR_ISENABLER0;
        mmio_write32(base, offset, 1u << irq);
    } else {
        mmio_write32(gic->gicd, GICD_ISENABLER + (irq / 32u) * 4u,
                     1u << (irq % 32u));
    }
    gic_dsb_sy();
    return 0;
}

int kira_gicv3_disable_irq(struct kira_gicv3 *gic, uint32_t irq)
{
    if (!gic || !gic->initialized || irq >= gic->irq_count)
        return -1;
    if (irq < 32) {
        if (!g_current_gicr)
            return -2;
        mmio_write32(g_current_gicr, GICR_ICENABLER0, 1u << irq);
    } else {
        mmio_write32(gic->gicd, GICD_ICENABLER + (irq / 32u) * 4u,
                     1u << (irq % 32u));
    }
    gic_dsb_sy();
    return 0;
}

int kira_gicv3_set_priority(struct kira_gicv3 *gic,
                            uint32_t irq, uint8_t priority)
{
    volatile uint8_t *base;
    uint32_t offset;
    if (!gic || irq >= gic->irq_count)
        return -1;
    if (irq < 32) {
        if (!g_current_gicr)
            return -2;
        base = g_current_gicr;
        offset = GICR_IPRIORITYR;
    } else {
        base = gic->gicd;
        offset = GICD_IPRIORITYR;
    }
    base[offset + irq] = priority;
    gic_dsb_sy();
    return 0;
}

int kira_gicv3_set_trigger(struct kira_gicv3 *gic,
                           uint32_t irq, uint32_t trigger)
{
    volatile uint8_t *base;
    uint32_t offset, bit, value;
    if (!gic || irq < 16 || irq >= gic->irq_count ||
        (trigger != KIRA_GIC_IRQ_EDGE && trigger != KIRA_GIC_IRQ_LEVEL))
        return -1;

    if (irq < 32) {
        if (!g_current_gicr)
            return -2;
        base = g_current_gicr;
        offset = GICR_ICFGR0;
    } else {
        base = gic->gicd;
        offset = GICD_ICFGR;
    }

    bit = (irq % 16u) * 2u + 1u;
    value = mmio_read32(base, offset + (irq / 16u) * 4u);
    if (trigger == KIRA_GIC_IRQ_EDGE)
        value |= 1u << bit;
    else
        value &= ~(1u << bit);
    mmio_write32(base, offset + (irq / 16u) * 4u, value);
    gic_dsb_sy();
    return 0;
}

int kira_gicv3_route_irq(struct kira_gicv3 *gic, uint32_t irq,
                          uint64_t target_mpidr)
{
    uint64_t affinity;
    if (!gic || irq < 32 || irq >= gic->irq_count)
        return -1;

    affinity = (((target_mpidr >> 32) & 0xffu) << 32) |
               (((target_mpidr >> 16) & 0xffu) << 16) |
               (((target_mpidr >> 8) & 0xffu) << 8) |
               (target_mpidr & 0xffu);
    mmio_write64(gic->gicd, GICD_IROUTER + irq * 8u, affinity);
    gic_dsb_sy();
    return 0;
}

uint32_t kira_gicv3_acknowledge(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(value));
    return (uint32_t)(value & 0xffffffu);
}

void kira_gicv3_end(uint32_t interrupt_id)
{
    uint64_t value = interrupt_id;
    __asm__ volatile("msr icc_eoir1_el1, %0" :: "r"(value) : "memory");
    gic_isb();
}

void kira_gicv3_send_sgi(uint32_t target_affinity, uint32_t sgi_id)
{
    uint64_t value;
    if (sgi_id > 15)
        return;
    value = ((uint64_t)(target_affinity & 0xffffu) << 16) |
            ((uint64_t)((target_affinity >> 16) & 0xffu) << 32) |
            ((uint64_t)((target_affinity >> 24) & 0xffu) << 48) |
            ((uint64_t)sgi_id << 24);
    __asm__ volatile("msr icc_sgi1r_el1, %0" :: "r"(value) : "memory");
    gic_dsb_sy();
}
