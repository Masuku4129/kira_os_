/* ARM64 SMMUv3 integration: FDT requester mapping, explicit per-stream bypass,
   optional per-device stage-1 isolation, DMA mapping and event attribution. */
#include "iommu.h"
#include "drivers/iommu/arm_smmu_domain.h"
#include "fdt.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "uart.h"
#include <stddef.h>
#include <stdint.h>

#define ARM_SMMU_REG_BYTES 0x2000u
#define ARM_IOMMU_MAX_MAPS 2048u
#define ARM_IOMMU_MAX_DEVICES 128u
#define ARM_PAGE_SIZE 4096ull
#define ARM_PAGE_MASK (~(ARM_PAGE_SIZE - 1ull))

typedef struct {
    uint64_t iova;
    uint64_t physical;
    uint32_t pages;
    uint32_t mapped_pages;
    uint8_t active;
} arm_iommu_map_t;

typedef struct {
    const pci_dev_t* pci;
    uint32_t stream_id;
    uint8_t covered;
    uint8_t blocked;
    uint8_t quarantine_pending;
    arm_iommu_map_t maps[ARM_IOMMU_MAX_MAPS];
} arm_iommu_device_t;

static struct kira_smmu arm_smmu;
static struct kira_smmu_platform arm_platform;
static struct kira_smmu_domain arm_domains[256];
static arm_iommu_device_t arm_devices[ARM_IOMMU_MAX_DEVICES];
static uint16_t arm_sid_refs[256];
static uint8_t arm_sid_shared[256];
static uint32_t arm_sid_faults[256];
static uint64_t arm_sid_last_event[256][4];
static uint8_t arm_sid_event_pending[256];
static unsigned arm_device_count;
static unsigned arm_domains_created;
static uint8_t arm_map_present;
static uint8_t arm_map_invalid;
static uint8_t arm_smmu_active;
static uint8_t arm_init_done;

static void arm_dsb(void)
{
    __asm__ volatile ("dsb sy" ::: "memory");
}

static volatile void* arm_map_mmio(void* context, uint64_t physical, size_t bytes)
{
    (void)context;
    return (void*)vmm_map_mmio(physical, bytes, VMM_MMIO_UC);
}

static void* arm_alloc_dma_pages(void* context, size_t pages,
                                 uint64_t* physical)
{
    uint64_t pa, translated = 0;
    (void)context;
    if (!physical || !pages || pages > UINT32_MAX)
        return 0;
    *physical = 0;
    pa = pmm_alloc_contiguous_below((uint32_t)pages, 1ull << 48);
    if (!pa)
        return 0;
    /* ARM64 early kernel mappings are identity-mapped RAM. Do not hand an
       SMMU table a physical address that is not also a valid CPU pointer. */
    if (vmm_translate(pa, &translated) != 0 || translated != pa) {
        (void)pmm_free_contiguous(pa, (uint32_t)pages);
        return 0;
    }
    *physical = pa;
    return (void*)(uintptr_t)pa;
}

static void arm_free_dma_pages(void* context, void* virtual_address,
                               uint64_t physical, size_t pages)
{
    (void)context;
    (void)virtual_address;
    if (physical && pages)
        if (pages <= UINT32_MAX)
            (void)pmm_free_contiguous(physical, (uint32_t)pages);
}

static void* arm_alloc_metadata(void* context, size_t bytes)
{
    (void)context;
    return kmalloc(bytes);
}

static void arm_free_metadata(void* context, void* memory)
{
    (void)context;
    kfree(memory);
}

static void arm_clean_cache(void* context, const void* address, size_t bytes)
{
    uintptr_t start = (uintptr_t)address & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)address + bytes;
    (void)context;
    if (!address || !bytes || end < (uintptr_t)address)
        return;
    for (uintptr_t p = start; p < end; p += 64u)
        __asm__ volatile ("dc civac, %0" : : "r"(p) : "memory");
    arm_dsb();
}

static void arm_invalidate_cache(void* context, const void* address,
                                 size_t bytes)
{
    uintptr_t start = (uintptr_t)address & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)address + bytes;
    (void)context;
    if (!address || !bytes || end < (uintptr_t)address)
        return;
    for (uintptr_t p = start; p < end; p += 64u)
        __asm__ volatile ("dc ivac, %0" : : "r"(p) : "memory");
    arm_dsb();
}

