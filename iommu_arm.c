#include "iommu.h"
#include "fdt.h"
#include "pmm.h"
#include "vmm.h"
#include "uart.h"
#include "arm_smmu.h"
#include "arm_smmu_domain.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARM_IOMMU_MAX_MAPS 64u
#define ARM_IOMMU_MAX_DOMAINS 32u
#define ARM_IOMMU_MAX_DOMAIN_MAPS 64u
#define ARM_IOMMU_MMIO_SIZE 0x2000u

struct arm_iommu_map {
    uint32_t rid_base;
    uint32_t sid_base;
    uint32_t length;
    uint32_t mask;
};

struct arm_iommu_mapping {
    uint64_t iova;
    uint64_t physical;
    uint64_t length;
    uint32_t pages;
    unsigned permissions;
    uint8_t used;
};

struct arm_iommu_device {
    uint8_t used;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t stream_id;
    uint32_t faults;
    struct kira_smmu_domain domain;
    struct arm_iommu_mapping mappings[ARM_IOMMU_MAX_DOMAIN_MAPS];
};

static struct kira_smmu g_smmu;
static struct kira_smmu_platform g_platform;
static struct arm_iommu_map g_maps[ARM_IOMMU_MAX_MAPS];
static struct arm_iommu_device g_devices[ARM_IOMMU_MAX_DOMAINS];
static uint32_t g_map_count;
static uint32_t g_smmu_phandle;
static int g_present;
static int g_active;

extern int platform_dma_noncoherent(void);

static volatile void *arm_map_mmio(void *context, uint64_t physical, size_t length)
{
    (void)context;
    return vmm_map_mmio(physical, length, VMM_MMIO_UC);
}

static void *arm_alloc_dma_pages(void *context, size_t pages, uint64_t *physical)
{
    uint64_t pa;
    (void)context;
    if (!physical || !pages || pages > UINT32_MAX)
        return 0;
    pa = pmm_alloc_contiguous((uint32_t)pages);
    if (!pa)
        return 0;
    for (size_t i = 0; i < pages; ++i) {
        uint64_t translated = 0;
        uint64_t page = pa + (uint64_t)i * PMM_PAGE_SIZE;
        if (vmm_translate(page, &translated) != 0 || translated != page) {
            (void)pmm_free_contiguous(pa, (uint32_t)pages);
            return 0;
        }
    }
    *physical = pa;
    return (void *)(uintptr_t)pa;
}

static void arm_free_dma_pages(void *context, void *virtual_address,
                               uint64_t physical, size_t pages)
{
    (void)context;
    (void)virtual_address;
    if (pages && pages <= UINT32_MAX)
        (void)pmm_free_contiguous(physical, (uint32_t)pages);
}

static size_t arm_cache_line_size(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return (size_t)4u << ((ctr >> 16) & 0xfu);
}

static void arm_clean_cache(void *context, const void *address, size_t length)
{
    uintptr_t start, end, p;
    size_t line = arm_cache_line_size();
    (void)context;
    if (!address || !length || !line)
        return;
    start = (uintptr_t)address & ~((uintptr_t)line - 1u);
    end = (uintptr_t)address + length;
    for (p = start; p < end; p += line)
        __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void arm_invalidate_cache(void *context, const void *address, size_t length)
{
    uintptr_t start, end, p;
    size_t line = arm_cache_line_size();
    (void)context;
    if (!address || !length || !line)
        return;
    start = (uintptr_t)address & ~((uintptr_t)line - 1u);
    end = (uintptr_t)address + length;
    for (p = start; p < end; p += line)
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static void arm_write_barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb oshst" ::: "memory");
}

static void arm_read_barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb oshld" ::: "memory");
}

static void arm_delay(void *context)
{
    (void)context;
    __asm__ volatile("yield" ::: "memory");
}

static int arm_quiesce_devices(void *context)
{
    int count = pci_count();
    (void)context;
    for (int i = 0; i < count; ++i) {
        const pci_dev_t *dev = pci_get(i);
        uint16_t command;
        if (!dev)
            continue;
        command = pci_read16(dev, 0x04u);
        if (command & PCI_CMD_MASTER)
            pci_write16(dev, 0x04u, (uint16_t)(command & ~PCI_CMD_MASTER));
    }
    return 0;
}

static struct arm_iommu_device *arm_find_device(const pci_dev_t *dev)
{
    if (!dev)
        return 0;
    for (unsigned i = 0; i < ARM_IOMMU_MAX_DOMAINS; ++i) {
        struct arm_iommu_device *entry = &g_devices[i];
        if (entry->used && entry->bus == dev->bus &&
            entry->device == dev->dev && entry->function == dev->fn)
            return entry;
    }
    return 0;
}

