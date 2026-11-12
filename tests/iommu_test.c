/*
 * VT-d end to end on the host: iommu.c (DMAR -> contexts -> translation on -> domains -> maps -> faults)
 * against a model of the remapping hardware. The model owns the register file and implements what
 * decides isolation: the root/context walk with translation types, a 4-level second-level walk with
 * separate read and write permission, an IOTLB that only an invalidation command empties, and the fault
 * recording registers with their write-1-to-clear semantics.
 * One scenario per process (iommu.c keeps global state): iommu_test <A|B|C|D|E>.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "iommu.h"
#include "iommu_vtd.h"
#include "acpi.h"
#include "pci.h"

#define CHECK(x) do { if (!(x)) { printf("iommu_test FAIL:%d %s\n", __LINE__, #x); printf("---- log ----\n%s\n", logbuf); return 1; } } while (0)

/* ---- captured kernel log ---- */
static char logbuf[1 << 16];
void uart_print(const char* s) { size_t n = strlen(logbuf); size_t l = strlen(s); if (n + l < sizeof logbuf - 1) memcpy(logbuf + n, s, l + 1); }
void uart_print_hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); uart_print(b); }
void uart_print_dec(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); uart_print(b); }
static int logged(const char* s) { return strstr(logbuf, s) != 0; }

/* ---- modelled PCI bus ---- */
typedef struct { pci_dev_t d; uint8_t sec, sub; } mdev_t;
static mdev_t mdev[8];
static int nmdev;
static void add_dev(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t ven, uint16_t id, uint8_t hdr, uint8_t cls, uint8_t sub, uint8_t sec, uint8_t subo) {
    pci_dev_t* d = &mdev[nmdev].d;
    memset(d, 0, sizeof *d);
    d->bus = bus; d->dev = dev; d->fn = fn; d->vendor = ven; d->device = id; d->header_type = hdr; d->class_code = cls; d->subclass = sub;
    mdev[nmdev].sec = sec; mdev[nmdev].sub = subo; nmdev++;
}
int pci_count(void) { return nmdev; }
const pci_dev_t* pci_get(int i) { return i >= 0 && i < nmdev ? &mdev[i].d : 0; }
uint8_t pci_read8(const pci_dev_t* d, uint16_t off) {
    for (int i = 0; i < nmdev; i++) if (&mdev[i].d == d) return off == 0x19 ? mdev[i].sec : off == 0x1A ? mdev[i].sub : 0;
    return 0;
}

/* ---- ACPI + DMAR ---- */
static acpi_info_t ai;
const acpi_info_t* acpi_get(void) { return &ai; }
static uint8_t dmar[512];
static uint32_t dmar_len;
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void fix(void) { dmar[9] = 0; uint8_t s = 0; for (uint32_t i = 0; i < dmar_len; i++) s = (uint8_t)(s + dmar[i]); dmar[9] = (uint8_t)(0 - s); }
static uint32_t scope(uint32_t at, uint8_t type, uint8_t start_bus, int n, const uint8_t* path) {
    uint8_t* s = dmar + at; s[0] = type; s[1] = (uint8_t)(6 + 2 * n); s[5] = start_bus;
    for (int i = 0; i < 2 * n; i++) s[6 + i] = path[i];
    return at + 6 + 2 * n;
}
#define REGS_BASE 0xFED90000ull
static void build_dmar(char scenario) {
    memset(dmar, 0, sizeof dmar);
    memcpy(dmar, "DMAR", 4); dmar[8] = 1; dmar[36] = 47; dmar[37] = 1;
    uint32_t at = 48;
    uint32_t drhd = at;
    w16(dmar + at, 0); dmar[at + 4] = scenario == 'B' ? 0 : 1; w16(dmar + at + 6, 0); w64(dmar + at + 8, REGS_BASE);
    at += 16;
    if (scenario == 'B') {                       /* scope-only unit: 00:03.0 and 01:00.0 (through the bridge at 00:1c.0) */
        const uint8_t p1[2] = {3, 0};
        const uint8_t p2[4] = {0x1c, 0, 0, 0};
        at = scope(at, 1, 0, 1, p1);
        at = scope(at, 1, 0, 2, p2);
    }
    w16(dmar + drhd + 2, (uint16_t)(at - drhd));
    if (scenario == 'A') {                       /* RMRR: firmware DMA region used by 00:02.0 */
        uint32_t r = at;
        w16(dmar + at, 1); w16(dmar + at + 6, 0); w64(dmar + at + 8, 0x7F000000); w64(dmar + at + 16, 0x7F000FFF);
        at += 24;
        const uint8_t p[2] = {2, 0};
        at = scope(at, 1, 0, 1, p);
        w16(dmar + r + 2, (uint16_t)(at - r));
    }
    dmar_len = at; w32(dmar + 4, dmar_len);
    fix();
}

