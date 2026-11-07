#ifndef KIRA_VTD_TABLES_H
#define KIRA_VTD_TABLES_H

#include <stdint.h>
#include <stddef.h>

/* Intel VT-d legacy root/context tables and four-level second-level paging.
 * The caller owns allocation, DMA visibility, cache maintenance, invalidation,
 * and the final translation-enable sequence. These helpers only encode tables. */

#define KIRA_VTD_PAGE_SIZE 4096u
#define KIRA_VTD_ENTRIES 512u
#define KIRA_VTD_ADDR_MASK 0x000ffffffffff000ull
#define KIRA_VTD_R  (1ull << 0)
#define KIRA_VTD_W  (1ull << 1)
#define KIRA_VTD_SNOOP (1ull << 11)
#define KIRA_VTD_ROOT_PRESENT 1ull
#define KIRA_VTD_CONTEXT_PRESENT 1ull
#define KIRA_VTD_CONTEXT_TT_MULTI 0u

typedef struct __attribute__((aligned(4096))) {
    uint64_t entry[256][2];
} kira_vtd_root_table_t;

typedef struct __attribute__((aligned(4096))) {
    uint64_t lo;
    uint64_t hi;
} kira_vtd_root_entry_t;

typedef struct __attribute__((aligned(4096))) {
    uint64_t entry[256][2];
} kira_vtd_context_table_t;

typedef struct __attribute__((aligned(4096))) {
    uint64_t entry[KIRA_VTD_ENTRIES];
} kira_vtd_pt_t;

static inline int kira_vtd_phys_page_valid(uint64_t p) {
    return p && !(p & (KIRA_VTD_PAGE_SIZE - 1u)) &&
           !(p & ~KIRA_VTD_ADDR_MASK);
}

static inline int kira_vtd_root_set(kira_vtd_root_table_t *root,
                                     uint8_t bus, uint64_t context_phys) {
    if (!root || !kira_vtd_phys_page_valid(context_phys)) return -1;
    root->entry[bus][0] = context_phys | KIRA_VTD_ROOT_PRESENT;\n    root->entry[bus][1] = 0;
    return 0;
}

static inline int kira_vtd_context_set(kira_vtd_context_table_t *table,
                                        uint8_t devfn, uint64_t slpt_phys,
                                        uint16_t domain_id, uint8_t agaw) {
    if (!table || !kira_vtd_phys_page_valid(slpt_phys) ||
        domain_id == 0 || agaw > 2) return -1;
    uint64_t lo = slpt_phys | KIRA_VTD_CONTEXT_PRESENT |
                  (uint64_t)KIRA_VTD_CONTEXT_TT_MULTI << 2;
    uint64_t hi = (uint64_t)agaw | ((uint64_t)domain_id << 8);
    table->entry[devfn][0] = lo;
    table->entry[devfn][1] = hi;
    return 0;
}

static inline unsigned kira_vtd_pt_index(uint64_t iova, unsigned level) {
    return (unsigned)((iova >> (12u + 9u * level)) & 0x1ffu);
}

/* Install a 4 KiB leaf in an already allocated level-1 page table.
 * Upper-level tables must be linked by the caller with R|W permissions. */
static inline int kira_vtd_map_4k(kira_vtd_pt_t *pt, uint64_t iova,
                                  uint64_t phys, int writable, int snoop);

typedef kira_vtd_pt_t *(*kira_vtd_resolve_fn)(void *ctx, uint64_t physical);
typedef kira_vtd_pt_t *(*kira_vtd_alloc_fn)(void *ctx, uint64_t *physical);

typedef struct {
    void *ctx;
    kira_vtd_resolve_fn resolve;
    kira_vtd_alloc_fn alloc;
} kira_vtd_pt_ops_t;

static inline int kira_vtd_link_table(kira_vtd_pt_t *parent,
                                       uint64_t iova, unsigned level,
                                       uint64_t child_phys) {
    if (!parent || level == 0 || level > 3 ||
        !kira_vtd_phys_page_valid(child_phys)) return -1;
    uint64_t *slot = &parent->entry[kira_vtd_pt_index(iova, level)];
    if (*slot & (KIRA_VTD_R | KIRA_VTD_W)) return -2;
    *slot = child_phys | KIRA_VTD_R | KIRA_VTD_W;
    return 0;
}

