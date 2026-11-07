#ifndef KIRA_AARCH64_GICV3_H
#define KIRA_AARCH64_GICV3_H

#include <stddef.h>
#include <stdint.h>

#define KIRA_GIC_MAX_IRQS 1020u
#define KIRA_GIC_SPURIOUS_IRQ 1023u
#define KIRA_GIC_IRQ_EDGE 1u
#define KIRA_GIC_IRQ_LEVEL 0u

struct kira_gicv3_platform {
    void *context;
    volatile void *(*map_device)(void *context, uint64_t physical,
                                 size_t length);
    uint64_t (*read_mpidr)(void *context);
    void (*write_barrier)(void *context);
    void (*read_barrier)(void *context);
    void (*wait_event)(void *context);
};

struct kira_gicv3 {
    volatile uint8_t *gicd;
    volatile uint8_t *gicr;
    uint64_t gicd_physical;
    uint64_t gicr_physical;
    uint32_t irq_count;
    uint32_t redistributor_stride;
    uint8_t initialized;
};

int kira_gicv3_init(struct kira_gicv3 *gic,
                    uint64_t distributor_base,
                    uint64_t redistributor_base,
                    uint32_t redistributor_stride,
                    const struct kira_gicv3_platform *platform);
int kira_gicv3_cpu_init(struct kira_gicv3 *gic,
                        const struct kira_gicv3_platform *platform);
int kira_gicv3_enable_irq(struct kira_gicv3 *gic, uint32_t irq);
int kira_gicv3_disable_irq(struct kira_gicv3 *gic, uint32_t irq);
int kira_gicv3_set_priority(struct kira_gicv3 *gic,
                            uint32_t irq, uint8_t priority);
int kira_gicv3_set_trigger(struct kira_gicv3 *gic,
                           uint32_t irq, uint32_t trigger);
int kira_gicv3_route_irq(struct kira_gicv3 *gic, uint32_t irq,
                          uint64_t target_mpidr);
uint32_t kira_gicv3_acknowledge(void);
void kira_gicv3_end(uint32_t interrupt_id);
void kira_gicv3_send_sgi(uint32_t target_affinity, uint32_t sgi_id);

#endif