/* ---- remapping hardware model ---- */
#define R_VER 0x00
#define R_CAP 0x08
#define R_ECAP 0x10
#define R_GCMD 0x18
#define R_GSTS 0x1C
#define R_RTADDR 0x20
#define R_CCMD 0x28
#define R_FSTS 0x34
#define IRO 0x100u
#define R_IOTLB (IRO + 8u)
#define FRO 0x220u
#define NFR 4
static uint8_t* regs;
static int hw_has_pt = 1, hw_hang, hw_reject;
static uint64_t root_phys;
static uint32_t fsts_shadow, frhi_shadow[NFR];
static int pfo;
static unsigned fr_next;
static int iotlb_flushes;
static struct { int valid; uint16_t sid; uint64_t page, pa; int r, w; } iotlb[16];
static unsigned live_pages;

static uint32_t* r32(uint32_t o) { return (uint32_t*)(regs + o); }
static uint64_t* r64(uint32_t o) { return (uint64_t*)(regs + o); }
static uint64_t* frec(unsigned i) { return (uint64_t*)(regs + FRO + 16u * i); }

static void hw_refresh_fsts(void) {
    int ppf = 0; unsigned fri = fr_next % NFR;
    for (unsigned i = 0; i < NFR; i++) {
        int set = (frec(i)[1] >> 63) & 1, prev = (frec((i + NFR - 1) % NFR)[1] >> 63) & 1;
        if (set) { if (!ppf || !prev) fri = i; ppf = 1; }          /* FRI: the oldest pending record (start of the run) */
    }
    *r32(R_FSTS) = (uint32_t)(pfo ? 1u : 0u) | (ppf ? 2u : 0u) | (fri << 8);
    fsts_shadow = *r32(R_FSTS);
    for (unsigned i = 0; i < NFR; i++) frhi_shadow[i] = *(uint32_t*)((uint8_t*)frec(i) + 12);
}
static void hw_sync(void) {
    /* writes by the driver, detected by comparing with what the hardware last wrote */
    if (*r32(R_FSTS) != fsts_shadow && (*r32(R_FSTS) & 1u)) pfo = 0;
    for (unsigned i = 0; i < NFR; i++) {
        uint32_t dw = *(uint32_t*)((uint8_t*)frec(i) + 12);
        if (dw != frhi_shadow[i] && (dw & (1u << 31))) frec(i)[1] = 0;      /* write 1 to F clears the record */
    }
    uint32_t g = *r32(R_GCMD), s = *r32(R_GSTS);
    if (g & (1u << 30)) { root_phys = *r64(R_RTADDR) & ~0xFFFull; s |= 1u << 30; *r32(R_GCMD) = g & ~(1u << 30); }
    s = (s & ~(1u << 31)) | (g & (1u << 31));
    *r32(R_GSTS) = s;
    if (!hw_hang) {
        if (*r64(R_CCMD) & (1ull << 63)) *r64(R_CCMD) &= ~(1ull << 63);
        if (*r64(R_IOTLB) & (1ull << 63)) {
            uint64_t v = *r64(R_IOTLB) & ~(1ull << 63);
            v = (v & ~(3ull << 57)) | (hw_reject ? 0 : (1ull << 57));
            *r64(R_IOTLB) = v;
            if (!hw_reject) { memset(iotlb, 0, sizeof iotlb); iotlb_flushes++; }
        }
    }
    hw_refresh_fsts();
}
void host_iommu_barrier(void) { if (regs) hw_sync(); }