/* Walk or allocate a complete four-level second-level page-table path.
 * The allocator must return a zeroable, identity-resolvable page and its
 * physical address. The resolver must map previously installed table pages. */
static inline int kira_vtd_map_4k_walk(kira_vtd_pt_t *root,
                                       uint64_t iova, uint64_t phys,
                                       int writable, int snoop,
                                       const kira_vtd_pt_ops_t *ops) {
    if (!root || !ops || !ops->resolve || !ops->alloc ||
        iova >= (1ull << 48) || (iova & 0xfffu) ||
        !kira_vtd_phys_page_valid(phys)) return -1;
    kira_vtd_pt_t *table = root;
    for (unsigned level = 3; level > 0; level--) {
        uint64_t *slot = &table->entry[kira_vtd_pt_index(iova, level)];
        if (*slot & (KIRA_VTD_R | KIRA_VTD_W)) {
            if (*slot & (1ull << 7)) return -3; /* large-page entry: never reinterpret */
            uint64_t child_phys = *slot & KIRA_VTD_ADDR_MASK;
            table = ops->resolve(ops->ctx, child_phys);
            if (!table) return -4;
        } else {
            uint64_t child_phys = 0;
            kira_vtd_pt_t *child = ops->alloc(ops->ctx, &child_phys);
            if (!child || !kira_vtd_phys_page_valid(child_phys)) return -5;
            for (unsigned i = 0; i < KIRA_VTD_ENTRIES; i++) child->entry[i] = 0;
            *slot = child_phys | KIRA_VTD_R | KIRA_VTD_W;
            table = child;
        }
    }
    return kira_vtd_map_4k(table, iova, phys, writable, snoop);
}

static inline int kira_vtd_map_4k(kira_vtd_pt_t *pt, uint64_t iova,
                                  uint64_t phys, int writable, int snoop) {
    if (!pt || (iova & 0xfffu) || !kira_vtd_phys_page_valid(phys))
        return -1;
    uint64_t flags = KIRA_VTD_R | (writable ? KIRA_VTD_W : 0) |
                     (snoop ? KIRA_VTD_SNOOP : 0);
    uint64_t *slot = &pt->entry[kira_vtd_pt_index(iova, 0)];
    if (*slot & (KIRA_VTD_R | KIRA_VTD_W)) return -2;
    *slot = phys | flags;
    return 0;
}

static inline int kira_vtd_unmap_4k(kira_vtd_pt_t *pt, uint64_t iova) {
    if (!pt || (iova & 0xfffu)) return -1;
    uint64_t *slot = &pt->entry[kira_vtd_pt_index(iova, 0)];
    if (!(*slot & (KIRA_VTD_R | KIRA_VTD_W))) return -2;
    *slot = 0;
    return 0;
}

/* Remove a 4 KiB mapping. The caller MUST invalidate the IOTLB and wait
 * for completion before reusing the physical page or its page-table pages. */
static inline int kira_vtd_unmap_4k_walk(kira_vtd_pt_t *root,
                                          uint64_t iova,
                                          kira_vtd_resolve_fn resolve,
                                          void *ctx,
                                          uint64_t *old_phys) {
    if (!root || !resolve || iova >= (1ull << 48) || (iova & 0xfffu))
        return -1;
    kira_vtd_pt_t *table = root;
    for (unsigned level = 3; level > 0; level--) {
        uint64_t entry = table->entry[kira_vtd_pt_index(iova, level)];
        if (!(entry & (KIRA_VTD_R | KIRA_VTD_W))) return -2;
        if (entry & (1ull << 7)) return -3;
        table = resolve(ctx, entry & KIRA_VTD_ADDR_MASK);
        if (!table) return -4;
    }
    uint64_t *slot = &table->entry[kira_vtd_pt_index(iova, 0)];
    if (!(*slot & (KIRA_VTD_R | KIRA_VTD_W))) return -2;
    if (old_phys) *old_phys = *slot & KIRA_VTD_ADDR_MASK;
    *slot = 0;
    __asm__ volatile("" ::: "memory");
    return 0;
}

#endif