static void arm_write_barrier(void* context)
{
    (void)context;
    __asm__ volatile ("dsb oshst" ::: "memory");
}

static void arm_read_barrier(void* context)
{
    (void)context;
    __asm__ volatile ("dsb oshld" ::: "memory");
}

static void arm_delay(void* context)
{
    (void)context;
    __asm__ volatile ("yield" ::: "memory");
}

static int arm_quiesce_devices(void* context)
{
    int failed = 0;
    (void)context;
    for (int i = 0; i < pci_count(); ++i) {
        const pci_dev_t* dev = pci_get(i);
        if (!dev || pci_is_bridge(dev))
            continue; /* Bridge bus mastering forwards downstream traffic. */
        pci_disable_bus_master(dev);
        if (pci_read16(dev, 0x04) & PCI_CMD_MASTER)
            failed = 1;
    }
    return failed ? -1 : 0;
}

static void arm_report_event(void* context, const uint64_t event[4])
{
    uint32_t sid = (uint32_t)(event[0] >> 32);
    uint32_t id = (uint32_t)(event[0] & 0xffu);
    int translation_fault = id >= 0x10u && id <= 0x13u;
    (void)context;

    if (sid >= 256u)
        return;

    /* This callback runs under EVTQ lock: bounded memory operations and
       atomics only. PCI config writes and console output are deferred. */
    for (unsigned i = 0; i < 4u; ++i)
        __atomic_store_n(&arm_sid_last_event[sid][i], event[i], __ATOMIC_RELAXED);
    __atomic_store_n(&arm_sid_event_pending[sid], 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&arm_sid_faults[sid], 1u, __ATOMIC_RELAXED);

    if (translation_fault)
        return;

    /* Malformed STE/CD/stream events quarantine every function sharing SID. */
    for (unsigned i = 0; i < arm_device_count; ++i) {
        arm_iommu_device_t* dev = &arm_devices[i];
        if (dev->covered && dev->stream_id == sid) {
            __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
            __atomic_store_n(&dev->quarantine_pending, 1u, __ATOMIC_RELEASE);
        }
    }
}

static void arm_platform_init(void)
{
    arm_platform.context = 0;
    arm_platform.map_mmio = arm_map_mmio;
    arm_platform.alloc_dma_pages = arm_alloc_dma_pages;
    arm_platform.free_dma_pages = arm_free_dma_pages;
    arm_platform.alloc_metadata = arm_alloc_metadata;
    arm_platform.free_metadata = arm_free_metadata;
    arm_platform.clean_cache = arm_clean_cache;
    arm_platform.invalidate_cache = arm_invalidate_cache;
    arm_platform.write_barrier = arm_write_barrier;
    arm_platform.read_barrier = arm_read_barrier;
    arm_platform.delay = arm_delay;
    arm_platform.quiesce_devices = arm_quiesce_devices;
    arm_platform.report_event = arm_report_event;
}

static arm_iommu_device_t* arm_find_device(const pci_dev_t* pci)
{
    if (!pci)
        return 0;
    for (unsigned i = 0; i < arm_device_count; ++i) {
        const pci_dev_t* current = arm_devices[i].pci;
        if (current && current->bus == pci->bus &&
            current->dev == pci->dev && current->fn == pci->fn)
            return &arm_devices[i];
    }
    return 0;
}

static int arm_map_pages(uint64_t iova, uint64_t physical, uint64_t bytes,
                         uint32_t* pages_out)
{
    uint64_t offset, span, pages;
    if (!bytes || !pages_out || iova > UINT64_MAX - bytes ||
        physical > UINT64_MAX - bytes)
        return IOMMU_E_RANGE;
    offset = iova & (ARM_PAGE_SIZE - 1u);
    if ((physical & (ARM_PAGE_SIZE - 1u)) != offset)
        return IOMMU_E_ARG;
    if (bytes > UINT64_MAX - offset)
        return IOMMU_E_RANGE;
    span = offset + bytes;
    if (span > UINT64_MAX - (ARM_PAGE_SIZE - 1u))
        return IOMMU_E_RANGE;
    pages = (span + ARM_PAGE_SIZE - 1u) / ARM_PAGE_SIZE;
    if (!pages || pages > UINT32_MAX)
        return IOMMU_E_RANGE;
    *pages_out = (uint32_t)pages;
    return IOMMU_OK;
}