volatile void* host_iommu_map_regs(uint64_t phys, uint64_t len) { (void)len; return phys == REGS_BASE ? (volatile void*)regs : 0; }
const void* host_iommu_dmar(uint64_t phys, uint64_t len) { (void)phys; return len == dmar_len ? dmar : 0; }
void* host_iommu_page_alloc(uint64_t* phys) { void* p = aligned_alloc(4096, 4096); if (!p) { *phys = 0; return 0; } memset(p, 0xA5, 4096); *phys = (uint64_t)(uintptr_t)p; live_pages++; return p; }
void host_iommu_page_free(void* p, uint64_t phys) { (void)phys; free(p); live_pages--; }

static void record_fault(uint16_t sid, uint64_t iova, int write, int reason) {
    unsigned i = fr_next % NFR;
    if (frec(i)[1] & (1ull << 63)) { pfo = 1; return; }                    /* all records busy: overflow */
    frec(i)[0] = iova & ~0xFFFull;
    frec(i)[1] = (1ull << 63) | (write ? 0 : (1ull << 62)) | ((uint64_t)reason << 32) | sid;
    fr_next++;
}
/* One DMA access by a device. 0 = allowed (pa filled), -1 = blocked and recorded. */
static int hw_dma(uint16_t sid, uint64_t iova, int write, uint64_t* pa) {
    hw_sync();
    int rc = -1;
    if (!(*r32(R_GSTS) & (1u << 31))) { *pa = iova; return 0; }            /* translation off */
    for (int i = 0; i < 16; i++)
        if (iotlb[i].valid && iotlb[i].sid == sid && iotlb[i].page == (iova >> 12)) {
            if (write ? iotlb[i].w : iotlb[i].r) { *pa = iotlb[i].pa | (iova & 0xFFF); return 0; }
        }
    uint64_t* root = (uint64_t*)(uintptr_t)root_phys;
    uint64_t re = root[sid >> 8];
    if (!(re & 1)) { record_fault(sid, iova, write, 1); goto out; }
    uint64_t* ctx = (uint64_t*)(uintptr_t)(re & ~0xFFFull);
    uint64_t lo = ctx[(sid & 0xFF) * 2], hi = ctx[(sid & 0xFF) * 2 + 1];
    if (!(lo & 1)) { record_fault(sid, iova, write, 2); goto out; }
    unsigned tt = (unsigned)(lo >> 2) & 3;
    if (tt == 2) { *pa = iova; rc = 0; goto out; }
    if (tt != 0 || (hi & 7) != 2) { record_fault(sid, iova, write, 3); goto out; }
    uint64_t* t = (uint64_t*)(uintptr_t)(lo & ~0xFFFull);
    for (int level = 0; level < 4; level++) {
        uint64_t e = t[(iova >> (39 - 9 * level)) & 0x1FF];
        if (!(e & 3)) { record_fault(sid, iova, write, write ? 5 : 6); goto out; }
        if (level == 3) {
            if (write ? !(e & 2) : !(e & 1)) { record_fault(sid, iova, write, write ? 5 : 6); goto out; }
            *pa = (e & 0x000FFFFFFFFFF000ull) | (iova & 0xFFF);
            for (int i = 0; i < 16; i++) if (!iotlb[i].valid) { iotlb[i].valid = 1; iotlb[i].sid = sid; iotlb[i].page = iova >> 12; iotlb[i].pa = e & 0x000FFFFFFFFFF000ull; iotlb[i].r = (int)(e & 1); iotlb[i].w = (int)((e >> 1) & 1); break; }
            rc = 0; goto out;
        }
        t = (uint64_t*)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
    }
out:
    hw_refresh_fsts();
    return rc;
}
static uint16_t sid_of(const pci_dev_t* d) { return (uint16_t)(d->bus << 8 | d->dev << 3 | d->fn); }
static uint32_t did_of(uint16_t sid) {
    uint64_t* root = (uint64_t*)(uintptr_t)root_phys;
    uint64_t* ctx = (uint64_t*)(uintptr_t)(root[sid >> 8] & ~0xFFFull);
    return (uint32_t)((ctx[(sid & 0xFF) * 2 + 1] >> 8) & 0xFFFF);
}
static void hw_init(void) {
    regs = aligned_alloc(4096, 4096); memset(regs, 0, 4096);
    *r32(R_VER) = 0x10;
    *r64(R_CAP) = (1ull << 10) | (0ull << 0) | ((uint64_t)(FRO / 16) << 24) | ((uint64_t)(NFR - 1) << 40) | (1ull << 54) | (1ull << 55);
    *r64(R_ECAP) = ((uint64_t)(IRO / 16) << 8) | (hw_has_pt ? (1ull << 6) : 0);
    hw_refresh_fsts();
}