static void arm_report_event(void *context, const uint64_t event[4])
{
    uint32_t sid = (uint32_t)(event[0] >> 32);
    (void)context;
    for (unsigned i = 0; i < ARM_IOMMU_MAX_DOMAINS; ++i)
        if (g_devices[i].used && g_devices[i].stream_id == sid)
            ++g_devices[i].faults;
    uart_print("[KIRA SMMU] event SID=");
    uart_print_dec(sid);
    uart_print(" type=");
    uart_print_hex(event[0] & 0xffu);
    uart_print(" word1=");
    uart_print_hex(event[1]);
    uart_print(" word2=");
    uart_print_hex(event[2]);
    uart_print(" word3=");
    uart_print_hex(event[3]);
    uart_print("\n");
}

static int arm_parse_iommu_maps(const void *fdt)
{
    int depth = 0;
    int node = fdt_root(fdt);
    g_map_count = 0;
    while (node >= 0) {
        uint32_t length = 0;
        const uint8_t *property =
            (const uint8_t *)fdt_prop(fdt, node, "iommu-map", &length);
        if (property) {
            uint32_t mask = 0xffffffffu;
            (void)fdt_prop_u32(fdt, node, "iommu-map-mask", &mask);
            if (length == 0 || (length & 15u) != 0)
                return IOMMU_E_ARG;
            for (uint32_t offset = 0; offset < length; offset += 16u) {
                uint32_t rid = fdt_u32(property + offset);
                uint32_t phandle = fdt_u32(property + offset + 4u);
                uint32_t sid = fdt_u32(property + offset + 8u);
                uint32_t count = fdt_u32(property + offset + 12u);
                if (phandle != g_smmu_phandle)
                    continue;
                rid &= mask;
                if (!count || (uint64_t)rid + count > 0x100000000ull ||
                    (uint64_t)sid + count > 256u)
                    return IOMMU_E_RANGE;
                if (g_map_count >= ARM_IOMMU_MAX_MAPS)
                    return IOMMU_E_NOMEM;
                g_maps[g_map_count].rid_base = rid;
                g_maps[g_map_count].sid_base = sid;
                g_maps[g_map_count].length = count;
                g_maps[g_map_count].mask = mask;
                ++g_map_count;
            }
        }
        node = fdt_next_node(fdt, node, &depth);
    }
    return IOMMU_OK;
}

static int arm_stream_id(const pci_dev_t *dev, uint32_t *stream_id)
{
    uint32_t rid, found = 0, matches = 0;
    if (!dev || !stream_id)
        return IOMMU_E_ARG;
    rid = ((uint32_t)dev->bus << 8) | ((uint32_t)dev->dev << 3) | dev->fn;
    for (uint32_t i = 0; i < g_map_count; ++i) {
        const struct arm_iommu_map *map = &g_maps[i];
        uint32_t masked = rid & map->mask;
        if (masked >= map->rid_base &&
            (uint64_t)masked - map->rid_base < map->length) {
            found = map->sid_base + (masked - map->rid_base);
            ++matches;
        }
    }
    if (matches != 1u || found >= g_smmu.stream_id_limit)
        return IOMMU_E_NOCOVER;
    *stream_id = found;
    return IOMMU_OK;
}

static int arm_smmu_result(int rc)
{
    switch (rc) {
    case KIRA_SMMU_OK: return IOMMU_OK;
    case KIRA_SMMU_MEMORY_FAILED: return IOMMU_E_NOMEM;
    case KIRA_SMMU_UNSUPPORTED: return IOMMU_E_RANGE;
    case KIRA_SMMU_INVALID: return IOMMU_E_ARG;
    default: return IOMMU_E_HW;
    }
}

int iommu_init(void)
{
    return IOMMU_E_ARG; /* ARM64 initialization requires the validated firmware DTB. */
}