static arm_iommu_map_t* arm_free_map_slot(arm_iommu_device_t* dev)
{
    for (unsigned i = 0; i < ARM_IOMMU_MAX_MAPS; ++i)
        if (!dev->maps[i].active)
            return &dev->maps[i];
    return 0;
}

static int arm_has_maps(const arm_iommu_device_t* dev)
{
    for (unsigned i = 0; i < ARM_IOMMU_MAX_MAPS; ++i)
        if (dev->maps[i].active)
            return 1;
    return 0;
}

static void arm_unmap_record(arm_iommu_device_t* dev, arm_iommu_map_t* map)
{
    struct kira_smmu_domain* domain;
    if (!dev || !map || !map->active || dev->stream_id >= 256u)
        return;
    domain = &arm_domains[dev->stream_id];
    for (uint32_t i = map->mapped_pages; i > 0; --i) {
        int rc = kira_smmu_domain_unmap_4k(
            domain, map->iova + (uint64_t)(i - 1u) * ARM_PAGE_SIZE,
            &arm_platform);
        if (rc != KIRA_SMMU_OK) {
            map->mapped_pages = i; /* Remaining mappings are the low prefix. */
            __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
            return;
        }
    }
    map->active = 0;
    map->pages = 0;
    map->mapped_pages = 0;
    map->iova = 0;
    map->physical = 0;
}