/* ---- scenarios ---- */
static const pci_dev_t* dev_at(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (int i = 0; i < nmdev; i++) if (mdev[i].d.bus == bus && mdev[i].d.dev == dev && mdev[i].d.fn == fn) return &mdev[i].d;
    return 0;
}

int main(int argc, char** argv) {
    char sc = argc > 1 ? argv[1][0] : 'A';
    add_dev(0, 0, 0, 0x8086, 0x29c0, 0, 6, 0, 0, 0);
    add_dev(0, 2, 0, 0x8086, 0x10d3, 0, 2, 0, 0, 0);
    add_dev(0, 3, 0, 0x1af4, 0x1042, 0, 1, 0, 0, 0);
    add_dev(0, 0x1c, 0, 0x8086, 0x3a40, 1, 6, 4, 1, 1);
    add_dev(1, 0, 0, 0x10ec, 0x8139, 0, 2, 0, 0, 0);
    if (sc == 'E') hw_has_pt = 0;
    hw_init();
    ai.valid = 1;
    if (sc != 'D') { build_dmar(sc); ai.dmar_phys = 0x7F800000; ai.dmar_len = dmar_len; }
    if (sc == 'C') dmar[60] ^= 0x10;                 /* damaged byte, checksum no longer matches */

    const pci_dev_t* d3 = dev_at(0, 3, 0);
    const pci_dev_t* d2 = dev_at(0, 2, 0);
    uint64_t pa;

    if (sc == 'D') {                                 /* no DMAR: behaviour as before */
        CHECK(iommu_init() == 0);
        CHECK(logged("No DMAR table"));
        CHECK(!iommu_enabled() && iommu_isolate_device(d3) == IOMMU_E_NONE && !iommu_device_isolated(d3));
        CHECK(iommu_map(d3, 0x1000, 0x2000, 0x1000, IOMMU_R) == IOMMU_E_NONE);
        CHECK(!(*r32(R_GSTS) & (1u << 31)));         /* hardware never touched */
        puts("iommu_test D: PASS"); return 0;
    }
    if (sc == 'C') {                                 /* damaged table is rejected, nothing is enabled */
        CHECK(iommu_init() < 0);
        CHECK(logged("DMAR table rejected"));
        CHECK(!iommu_enabled() && iommu_isolate_device(d3) == IOMMU_E_NONE);
        CHECK(!(*r32(R_GSTS) & (1u << 31)) && root_phys == 0);
        puts("iommu_test C: PASS"); return 0;
    }
    if (sc == 'E') {                                 /* unit without pass-through: left off, nothing blocked */
        CHECK(iommu_init() == 0);
        CHECK(logged("no pass-through support"));
        CHECK(!(*r32(R_GSTS) & (1u << 31)));
        CHECK(iommu_isolate_device(d3) == IOMMU_E_NONE);
        puts("iommu_test E: PASS"); return 0;
    }

    CHECK(iommu_init() == 1);
    CHECK(*r32(R_GSTS) & (1u << 31));                /* TE */
    CHECK(logged("VT-d translation enabled on 1 unit"));
    CHECK(logged("0 isolated until a driver asks"));

    if (sc == 'B') {                                 /* scope-limited unit: uncovered devices are reported, not isolated */
        CHECK(logged("00:00.0 8086:29c0 no DMAR unit covers it"));
        CHECK(logged("00:03.0 1af4:1042 passthrough"));
        CHECK(logged("01:00.0 10ec:8139 passthrough"));            /* found through the bridge path 00:1c.0 -> bus 1 */
        CHECK(iommu_isolate_device(dev_at(0, 0, 0)) == IOMMU_E_NOCOVER);
        CHECK(!iommu_device_isolated(dev_at(0, 0, 0)));
        const pci_dev_t* b1 = dev_at(1, 0, 0);
        CHECK(iommu_isolate_device(d3) == IOMMU_OK && iommu_isolate_device(b1) == IOMMU_OK);
        CHECK(did_of(sid_of(d3)) != did_of(sid_of(b1)) && did_of(sid_of(d3)) != 0 && did_of(sid_of(b1)) != 0);
        CHECK(iommu_map(d3, 0x10000, 0x40000000, 0x1000, IOMMU_R | IOMMU_W) == IOMMU_OK);
        CHECK(hw_dma(sid_of(d3), 0x10000, 0, &pa) == 0 && pa == 0x40000000);
        CHECK(hw_dma(sid_of(b1), 0x10000, 0, &pa) != 0);             /* the other domain does not see it */
        puts("iommu_test B: PASS"); return 0;
    }

    /* ---- scenario A: include-all unit, RMRR on 00:02.0 ---- */
    for (int i = 0; i < nmdev; i++) {                /* before isolation every function keeps full access (pass-through context) */
        CHECK(hw_dma(sid_of(&mdev[i].d), 0x123456000ull, 1, &pa) == 0 && pa == 0x123456000ull);
        CHECK(!iommu_device_isolated(&mdev[i].d));
    }
    CHECK(logged("00:03.0 1af4:1042 passthrough: unrestricted DMA, NOT isolated"));
    CHECK(logged("00:02.0 8086:10d3 passthrough: unrestricted DMA, NOT isolated (RMRR 0x7F000000-0x7F000FFF"));
    CHECK(logged("1 of them RMRR"));
    CHECK(iommu_isolate_device(d2) == IOMMU_E_RMRR && !iommu_device_isolated(d2));      /* RMRR device stays passthrough */
    CHECK(hw_dma(sid_of(d2), 0x7F000000, 1, &pa) == 0);
    CHECK(logged("NOT isolated: named by RMRR"));
    CHECK(iommu_map(d3, 0x1000, 0x2000, 0x1000, IOMMU_R) == IOMMU_E_NOENT);            /* no domain yet */

    unsigned pages_before = live_pages;
    CHECK(iommu_isolate_device(d3) == IOMMU_OK && iommu_isolate_device(d3) == IOMMU_OK);
    CHECK(iommu_device_isolated(d3) && !iommu_device_isolated(dev_at(0, 0, 0)));
    CHECK(logged("00:03.0 isolated: own translation domain"));
    CHECK(hw_dma(sid_of(d3), 0x5000, 0, &pa) != 0);                                     /* nothing mapped: everything faults */
    CHECK(hw_dma(sid_of(dev_at(0, 0, 0)), 0x5000, 1, &pa) == 0);                        /* neighbours unaffected */

    /* byte range that starts and ends inside pages: whole pages become visible, no more */
    CHECK(iommu_map(d3, 0x10000100, 0x70000100, 0x2100, IOMMU_R | IOMMU_W) == IOMMU_OK);
    CHECK(hw_dma(sid_of(d3), 0x10000100, 1, &pa) == 0 && pa == 0x70000100);
    CHECK(hw_dma(sid_of(d3), 0x10002FFF, 0, &pa) == 0 && pa == 0x70002FFF);
    CHECK(hw_dma(sid_of(d3), 0x10003000, 0, &pa) != 0 && hw_dma(sid_of(d3), 0x0FFFF000, 0, &pa) != 0);
    CHECK(hw_dma(sid_of(d3), 0x70000100, 0, &pa) != 0);                                 /* the physical address itself is not granted */
    /* permissions are separate */
    CHECK(iommu_map(d3, 0x20000, 0x80000, 0x1000, IOMMU_R) == IOMMU_OK);
    CHECK(hw_dma(sid_of(d3), 0x20000, 0, &pa) == 0 && hw_dma(sid_of(d3), 0x20000, 1, &pa) != 0);
    CHECK(iommu_map(d3, 0x30000, 0x90000, 0x1000, IOMMU_W) == IOMMU_OK);
    CHECK(hw_dma(sid_of(d3), 0x30000, 1, &pa) == 0 && hw_dma(sid_of(d3), 0x30000, 0, &pa) != 0);
    /* argument and overlap checks */
    CHECK(iommu_map(d3, 0x10002000, 0xA0000, 0x1000, IOMMU_R) == IOMMU_E_EXIST);
    CHECK(iommu_map(d3, 0x40000, 0xA0010, 0x100, IOMMU_R) == IOMMU_E_ARG);              /* page offsets differ */
    CHECK(iommu_map(d3, 0x40000, 0xA0000, 0, IOMMU_R) == IOMMU_E_RANGE);
    CHECK(iommu_map(d3, 0x40000, 0xA0000, 0x1000, 0) == IOMMU_E_ARG);
    CHECK(iommu_map(d3, 0x40000, 0xA0000, 0x1000, 8) == IOMMU_E_ARG);
    CHECK(iommu_map(d3, 0x40000, 1ull << 48, 0x1000, IOMMU_R) == IOMMU_E_RANGE);        /* beyond the host address width */
    CHECK(iommu_map(d3, (1ull << 48), 0xA0000, 0x1000, IOMMU_R) == IOMMU_E_RANGE);     /* beyond 48-bit device addresses */
    CHECK(iommu_map(d3, 0xFFFFFFFFFFFFF000ull, 0xA0000, 0x1000, IOMMU_R) == IOMMU_E_RANGE);
    CHECK(hw_dma(sid_of(d3), 0x40000, 0, &pa) != 0);                                    /* refused maps leave nothing behind */

    /* unmap needs the exact range. A failed invalidation keeps the map record
       and backing page pinned until a later retry actually flushes the IOTLB. */
    CHECK(hw_dma(sid_of(d3), 0x20000, 0, &pa) == 0);                                    /* cached in the IOTLB */
    CHECK(iommu_unmap(d3, 0x20000, 0x2000) == IOMMU_E_NOENT);                      /* a different page range is not that mapping */
    int fl = iotlb_flushes;
    hw_hang = 1;
    CHECK(iommu_unmap(d3, 0x20000, 0x1000) == IOMMU_E_HW);
    CHECK(hw_dma(sid_of(d3), 0x20000, 0, &pa) == 0);                                    /* cached DMA is still possible: do not free */
    hw_hang = 0; *r64(R_IOTLB) &= ~(1ull << 63); *r64(R_CCMD) &= ~(1ull << 63); hw_sync();
    CHECK(iommu_unmap(d3, 0x20000, 0x1000) == IOMMU_OK);                               /* retries even though the PTE is already clear */
    CHECK(iotlb_flushes > fl);
    CHECK(hw_dma(sid_of(d3), 0x20000, 0, &pa) != 0);                                    /* stale translation is now gone */
    CHECK(iommu_unmap(d3, 0x20000, 0x1000) == IOMMU_E_NOENT);

    /* Bulk unmap also retains failed ranges instead of dropping their ledger. */
    CHECK(iommu_map(d3, 0x60000, 0xC0000, 0x1000, IOMMU_R | IOMMU_W) == IOMMU_OK);
    CHECK(hw_dma(sid_of(d3), 0x60000, 0, &pa) == 0);
    hw_hang = 1;
    CHECK(iommu_unmap_all(d3) == IOMMU_E_HW);
    CHECK(hw_dma(sid_of(d3), 0x60000, 0, &pa) == 0);
    hw_hang = 0; *r64(R_IOTLB) &= ~(1ull << 63); *r64(R_CCMD) &= ~(1ull << 63); hw_sync();
    CHECK(iommu_unmap_all(d3) == IOMMU_OK);
    CHECK(hw_dma(sid_of(d3), 0x60000, 0, &pa) != 0);

    /* hardware that does not complete invalidation: the map reports it and leaves no access behind */
    hw_hang = 1;
    CHECK(iommu_map(d3, 0x50000, 0xB0000, 0x1000, IOMMU_R | IOMMU_W) == IOMMU_E_HW);
    hw_hang = 0; *r64(R_IOTLB) &= ~(1ull << 63); *r64(R_CCMD) &= ~(1ull << 63); hw_sync();
    CHECK(iommu_unmap(d3, 0x50000, 0x1000) == IOMMU_OK);                                /* retry retained rollback ledger */
    CHECK(hw_dma(sid_of(d3), 0x50000, 0, &pa) != 0);
    CHECK(iommu_map(d3, 0x50000, 0xB0000, 0x1000, IOMMU_R | IOMMU_W) == IOMMU_OK);     /* and the address is usable afterwards */
    CHECK(iommu_unmap(d3, 0x50000, 0x1000) == IOMMU_OK);

    /* table of mappings is bounded */
    unsigned n_ok = 0;
    for (unsigned i = 0; i < 40; i++) if (iommu_map(d3, 0x100000 + 0x2000ull * i, 0x200000 + 0x2000ull * i, 0x1000, IOMMU_R) == IOMMU_OK) n_ok++;
    CHECK(n_ok > 0 && n_ok < 40);

    /* faults: recorded by the hardware, one log line each, counted per device, never fatal */
    iommu_fault_poll();                                                                 /* drain what accumulated */
    uint32_t before = iommu_fault_count(d3);
    memset(logbuf, 0, 1);                                                               /* new log from here */
    for (int i = 0; i < 6; i++) hw_dma(sid_of(d3), 0x900000 + 0x1000ull * i, i & 1, &pa);   /* 6 violations, 4 fault records */
    int got = iommu_fault_poll();
    CHECK(got == NFR);
    CHECK(iommu_fault_count(d3) == before + NFR);
    CHECK(logged("DMA fault: device 00:03.0 read 0x900000: read not permitted"));
    CHECK(logged("DMA fault: device 00:03.0 write 0x901000: write not permitted"));
    hw_sync();
    CHECK(iommu_fault_poll() == 0);                                                     /* records and overflow flag cleared */
    CHECK(hw_dma(sid_of(d3), 0xA00000, 1, &pa) != 0 && iommu_fault_poll() == 1);        /* works again after the overflow */
    for (int i = 0; i < 20; i++) { hw_dma(sid_of(d3), 0xB00000 + 0x1000ull * i, 0, &pa); iommu_fault_poll(); }
    int lines = 0; for (const char* s = logbuf; (s = strstr(s, "DMA fault: device 00:03.0")) != 0; s++) lines++;
    CHECK(lines <= 8 + 2);                                                              /* log is rate limited per device */

    /* a second device gets its own domain id, mappings are per device */
    const pci_dev_t* d0 = dev_at(0, 0, 0);
    CHECK(iommu_isolate_device(d0) == IOMMU_OK);
    CHECK(did_of(sid_of(d0)) != did_of(sid_of(d3)));
    CHECK(hw_dma(sid_of(d0), 0x10000100, 0, &pa) != 0);

    /* drop everything, then back to pass-through; tables are returned */
    CHECK(iommu_unmap_all(d3) == IOMMU_OK);
    CHECK(hw_dma(sid_of(d3), 0x10000100, 0, &pa) != 0);
    CHECK(iommu_release_device(d3) == IOMMU_OK && !iommu_device_isolated(d3));
    CHECK(hw_dma(sid_of(d3), 0xDEAD000, 1, &pa) == 0 && pa == 0xDEAD000);
    CHECK(iommu_release_device(d3) == IOMMU_E_NOENT);
    CHECK(iommu_release_device(d0) == IOMMU_OK);
    CHECK(live_pages == pages_before);                                                  /* no table page leaked */
    CHECK(iommu_isolate_device(d3) == IOMMU_OK && iommu_map(d3, 0x1000, 0x2000, 0x1000, IOMMU_R) == IOMMU_OK);   /* and it can be redone */
    CHECK(hw_dma(sid_of(d3), 0x1000, 0, &pa) == 0 && pa == 0x2000);

    puts("iommu_test A: PASS");
    return 0;
}
