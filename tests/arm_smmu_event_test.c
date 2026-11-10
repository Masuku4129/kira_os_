#include "drivers/iommu/arm_smmu.h"
#include <stdio.h>
#include <string.h>

#define PAGE 4096u
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static _Alignas(PAGE) uint8_t registers_page[2u * PAGE];
static _Alignas(PAGE) uint8_t event_queue[256u * 32u];

struct test_context { unsigned invalidates, reads, writes, events; uint64_t first[4]; };

static void invalidate_cache(void *opaque, const void *address, size_t length)
{
    struct test_context *ctx = opaque;
    (void)address;
    if (length == 32u) ++ctx->invalidates;
}

static void read_barrier(void *opaque)
{
    struct test_context *ctx = opaque;
    (void)ctx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void write_barrier(void *opaque)
{
    struct test_context *ctx = opaque;
    ++ctx->writes;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void event_handler(void *opaque, const uint64_t words[4])
{
    struct test_context *ctx = opaque;
    if (ctx->events == 0)
        memcpy(ctx->first, words, sizeof(ctx->first));
    ++ctx->events;
}

static void reset(struct kira_smmu *smmu, struct kira_smmu_platform *platform,
                  struct test_context *ctx)
{
    memset(registers_page, 0, sizeof(registers_page));
    memset(event_queue, 0, sizeof(event_queue));
    memset(smmu, 0, sizeof(*smmu));
    memset(platform, 0, sizeof(*platform));
    memset(ctx, 0, sizeof(*ctx));
    smmu->registers = registers_page;
    smmu->evtq = event_queue;
    smmu->enabled = 1;
    platform->context = ctx;
    platform->invalidate_cache = invalidate_cache;
    platform->read_barrier = read_barrier;
    platform->write_barrier = write_barrier;
}

int main(void)
{
    struct kira_smmu smmu;
    struct kira_smmu_platform platform;
    struct test_context ctx;
    size_t processed;
    int overflow;
    uint64_t *entry;

    reset(&smmu, &platform, &ctx);
    smmu.evtq_consumer = 0x1feu;
    entry = (uint64_t *)event_queue + 254u * 4u;
    entry[0] = 0xaaaau; entry[1] = 0xbbbbu; entry[2] = 0xccccu; entry[3] = 0xddddu;
    entry = (uint64_t *)event_queue + 255u * 4u;
    entry[0] = 0x1111u;
    entry = (uint64_t *)event_queue;
    entry[0] = 0x2222u;
    entry = (uint64_t *)event_queue + 1u * 4u;
    entry[0] = 0x3333u;
    *(volatile uint32_t *)(registers_page + 0x10a8u) = (1u << 31) | 0x002u;

    CHECK(kira_smmu_v3_poll_events(&smmu, &platform, event_handler, &ctx,
                                   &processed, &overflow) == KIRA_SMMU_OK);
    CHECK(processed == 4u && overflow == 1);
    CHECK(smmu.evtq_consumer == 0x002u && smmu.evtq_overflows == 1u);
    CHECK(*(volatile uint32_t *)(registers_page + 0x10acu) ==
          ((1u << 31) | 0x102u));
    CHECK(ctx.events == 4u && ctx.invalidates == 4u && ctx.writes == 1u);
    CHECK(ctx.first[0] == 0xaaaau && ctx.first[1] == 0xbbbbu &&
          ctx.first[2] == 0xccccu && ctx.first[3] == 0xddddu);

    /* Empty poll is harmless and does not repeat an old overflow indication. */
    *(volatile uint32_t *)(registers_page + 0x10a8u) = 0x002u;
    CHECK(kira_smmu_v3_poll_events(&smmu, &platform, event_handler, &ctx,
                                   &processed, &overflow) == KIRA_SMMU_OK);
    CHECK(processed == 0u && overflow == 0);
    CHECK(ctx.events == 4u && smmu.evtq_overflows == 1u);

    /* Full ring: same index with the opposite wrap bit means 256 pending events. */
    reset(&smmu, &platform, &ctx);
    *(volatile uint32_t *)(registers_page + 0x10a8u) = 0x100u;
    for (unsigned i = 0; i < 256u; ++i)
        ((uint64_t *)event_queue)[i * 4u] = i;
    CHECK(kira_smmu_v3_poll_events(&smmu, &platform, event_handler, &ctx,
                                   &processed, &overflow) == KIRA_SMMU_OK);
    CHECK(processed == 256u && overflow == 0);
    CHECK(ctx.events == 256u && ctx.invalidates == 256u);
    CHECK(smmu.evtq_consumer == 0x100u);

    smmu.enabled = 0;
    CHECK(kira_smmu_v3_poll_events(&smmu, &platform, event_handler, &ctx,
                                   &processed, &overflow) == KIRA_SMMU_INVALID);

    puts("ARM SMMUv3 event queue: wrap, overflow, cache invalidation and drain passed");
    return 0;
}