int iommu_arm64_init(const void* fdt, int pci_host_node)
{
    uint32_t map_len = 0;
    const void* map_property;
    uint32_t first_phandle = 0;
    int first_smmu_node;
    uint64_t smmu_base = 0, smmu_size = 0;
    unsigned created = 0;

    if (!fdt || pci_host_node < 0)
        return IOMMU_E_ARG;
    if (arm_init_done)
        return arm_smmu_active ? (int)arm_domains_created : 0;
    arm_init_done = 1;

    map_property = fdt_prop(fdt, pci_host_node, "iommu-map", &map_len);
    if (!map_property)
        return 0;
    arm_map_present = 1u;
    /* iommu-map makes this controller's DMA policy authoritative. Quiesce
       endpoints before parsing it, so malformed or unsupported mappings
       cannot leave firmware-enabled bus mastering running. */
    if (arm_quiesce_devices(0) != 0) {
        arm_map_invalid = 1u;
        uart_print("[KIRA SMMU] cannot quiesce every PCI endpoint; DMA remains blocked.\\n");
        return IOMMU_E_HW;
    }
    if (map_len < 16u || (map_len & 3u)) {
        arm_map_invalid = 1u;
        uart_print("[KIRA SMMU] malformed PCI iommu-map; bus mastering blocked for safety.\n");
        return IOMMU_E_ARG;
    }

    if (pci_count() < 0 || (unsigned)pci_count() > ARM_IOMMU_MAX_DEVICES) {
        arm_map_invalid = 1u;
        uart_print("[KIRA SMMU] PCI function count exceeds the bounded requester table; bus mastering blocked.\n");
        return IOMMU_E_RANGE;
    }
    arm_device_count = (unsigned)pci_count();
    for (unsigned i = 0; i < arm_device_count; ++i) {
        const pci_dev_t* pci = pci_get((int)i);
        uint32_t phandle = 0, sid = 0;
        uint32_t rid;
        int rc;
        arm_devices[i].pci = pci;
        if (!pci)
            continue;
        rid = ((uint32_t)pci->bus << 8) |
              ((uint32_t)pci->dev << 3) | (uint32_t)pci->fn;
        rc = fdt_iommu_map(fdt, pci_host_node, rid, &phandle, &sid);
        if (rc == -1)
            continue;
        if (rc != 0 || sid >= 256u) {
            arm_map_invalid = 1u;
            uart_print("[KIRA SMMU] invalid requester mapping; bus mastering blocked.\n");
            return IOMMU_E_ARG;
        }
        arm_devices[i].covered = 1u;
        arm_devices[i].stream_id = sid;
        if (!first_phandle) {
            first_phandle = phandle;
        } else if (phandle != first_phandle) {
            /* This implementation owns one SMMUv3 instance. Do not silently
               attach a stream from another controller to the first instance. */
            arm_map_invalid = 1u;
            uart_print("[KIRA SMMU] multiple SMMUv3 targets in iommu-map are unsupported; all endpoints remain blocked.\\n");
            return IOMMU_E_UNSUPPORTED;
        }
        if (arm_sid_refs[sid] != UINT16_MAX)
            ++arm_sid_refs[sid];
        if (arm_sid_refs[sid] > 1u)
            arm_sid_shared[sid] = 1u;
    }

    if (!first_phandle)
        return 0; /* iommu-map exists, but no enumerated requester is covered. */

    first_smmu_node = fdt_phandle_node(fdt, first_phandle);
    if (first_smmu_node < 0 ||
        !fdt_compatible(fdt, first_smmu_node, "arm,smmu-v3") ||
        fdt_reg(fdt, first_smmu_node, 0, &smmu_base, &smmu_size) != 0 ||
        smmu_size < ARM_SMMU_REG_BYTES) {
        arm_map_invalid = 1u;
        uart_print("[KIRA SMMU] iommu-map target is not an enabled SMMUv3 node or has no register range.\n");
        return IOMMU_E_ARG;
    }

    arm_platform_init();
    int rc = kira_smmu_v3_prepare(&arm_smmu, smmu_base, &arm_platform);
    if (rc != KIRA_SMMU_OK) {
        uart_print("[KIRA SMMU] prepare failed: ");
        uart_print_dec((uint64_t)(-rc));
        uart_print("; covered devices remain bus-master blocked.\n");
        return IOMMU_E_HW;
    }
    rc = kira_smmu_v3_enable(&arm_smmu, &arm_platform);
    if (rc != KIRA_SMMU_OK) {
        (void)kira_smmu_v3_disable(&arm_smmu, &arm_platform);
        uart_print("[KIRA SMMU] enable failed; covered devices remain blocked.\n");
        return IOMMU_E_HW;
    }
    arm_smmu_active = 1u;

    for (uint32_t sid = 0; sid < 256u; ++sid) {
        if (!arm_sid_refs[sid])
            continue;
        rc = kira_smmu_domain_passthrough(&arm_domains[sid], &arm_smmu,
                                          sid, &arm_platform);
        if (rc == KIRA_SMMU_OK) {
            ++created;
            continue;
        }
        for (unsigned i = 0; i < arm_device_count; ++i)
            if (arm_devices[i].covered && arm_devices[i].stream_id == sid)
                arm_devices[i].blocked = 1u;
    }

    arm_domains_created = created;
    uart_print("[KIRA SMMU] SMMUv3 active; explicit per-stream bypass domains: ");
    uart_print_dec(created);
    uart_print(", isolated devices will receive private stage-1 mappings.\n");
    return (int)created;
}

int iommu_init(void)
{
    return arm_smmu_active ? 1 : 0;
}

int iommu_enabled(void)
{
    return arm_smmu_active ? 1 : 0;
}

int iommu_device_ready(const pci_dev_t* pci)
{
    arm_iommu_device_t* dev;
    if (!pci || __atomic_load_n(&pci->dma_quarantined, __ATOMIC_ACQUIRE) || arm_map_invalid)
        return 0;
    if (!arm_map_present)
        return 1; /* No SMMU mapping property: preserve platform legacy behavior. */
    dev = arm_find_device(pci);
    if (!dev || !dev->covered)
        return 0; /* iommu-map is authoritative: unknown requester stays quiesced. */
    if (__atomic_load_n(&dev->blocked, __ATOMIC_ACQUIRE) ||
        !arm_smmu_active || dev->stream_id >= 256u ||
        arm_sid_shared[dev->stream_id])
        return 0; /* Shared SID cannot be isolated per function; fail closed. */
    /* A valid per-stream bypass STE is only a quarantine/transition state,
       not DMA isolation. Never grant PCI bus mastering until the private
       stage-1 domain has replaced it successfully. */
    return arm_domains[dev->stream_id].active != 0 &&
           arm_domains[dev->stream_id].bypass == 0;
}