int iommu_init_fdt(const void *fdt)
{
    int node, next;
    uint64_t base = 0, size = 0;
    uint32_t cells = 0, phandle = 0;
    int rc;

    if (g_present)
        return IOMMU_E_EXIST;
    if (!fdt || !fdt_valid(fdt, fdt_total_size(fdt)))
        return IOMMU_E_ARG;

    node = fdt_find_compatible(fdt, -1, "arm,smmu-v3");
    if (node < 0 || !fdt_node_enabled(fdt, node))
        return IOMMU_E_NONE;

    g_present = 1;
    if (arm_quiesce_devices(0) != 0)
        return IOMMU_E_HW;
    next = fdt_find_compatible(fdt, node, "arm,smmu-v3");
    if (next >= 0 && fdt_node_enabled(fdt, next))
        return IOMMU_E_HW;
    if (fdt_prop_u32(fdt, node, "#iommu-cells", &cells) != 0 || cells != 1u)
        return IOMMU_E_ARG;
    if (fdt_prop_u32(fdt, node, "phandle", &phandle) != 0 &&
        fdt_prop_u32(fdt, node, "linux,phandle", &phandle) != 0)
        return IOMMU_E_ARG;
    if (fdt_reg(fdt, node, 0, &base, &size) != 0 ||
        size < ARM_IOMMU_MMIO_SIZE || (base & 0xfffu))
        return IOMMU_E_RANGE;
    g_smmu_phandle = phandle;
    rc = arm_parse_iommu_maps(fdt);
    if (rc != IOMMU_OK)
        return rc;

    memset(&g_smmu, 0, sizeof(g_smmu));
    memset(&g_platform, 0, sizeof(g_platform));
    memset(g_devices, 0, sizeof(g_devices));
    g_platform.map_mmio = arm_map_mmio;
    g_platform.alloc_dma_pages = arm_alloc_dma_pages;
    g_platform.free_dma_pages = arm_free_dma_pages;
    g_platform.clean_cache = arm_clean_cache;
    g_platform.invalidate_cache = arm_invalidate_cache;
    g_platform.write_barrier = arm_write_barrier;
    g_platform.read_barrier = arm_read_barrier;
    g_platform.delay = arm_delay;
    g_platform.quiesce_devices = arm_quiesce_devices;
    g_platform.report_event = arm_report_event;

    rc = kira_smmu_v3_prepare(&g_smmu, base, &g_platform);
    if (rc != KIRA_SMMU_OK)
        return arm_smmu_result(rc);
    rc = kira_smmu_v3_enable(&g_smmu, &g_platform);
    if (rc != KIRA_SMMU_OK) {
        (void)kira_smmu_v3_disable(&g_smmu, &g_platform);
        return arm_smmu_result(rc);
    }
    g_active = 1;
    uart_print("[KIRA SMMU] SMMUv3 enabled; unconfigured StreamIDs abort DMA. iommu-map entries=");
    uart_print_dec(g_map_count);
    uart_print("\n");
    return IOMMU_OK;
}

int iommu_enabled(void)
{
    return g_present; /* a discovered SMMU remains a DMA gate if initialization fails */
}

int iommu_isolate_device(const pci_dev_t *dev)
{
    struct arm_iommu_device *entry;
    uint32_t sid;
    int rc;
    if (!dev)
        return IOMMU_E_ARG;
    if (!g_present)
        return IOMMU_E_NONE;
    if (!g_active)
        return IOMMU_E_HW;
    if (arm_find_device(dev))
        return IOMMU_OK;
    rc = arm_stream_id(dev, &sid);
    if (rc != IOMMU_OK)
        return rc;
    for (unsigned i = 0; i < ARM_IOMMU_MAX_DOMAINS; ++i) {
        entry = &g_devices[i];
        if (entry->used)
            continue;
        memset(entry, 0, sizeof(*entry));
        entry->bus = dev->bus;
        entry->device = dev->dev;
        entry->function = dev->fn;
        entry->stream_id = sid;
        rc = kira_smmu_domain_create(&entry->domain, &g_smmu, sid, &g_platform);
        if (rc != KIRA_SMMU_OK) {
            memset(entry, 0, sizeof(*entry));
            return arm_smmu_result(rc);
        }
        entry->used = 1;
        return IOMMU_OK;
    }
    return IOMMU_E_NOMEM;
}

int iommu_release_device(const pci_dev_t *dev)
{
    struct arm_iommu_device *entry = arm_find_device(dev);
    int rc;
    if (!g_present)
        return IOMMU_E_NONE;
    if (!entry)
        return IOMMU_E_NOENT;
    iommu_unmap_all(dev);
    rc = kira_smmu_domain_destroy(&entry->domain, &g_platform);
    if (rc != KIRA_SMMU_OK)
        return arm_smmu_result(rc);
    memset(entry, 0, sizeof(*entry));
    return IOMMU_OK;
}

int iommu_device_isolated(const pci_dev_t *dev)
{
    struct arm_iommu_device *entry;
    if (!g_active || !dev)
        return 0;
    entry = arm_find_device(dev);
    return entry && entry->domain.active;
}