int iommu_device_isolated(const pci_dev_t* pci)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    if (!pci || __atomic_load_n(&pci->dma_quarantined, __ATOMIC_ACQUIRE) || !dev || !dev->covered ||
        __atomic_load_n(&dev->blocked, __ATOMIC_ACQUIRE) || dev->stream_id >= 256u ||
        arm_sid_shared[dev->stream_id])
        return 0;
    return arm_domains[dev->stream_id].active &&
           !arm_domains[dev->stream_id].bypass;
}

int iommu_isolate_device(const pci_dev_t* pci)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    struct kira_smmu_domain* domain;
    int rc;

    if (!pci)
        return IOMMU_E_ARG;
    if (__atomic_load_n(&pci->dma_quarantined, __ATOMIC_ACQUIRE))
        return IOMMU_E_HW;
    if (arm_map_invalid)
        return IOMMU_E_HW;
    if (!dev || !dev->covered)
        return IOMMU_E_NOCOVER;
    if (__atomic_load_n(&dev->blocked, __ATOMIC_ACQUIRE) ||
        !arm_smmu_active || dev->stream_id >= 256u)
        return IOMMU_E_HW;
    if (arm_sid_shared[dev->stream_id])
        return IOMMU_E_EXIST;
    domain = &arm_domains[dev->stream_id];
    if (!domain->active)
        return IOMMU_E_HW;
    if (!domain->bypass)
        return IOMMU_OK;
    if (arm_has_maps(dev))
        return IOMMU_E_EXIST;

    rc = kira_smmu_domain_destroy(domain, &arm_platform);
    if (rc != KIRA_SMMU_OK) {
        __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
        return IOMMU_E_HW;
    }
    rc = kira_smmu_domain_create(domain, &arm_smmu, dev->stream_id,
                                 &arm_platform);
    if (rc == KIRA_SMMU_OK)
        return IOMMU_OK;

    /* Restore explicit bypass so a failed isolation attempt never leaves an
       unowned valid STE. If restoration also fails, block bus mastering. */
    if (kira_smmu_domain_passthrough(domain, &arm_smmu, dev->stream_id,
                                     &arm_platform) != KIRA_SMMU_OK)
        __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
    return rc == KIRA_SMMU_MEMORY_FAILED ? IOMMU_E_NOMEM : IOMMU_E_HW;
}

int iommu_release_device(const pci_dev_t* pci)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    struct kira_smmu_domain* domain;
    int rc;

    if (!dev || !dev->covered)
        return IOMMU_E_NOCOVER;
    if (!pci_disable_bus_master(pci))
        return IOMMU_E_HW;
    /* A blocked domain is not eligible for DMA, but teardown must still be
       allowed to retry failed TLBI/unmap operations while the requester is
       stopped and all backing pages remain pinned. */
    if (dev->stream_id >= 256u || !arm_smmu_active ||
        arm_sid_shared[dev->stream_id])
        return IOMMU_E_HW;
    domain = &arm_domains[dev->stream_id];
    if (!domain->active)
        return IOMMU_E_HW;
    if (domain->bypass) {
        /* A bypass domain has no translations to revoke, but stale software
           map records must never be discarded as if teardown had succeeded. */
        if (arm_has_maps(dev))
            return IOMMU_E_HW;
        __atomic_store_n(&dev->quarantine_pending, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&dev->blocked, 0u, __ATOMIC_RELEASE);
        return IOMMU_OK;
    }

    if (iommu_unmap_all(pci) != IOMMU_OK || arm_has_maps(dev))
        return IOMMU_E_HW;
    rc = kira_smmu_domain_destroy(domain, &arm_platform);
    if (rc != KIRA_SMMU_OK) {
        __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
        return IOMMU_E_HW;
    }
    rc = kira_smmu_domain_passthrough(domain, &arm_smmu, dev->stream_id,
                                      &arm_platform);
    if (rc != KIRA_SMMU_OK) {
        __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
        return IOMMU_E_HW;
    }
    /* Domain is fully detached and the stream is back in pass-through. Clear
       only the transient map-failure quarantine; PCI bus mastering stays off. */
    __atomic_store_n(&dev->quarantine_pending, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&dev->blocked, 0u, __ATOMIC_RELEASE);
    return IOMMU_OK;
}

int iommu_map(const pci_dev_t* pci, uint64_t iova, uint64_t physical,
              uint64_t bytes, unsigned rw)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    arm_iommu_map_t* map;
    uint32_t pages;
    uint64_t iova_base, physical_base, new_end;
    uint32_t permissions;
    int rc;

    if (!dev || !dev->covered || !iommu_device_isolated(pci))
        return IOMMU_E_NOCOVER;
    if (!(rw & IOMMU_R) || (rw & ~(IOMMU_R | IOMMU_W)))
        return IOMMU_E_ARG;
    rc = arm_map_pages(iova, physical, bytes, &pages);
    if (rc != IOMMU_OK)
        return rc;
    iova_base = iova & ARM_PAGE_MASK;
    physical_base = physical & ARM_PAGE_MASK;
    if (iova_base >= (1ull << 48) || physical_base >= (1ull << 48) ||
        (uint64_t)pages > ((1ull << 48) - iova_base) / ARM_PAGE_SIZE ||
        (uint64_t)pages > ((1ull << 48) - physical_base) / ARM_PAGE_SIZE)
        return IOMMU_E_RANGE;
    new_end = iova_base + (uint64_t)pages * ARM_PAGE_SIZE;

    for (unsigned i = 0; i < ARM_IOMMU_MAX_MAPS; ++i) {
        arm_iommu_map_t* old = &dev->maps[i];
        uint64_t old_end;
        if (!old->active)
            continue;
        old_end = old->iova + (uint64_t)old->pages * ARM_PAGE_SIZE;
        if (iova_base < old_end && old->iova < new_end)
            return IOMMU_E_EXIST;
    }
    map = arm_free_map_slot(dev);
    if (!map)
        return IOMMU_E_EXIST;

    permissions = KIRA_SMMU_MAP_READ |
                  ((rw & IOMMU_W) ? KIRA_SMMU_MAP_WRITE : 0u);
    for (uint32_t i = 0; i < pages; ++i) {
        rc = kira_smmu_domain_map_4k(&arm_domains[dev->stream_id],
                                    iova_base + (uint64_t)i * ARM_PAGE_SIZE,
                                    physical_base + (uint64_t)i * ARM_PAGE_SIZE,
                                    permissions, &arm_platform);
        if (rc != KIRA_SMMU_OK) {
            uint32_t pinned_pages = i;
            int uncertain = rc == KIRA_SMMU_TIMEOUT ||
                            rc == KIRA_SMMU_HARDWARE_ERROR;
            if (uncertain) {
                /* PTE publication may have succeeded before TLBI timed out.
                   Pin the uncertain page and quarantine DMA until teardown
                   retries invalidation; never let the caller free this memory. */
                pinned_pages = i + 1u;
                __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
                __atomic_store_n(&dev->quarantine_pending, 1u, __ATOMIC_RELEASE);
            }
            if (pinned_pages) {
                map->iova = iova_base;
                map->physical = physical_base;
                map->pages = pinned_pages;
                map->mapped_pages = pinned_pages;
                map->active = 1u;
                if (!uncertain) {
                    /* A definite allocation/validation failure must not leave
                       the successful prefix mapped after returning an error. */
                    arm_unmap_record(dev, map);
                    if (map->active) {
                        __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
                        __atomic_store_n(&dev->quarantine_pending, 1u, __ATOMIC_RELEASE);
                        pci_disable_bus_master(pci);
                    }
                }
            }
            return rc == KIRA_SMMU_MEMORY_FAILED ? IOMMU_E_NOMEM :
                   rc == KIRA_SMMU_INVALID ? IOMMU_E_RANGE : IOMMU_E_HW;
        }
    }

    map->iova = iova_base;
    map->physical = physical_base;
    map->pages = pages;
    map->mapped_pages = pages;
    map->active = 1u;
    return IOMMU_OK;
}