int iommu_map(const pci_dev_t *dev, uint64_t iova, uint64_t physical,
              uint64_t length, unsigned rw)
{
    struct arm_iommu_device *entry;
    struct arm_iommu_mapping *record = 0;
    uint64_t iova_page, physical_page, offset, span, bytes, pages64;
    uint32_t pages, mapped = 0;
    uint32_t permissions;
    int rc;

    if (!g_present)
        return IOMMU_E_NONE;
    if (!g_active)
        return IOMMU_E_HW;
    if (!dev || !length || !(rw & IOMMU_R) ||
        (rw & ~(IOMMU_R | IOMMU_W)) || ((iova ^ physical) & 0xfffu))
        return IOMMU_E_ARG;
    if (iova > UINT64_MAX - length || physical > UINT64_MAX - length)
        return IOMMU_E_RANGE;
    entry = arm_find_device(dev);
    if (!entry || !entry->domain.active)
        return IOMMU_E_NOENT;

    offset = iova & 0xfffu;
    if (length > UINT64_MAX - offset - 4095u)
        return IOMMU_E_RANGE;
    span = offset + length;
    pages64 = (span + 4095u) / 4096u;
    if (!pages64 || pages64 > UINT32_MAX)
        return IOMMU_E_RANGE;
    pages = (uint32_t)pages64;
    bytes = (uint64_t)pages * 4096u;
    iova_page = iova & ~0xfffull;
    physical_page = physical & ~0xfffull;
    if (iova_page >= (1ull << 48) || physical_page >= (1ull << 48) ||
        bytes > (1ull << 48) - iova_page ||
        bytes > (1ull << 48) - physical_page)
        return IOMMU_E_RANGE;

    for (unsigned i = 0; i < ARM_IOMMU_MAX_DOMAIN_MAPS; ++i) {
        struct arm_iommu_mapping *m = &entry->mappings[i];
        if (!m->used) {
            if (!record)
                record = m;
            continue;
        }
        if (iova_page < m->iova + (uint64_t)m->pages * 4096u &&
            m->iova < iova_page + bytes)
            return IOMMU_E_EXIST;
    }
    if (!record)
        return IOMMU_E_NOMEM;

    permissions = KIRA_SMMU_MAP_READ;
    if (rw & IOMMU_W)
        permissions |= KIRA_SMMU_MAP_WRITE;
    for (mapped = 0; mapped < pages; ++mapped) {
        rc = kira_smmu_domain_map_4k(&entry->domain,
                                    iova_page + (uint64_t)mapped * 4096u,
                                    physical_page + (uint64_t)mapped * 4096u,
                                    permissions, &g_platform);
        if (rc != KIRA_SMMU_OK) {
            while (mapped)
                (void)kira_smmu_domain_unmap_4k(&entry->domain,
                    iova_page + (uint64_t)(--mapped) * 4096u, &g_platform);
            return arm_smmu_result(rc);
        }
    }
    record->iova = iova;
    record->physical = physical;
    record->length = length;
    record->pages = pages;
    record->permissions = rw;
    record->used = 1;
    return IOMMU_OK;
}

int iommu_unmap(const pci_dev_t *dev, uint64_t iova, uint64_t length)
{
    struct arm_iommu_device *entry;
    if (!g_present)
        return IOMMU_E_NONE;
    if (!g_active || !dev || !length)
        return IOMMU_E_ARG;
    entry = arm_find_device(dev);
    if (!entry)
        return IOMMU_E_NOENT;
    for (unsigned i = 0; i < ARM_IOMMU_MAX_DOMAIN_MAPS; ++i) {
        struct arm_iommu_mapping *m = &entry->mappings[i];
        uint64_t page;
        if (!m->used || m->iova != iova || m->length != length)
            continue;
        page = m->iova & ~0xfffull;
        for (uint32_t n = 0; n < m->pages; ++n) {
            int rc = kira_smmu_domain_unmap_4k(&entry->domain,
                page + (uint64_t)n * 4096u, &g_platform);
            if (rc != KIRA_SMMU_OK)
                return arm_smmu_result(rc);
        }
        memset(m, 0, sizeof(*m));
        return IOMMU_OK;
    }
    return IOMMU_E_NOENT;
}

void iommu_unmap_all(const pci_dev_t *dev)
{
    struct arm_iommu_device *entry = arm_find_device(dev);
    if (!entry)
        return;
    for (unsigned i = 0; i < ARM_IOMMU_MAX_DOMAIN_MAPS; ++i) {
        struct arm_iommu_mapping *m = &entry->mappings[i];
        if (m->used)
            (void)iommu_unmap(dev, m->iova, m->length);
    }
}

int iommu_fault_poll(void)
{
    uint32_t processed = 0;
    int rc;
    if (!g_active)
        return 0;
    rc = kira_smmu_v3_poll_events(&g_smmu, &g_platform,
                                  g_smmu.queue_entries, &processed);
    return rc == KIRA_SMMU_OK ? (int)processed : IOMMU_E_HW;
}

uint32_t iommu_fault_count(const pci_dev_t *dev)
{
    struct arm_iommu_device *entry = arm_find_device(dev);
    return entry ? entry->faults : 0;
}