int iommu_unmap(const pci_dev_t* pci, uint64_t iova, uint64_t bytes)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    uint32_t pages;
    uint64_t iova_base;
    int rc;

    if (!dev || !dev->covered || dev->stream_id >= 256u)
        return IOMMU_E_NOCOVER;
    rc = arm_map_pages(iova, iova, bytes, &pages);
    if (rc != IOMMU_OK)
        return rc;
    iova_base = iova & ARM_PAGE_MASK;

    for (unsigned i = 0; i < ARM_IOMMU_MAX_MAPS; ++i) {
        arm_iommu_map_t* map = &dev->maps[i];
        if (!map->active || map->iova != iova_base || map->pages != pages)
            continue;
        arm_unmap_record(dev, map);
        return map->active ? IOMMU_E_HW : IOMMU_OK;
    }
    return IOMMU_E_NOENT;
}

int iommu_unmap_all(const pci_dev_t* pci)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    if (!pci || !dev || !dev->covered)
        return IOMMU_E_NOCOVER;
    if (dev->stream_id >= 256u || !arm_smmu_active ||
        !arm_domains[dev->stream_id].active || arm_domains[dev->stream_id].bypass) {
        /* A bypass/disabled domain cannot revoke translations. Never report a
           successful teardown while software still owns DMA backing pages. */
        if (!arm_has_maps(dev))
            return IOMMU_OK;
        __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&dev->quarantine_pending, 1u, __ATOMIC_RELEASE);
        (void)pci_disable_bus_master(pci);
        return IOMMU_E_HW;
    }
    /* Stop new DMA before revoking mappings or releasing their backing pages. */
    if (!pci_disable_bus_master(pci))
        return IOMMU_E_HW;
    for (unsigned i = 0; i < ARM_IOMMU_MAX_MAPS; ++i) {
        if (!dev->maps[i].active)
            continue;
        arm_unmap_record(dev, &dev->maps[i]);
        if (dev->maps[i].active) {
            __atomic_store_n(&dev->blocked, 1u, __ATOMIC_RELEASE);
            __atomic_store_n(&dev->quarantine_pending, 1u, __ATOMIC_RELEASE);
            return IOMMU_E_HW; /* retain this and every remaining backing allocation */
        }
    }
    return IOMMU_OK;
}

int iommu_fault_poll(void)
{
    uint32_t processed = 0;
    int rc;
    if (!arm_smmu_active)
        return 0;
    rc = kira_smmu_v3_poll_events(&arm_smmu, &arm_platform, 256u,
                                  &processed);
    /* The callback has released EVTQ lock; perform potentially blocking PCI
       config writes and console output only now. */
    for (unsigned i = 0; i < arm_device_count; ++i) {
        arm_iommu_device_t* dev = &arm_devices[i];
        if (__atomic_exchange_n(&dev->quarantine_pending, 0u, __ATOMIC_ACQ_REL) &&
            dev->pci)
            pci_disable_bus_master(dev->pci);
    }
    for (uint32_t sid = 0; sid < 256u; ++sid) {
        if (!__atomic_exchange_n(&arm_sid_event_pending[sid], 0u, __ATOMIC_ACQ_REL))
            continue;
        uint64_t event0 = __atomic_load_n(&arm_sid_last_event[sid][0], __ATOMIC_RELAXED);
        uint64_t event2 = __atomic_load_n(&arm_sid_last_event[sid][2], __ATOMIC_RELAXED);
        uart_print("[KIRA SMMU] event ");
        uart_print_hex((uint32_t)(event0 & 0xffu));
        uart_print(" SID ");
        uart_print_hex(sid);
        uart_print(" IOVA ");
        uart_print_hex(event2);
        uart_print("\n");
    }
    if (rc != KIRA_SMMU_OK)
        uart_print("[KIRA SMMU] event queue reported overflow or hardware error.\n");
    return (int)processed;
}

uint32_t iommu_fault_count(const pci_dev_t* pci)
{
    arm_iommu_device_t* dev = arm_find_device(pci);
    if (!dev || !dev->covered || dev->stream_id >= 256u)
        return 0;
    return __atomic_load_n(&arm_sid_faults[dev->stream_id], __ATOMIC_RELAXED);
}
