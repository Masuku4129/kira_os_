/* DMA-изоляция через ARM SMMUv3: все функции iommu.h на ARM64. Таблицу потоков ведёт drivers/iommu/arm_smmu.c (очереди, включение),
   записи потоков, дескриптор контекста, таблицы страниц первой стадии, команды и события - drivers/iommu/smmu_dom.c. Модель как у x86 (iommu.h): сначала все
   устройства без трансляции (честно помечены), драйвер переводит своё устройство в домен с пустой таблицей и дальше получает только
   то, что отображено через iommu_map. Журнал нарушений опрашивается (iommu_fault_poll). Замок S.lock охраняет все таблицы. */
#include "iommu.h"
#include "iommu_smmu.h"
#include "drivers/iommu/arm_smmu.h"
#include "drivers/iommu/smmu_dom.h"
#include "uart.h"
#include "spinlock.h"
#include "pmm.h"
#include "vmm.h"

#define SM_MAX_DOMAINS 16
#define SM_MAX_MAPS 32
#define SM_LOG_PER_DEVICE 8
#define Q_ENTRIES 256u
#define SPINS 20000000u
enum { R_CMDQ_PROD = 0x98, R_CMDQ_CONS = 0x9C, R_EVTQ_PROD = 0x100A8, R_EVTQ_CONS = 0x100AC };

typedef struct { uint64_t iova; uint32_t pages; uint32_t rw; } smap_t;
typedef struct {
    uint8_t used, bus, dev, fn;
    uint32_t sid;
    uint64_t* root; uint64_t root_pa;
    uint64_t* cd; uint64_t cd_pa;                     /* дескриптор контекста: одна страница на домен */
    smap_t maps[SM_MAX_MAPS]; uint32_t nmaps;
    uint32_t faults, logged;
    uint8_t quiet;                                    /* проверка принуждения нарочно вызывает отказ: он считается, но не пишется в журнал */
} sdom_t;

static struct {
    uint64_t base, doorbell; int coherent; unsigned foreign; smmu_rid_map_t maps[SMMU_MAX_RID_MAPS]; unsigned nmaps; int registered;
    int active;
    struct kira_smmu smmu;
    struct kira_smmu_platform plat;
    unsigned oas;
    uint32_t prod;                                   /* 9 бит: индекс (8) и бит оборота */
    sdom_t dom[SM_MAX_DOMAINS];
    spinlock_t lock;
    volatile int polling;
} S;

static void zero(void* p, size_t n) { uint8_t* b = p; while (n--) *b++ = 0; }
static uint32_t rd32(unsigned off) { return *(volatile uint32_t*)(S.smmu.registers + off); }
static void wr32(unsigned off, uint32_t v) { *(volatile uint32_t*)(S.smmu.registers + off) = v; }

/* ---- платформа для arm_smmu.c ---- */
static volatile void* p_map(void* c, uint64_t pa, size_t len) { (void)c; return vmm_map_mmio(pa, len, VMM_MMIO_UC); }
static void* p_alloc(void* c, size_t pages, uint64_t* pa) {
    (void)c;
    uint64_t p = pmm_alloc_contiguous((uint32_t)pages), t;
    *pa = 0;
    if (!p) return 0;
    for (size_t i = 0; i < pages; i++) if (vmm_translate(p + i * 4096, &t) != 0 || t != p + i * 4096) { pmm_free_contiguous(p, (uint32_t)pages); return 0; }
    zero((void*)(uintptr_t)p, pages * 4096);
    *pa = p;
    return (void*)(uintptr_t)p;
}
static void p_free(void* c, void* v, uint64_t pa, size_t pages) { (void)c; (void)v; pmm_free_contiguous(pa, (uint32_t)pages); }
static void p_barrier(void* c) { (void)c; __asm__ volatile ("dsb sy" : : : "memory"); }
static void p_delay(void* c) { (void)c; __asm__ volatile ("yield"); }

/* страницы трансляции: тот же адрес для процессора и обходчика */
static void* t_alloc(void* c, uint64_t* pa) { uint64_t p; void* v = p_alloc(c, 1, &p); *pa = p; return v; }
static void t_free(void* c, void* v, uint64_t pa) { p_free(c, v, pa, 1); }
static void* t_virt(void* c, uint64_t pa) { (void)c; return (void*)(uintptr_t)pa; }
static const smmu_mem_t MEM = { 0, t_alloc, t_free, t_virt };

/* ---- команды ---- */
static int cmd_put(const uint64_t c[2]) {
    for (unsigned spin = 0; ((S.prod ^ rd32(R_CMDQ_CONS)) & 0x1FF) == 0x100; spin++)      /* очередь полна: ждём, пока блок заберёт */
        if (spin > SPINS) return IOMMU_E_HW;
    volatile uint64_t* e = (volatile uint64_t*)S.smmu.cmdq + (S.prod & 0xFF) * 2u;
    e[0] = c[0]; e[1] = c[1];
    __asm__ volatile ("dsb sy" : : : "memory");
    S.prod = (S.prod + 1) & 0x1FF;
    wr32(R_CMDQ_PROD, S.prod);
    return 0;
}
static int cmd_sync(void) {
    uint64_t c[2];
    smmu_cmd_sync(c);
    if (cmd_put(c)) return IOMMU_E_HW;
    for (unsigned spin = 0; spin < SPINS; spin++) { if ((rd32(R_CMDQ_CONS) & 0x1FF) == S.prod) return IOMMU_OK; __asm__ volatile ("yield"); }
    return IOMMU_E_HW;
}
static int inv_stream(uint32_t sid) {                 /* запись потока, его контекст и вся трансляция первой стадии (устройств мало, сбрасываем всё) */
    uint64_t c[2];
    smmu_cmd_cfgi_ste(c, sid);       if (cmd_put(c)) return IOMMU_E_HW;
    smmu_cmd_cfgi_cd_all(c, sid);    if (cmd_put(c)) return IOMMU_E_HW;
    smmu_cmd_tlbi_nh_all(c);         if (cmd_put(c)) return IOMMU_E_HW;
    return cmd_sync();
}
static void ste_write(uint32_t sid, const uint64_t v[SMMU_STE_WORDS]) {
    volatile uint64_t* d = (volatile uint64_t*)S.smmu.stream_table + (size_t)sid * SMMU_STE_WORDS;
    for (int i = 1; i < SMMU_STE_WORDS; i++) d[i] = v[i];
    __asm__ volatile ("dsb sy" : : : "memory");
    d[0] = v[0];                                           /* V и Config - последними */
    __asm__ volatile ("dsb sy" : : : "memory");
}

void iommu_smmu_register(uint64_t base, int coherent, unsigned foreign, const smmu_rid_map_t* maps, unsigned n, uint64_t msi_doorbell) {
    S.base = base; S.doorbell = msi_doorbell; S.coherent = coherent; S.foreign = foreign;
    S.nmaps = n > SMMU_MAX_RID_MAPS ? SMMU_MAX_RID_MAPS : n;
    for (unsigned i = 0; i < S.nmaps; i++) S.maps[i] = maps[i];
    S.registered = base != 0;
}

/* StreamID устройства по iommu-map; -1 - устройство не за этим SMMU */
static int64_t sid_of(const pci_dev_t* d) {
    uint32_t rid = (uint32_t)d->bus << 8 | (uint32_t)d->dev << 3 | d->fn;
    for (unsigned i = 0; i < S.nmaps; i++)
        if (rid >= S.maps[i].rid_base && rid - S.maps[i].rid_base < S.maps[i].len) return (int64_t)S.maps[i].sid_base + (rid - S.maps[i].rid_base);
    return -1;
}

int iommu_enabled(void) { return S.active; }

int iommu_init(void) {
    if (S.active) return 1;
    if (!S.registered) { uart_print("[KIRA IOMMU] No SMMUv3 in the device tree: DMA isolation is not available, devices keep unrestricted DMA.\n"); return 0; }
    if (!S.coherent) { uart_print("[KIRA IOMMU] SMMUv3: DMA is not cache-coherent on this platform, SMMU left off (devices keep unrestricted DMA).\n"); return 0; }
    if (S.foreign) { uart_print("[KIRA IOMMU] SMMUv3: devices other than PCI sit behind it and have no stream entries, SMMU left off.\n"); return 0; }
    S.plat = (struct kira_smmu_platform){ 0, p_map, p_alloc, p_free, 0, 0, p_barrier, p_barrier, p_delay };
    int rc = kira_smmu_v3_prepare(&S.smmu, S.base, &S.plat);
    if (rc) { uart_print("[KIRA IOMMU] SMMUv3 not usable (code "); uart_print_dec((uint64_t)-rc); uart_print("): left off, devices keep unrestricted DMA.\n"); return 0; }
    uint32_t idr0 = S.smmu.idr0, idr5 = S.smmu.idr5;
    S.oas = idr5 & 7u;
    if (!(idr0 & 2u) || !(idr0 & (2u << 2)) || !(idr5 & (1u << 4)) || S.oas > 5) {
        uart_print("[KIRA IOMMU] SMMUv3 without stage-1 translation, AArch64 tables or 4 KB granule (IDR0 "); uart_print_hex(idr0);
        uart_print(", IDR5 "); uart_print_hex(idr5); uart_print("): SMMU left off.\n");
        return 0;
    }
    if (!(idr0 & (1u << 4))) {                                    /* IDR0.COHACC: без него SMMU читает таблицы мимо кэша процессора, а очистки кэша здесь нет */
        uart_print("[KIRA IOMMU] SMMUv3 does not walk tables coherently with the CPU caches (IDR0.COHACC=0): SMMU left off, devices keep unrestricted DMA.\n");
        return 0;
    }
    for (int i = 0; i < pci_count(); i++) {                       /* поток вне таблицы при включённом SMMU отвергается: не включаем, если такой есть */
        int64_t sid = sid_of(pci_get(i));
        if (sid >= (int64_t)S.smmu.stream_id_limit) { uart_print("[KIRA IOMMU] SMMUv3: a PCI device has a StreamID beyond the stream table, SMMU left off.\n"); return 0; }
    }
    uint64_t by[SMMU_STE_WORDS];
    smmu_ste_bypass(by);
    for (uint32_t sid = 0; sid < S.smmu.stream_id_limit; sid++) ste_write(sid, by);    /* все потоки - без трансляции, пока драйвер не попросит */
    rc = kira_smmu_v3_enable(&S.smmu, &S.plat);
    if (rc) { uart_print("[KIRA IOMMU] SMMUv3 did not start (code "); uart_print_dec((uint64_t)-rc); uart_print("): devices keep unrestricted DMA.\n"); return 0; }
    S.prod = 0;
    if (cmd_sync() != IOMMU_OK) { kira_smmu_v3_disable(&S.smmu, &S.plat); uart_print("[KIRA IOMMU] SMMUv3 command queue does not answer: SMMU switched off.\n"); return 0; }
    S.active = 1;
    uart_print("[KIRA IOMMU] SMMUv3 translation enabled (stage 1, 4 KB, output "); uart_print_dec(S.oas == 5 ? 48 : S.oas == 4 ? 44 : S.oas == 3 ? 42 : 32 + 4 * S.oas); uart_print(" bits).\n");
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        uart_print("[KIRA IOMMU] pci "); uart_print_dec(d->bus); uart_print(":"); uart_print_dec(d->dev); uart_print("."); uart_print_dec(d->fn);
        uart_print(sid_of(d) >= 0 ? " passthrough: unrestricted DMA, NOT isolated\n" : " is not behind the SMMU: DMA unrestricted\n");
    }
    return 1;
}

static sdom_t* dom_find(uint32_t sid) {
    for (int i = 0; i < SM_MAX_DOMAINS; i++) if (S.dom[i].used && S.dom[i].sid == sid) return &S.dom[i];
    return 0;
}
static void name(const pci_dev_t* d) {
    uart_print("[KIRA IOMMU] pci "); uart_print_dec(d->bus); uart_print(":"); uart_print_dec(d->dev); uart_print("."); uart_print_dec(d->fn);
}

int iommu_device_isolated(const pci_dev_t* d) {
    if (!d || !S.active) return 0;
    int64_t sid = sid_of(d);
    if (sid < 0) return 0;
    uint64_t f = spin_lock_irqsave(&S.lock);
    int r = dom_find((uint32_t)sid) != 0;
    spin_unlock_irqrestore(&S.lock, f);
    return r;
}

int iommu_isolate_device(const pci_dev_t* d) {
    if (!d) return IOMMU_E_ARG;
    if (!S.active) return IOMMU_E_NONE;
    int64_t sid = sid_of(d);
    int rc = IOMMU_OK;
    uint64_t f = spin_lock_irqsave(&S.lock);
    if (sid >= 0 && dom_find((uint32_t)sid)) { spin_unlock_irqrestore(&S.lock, f); return IOMMU_OK; }
    if (sid < 0) rc = IOMMU_E_NOCOVER;
    else {
        sdom_t* x = 0;
        for (int i = 0; i < SM_MAX_DOMAINS && !x; i++) if (!S.dom[i].used) x = &S.dom[i];
        uint64_t rpa = 0, cpa = 0;
        uint64_t* root = x ? (uint64_t*)t_alloc(0, &rpa) : 0;
        uint64_t* cd = root ? (uint64_t*)t_alloc(0, &cpa) : 0;
        if (!x) rc = IOMMU_E_EXIST;
        else if (!root || !cd) { if (root) t_free(0, root, rpa); rc = IOMMU_E_NOMEM; }
        else {
            zero(x, sizeof *x);
            x->used = 1; x->bus = d->bus; x->dev = d->dev; x->fn = d->fn; x->sid = (uint32_t)sid; x->root = root; x->root_pa = rpa; x->cd = cd; x->cd_pa = cpa;
            uint64_t ste[SMMU_STE_WORDS], cdv[SMMU_CD_WORDS];
            /* Дверной звонок MSI: устройство пишет сообщение прерывания по этому адресу, он идёт через SMMU как любой другой. Одна страница, только она, как окно MMIO. */
            int bell = S.doorbell ? smmu_pt_map(root, S.doorbell & ~0xFFFull, S.doorbell & ~0xFFFull, SMMU_MAP_R | SMMU_MAP_W | SMMU_MAP_DEV, S.oas, &MEM) : 0;
            if (bell || smmu_cd_s1(cdv, rpa, (uint16_t)(sid + 1), S.oas) != 0 || smmu_ste_s1(ste, cpa) != 0) {
                smmu_pt_free(root, rpa, &MEM); t_free(0, cd, cpa); zero(x, sizeof *x); rc = bell == SMMU_D_NOMEM ? IOMMU_E_NOMEM : IOMMU_E_HW;
            }
            else {
                for (int i = 0; i < SMMU_CD_WORDS; i++) cd[i] = cdv[i];
                __asm__ volatile ("dsb sy" : : : "memory");
                ste_write((uint32_t)sid, ste);
                if (inv_stream((uint32_t)sid) != IOMMU_OK) rc = IOMMU_E_HW;     /* домен живой, даже если сброс не подтвердился */
            }
        }
    }
    spin_unlock_irqrestore(&S.lock, f);
    name(d);
    if (rc == IOMMU_OK) uart_print(" isolated: own translation domain, no DMA allowed until buffers are mapped\n");
    else if (rc == IOMMU_E_NOCOVER) uart_print(" NOT isolated: not behind the SMMU, stays unrestricted\n");
    else { uart_print(" NOT isolated: domain setup failed (code "); uart_print_dec((uint64_t)(rc < 0 ? -rc : rc)); uart_print("), stays passthrough\n"); }
    return rc;
}

int iommu_release_device(const pci_dev_t* d) {
    if (!d) return IOMMU_E_ARG;
    int64_t sid = sid_of(d);
    if (sid < 0 || !S.active) return IOMMU_E_NOENT;
    uint64_t f = spin_lock_irqsave(&S.lock);
    sdom_t* x = dom_find((uint32_t)sid);
    int rc = IOMMU_E_NOENT;
    if (x) {
        uint64_t by[SMMU_STE_WORDS];
        smmu_ste_bypass(by);
        ste_write(x->sid, by);
        rc = inv_stream(x->sid);
        if (rc == IOMMU_OK) { smmu_pt_free(x->root, x->root_pa, &MEM); t_free(0, x->cd, x->cd_pa); zero(x, sizeof *x); }     /* таблицы освобождаются после подтверждённого сброса */
    }
    spin_unlock_irqrestore(&S.lock, f);
    if (rc == IOMMU_OK) { name(d); uart_print(" back to passthrough: unrestricted DMA, NOT isolated\n"); }
    return rc;
}

static int range_pages(uint64_t iova, uint64_t len, uint64_t* first, uint32_t* pages) {
    if (!len || iova + len < iova || iova + len > (1ull << SMMU_IPA_BITS)) return -1;
    *first = iova & ~0xFFFull;
    uint64_t n = (iova + len - *first + 0xFFF) >> 12;
    if (n > 0x100000) return -1;
    *pages = (uint32_t)n;
    return 0;
}
static int overlaps(const sdom_t* x, uint64_t first, uint32_t pages) {
    uint64_t e = first + ((uint64_t)pages << 12);
    for (uint32_t i = 0; i < x->nmaps; i++) { uint64_t a = x->maps[i].iova, b = a + ((uint64_t)x->maps[i].pages << 12); if (first < b && a < e) return 1; }
    return 0;
}

int iommu_map(const pci_dev_t* d, uint64_t iova, uint64_t phys, uint64_t len, unsigned rw) {
    uint64_t first; uint32_t pages;
    if (!d) return IOMMU_E_ARG;
    if (!S.active) return IOMMU_E_NONE;
    if ((rw & ~(IOMMU_R | IOMMU_W)) || !rw || ((iova ^ phys) & 0xFFF)) return IOMMU_E_ARG;
    if (range_pages(iova, len, &first, &pages) != 0 || phys + len < phys) return IOMMU_E_RANGE;
    unsigned prw = (rw & IOMMU_R ? SMMU_MAP_R : 0) | (rw & IOMMU_W ? SMMU_MAP_W : 0);
    uint64_t pfirst = phys & ~0xFFFull;
    int64_t sid = sid_of(d);
    int rc = IOMMU_OK;
    uint64_t f = spin_lock_irqsave(&S.lock);
    sdom_t* x = sid >= 0 ? dom_find((uint32_t)sid) : 0;
    if (!x) rc = IOMMU_E_NOENT;
    else if (x->nmaps >= SM_MAX_MAPS || overlaps(x, first, pages)) rc = IOMMU_E_EXIST;
    else {
        uint32_t i;
        for (i = 0; i < pages; i++) {
            int hr = smmu_pt_map(x->root, first + ((uint64_t)i << 12), pfirst + ((uint64_t)i << 12), prw, S.oas, &MEM);
            if (hr) { rc = hr == SMMU_D_NOMEM ? IOMMU_E_NOMEM : hr == SMMU_D_ARG ? IOMMU_E_RANGE : IOMMU_E_HW; break; }
        }
        if (rc != IOMMU_OK) { for (uint32_t k = 0; k < i; k++) (void)smmu_pt_unmap(x->root, first + ((uint64_t)k << 12), &MEM); }
        else { x->maps[x->nmaps].iova = first; x->maps[x->nmaps].pages = pages; x->maps[x->nmaps].rw = rw; x->nmaps++; }
    }
    spin_unlock_irqrestore(&S.lock, f);
    return rc;
}

static int unmap_range(sdom_t* x, uint64_t first, uint32_t pages) {       /* под замком */
    int rc = IOMMU_OK;
    for (uint32_t p = 0; p < pages; p++) if (smmu_pt_unmap(x->root, first + ((uint64_t)p << 12), &MEM)) rc = IOMMU_E_HW;
    uint64_t c[2];
    smmu_cmd_tlbi_nh_all(c);
    if (cmd_put(c) || cmd_sync() != IOMMU_OK) rc = IOMMU_E_HW;
    return rc;
}

int iommu_unmap(const pci_dev_t* d, uint64_t iova, uint64_t len) {
    uint64_t first; uint32_t pages;
    if (!d) return IOMMU_E_ARG;
    if (!S.active) return IOMMU_E_NONE;
    if (range_pages(iova, len, &first, &pages) != 0) return IOMMU_E_RANGE;
    int64_t sid = sid_of(d);
    int rc = IOMMU_E_NOENT;
    uint64_t f = spin_lock_irqsave(&S.lock);
    sdom_t* x = sid >= 0 ? dom_find((uint32_t)sid) : 0;
    for (uint32_t i = 0; x && i < x->nmaps; i++) {
        if (x->maps[i].iova != first || x->maps[i].pages != pages) continue;
        rc = unmap_range(x, first, pages);
        x->maps[i] = x->maps[--x->nmaps];
        break;
    }
    spin_unlock_irqrestore(&S.lock, f);
    return rc;
}

void iommu_unmap_all(const pci_dev_t* d) {
    if (!d || !S.active) return;
    int64_t sid = sid_of(d);
    if (sid < 0) return;
    uint64_t f = spin_lock_irqsave(&S.lock);
    sdom_t* x = dom_find((uint32_t)sid);
    while (x && x->nmaps) { smap_t m = x->maps[--x->nmaps]; (void)unmap_range(x, m.iova, m.pages); }
    spin_unlock_irqrestore(&S.lock, f);
}

#define FAULT_BATCH 32
int iommu_fault_poll(void) {
    if (!S.active) return 0;
    if (__atomic_exchange_n(&S.polling, 1, __ATOMIC_ACQUIRE)) return 0;
    smmu_event_t out[FAULT_BATCH]; uint32_t n = 0, dropped = 0; int total = 0;
    uint64_t f = spin_lock_irqsave(&S.lock);
    uint32_t prod = rd32(R_EVTQ_PROD), cons = rd32(R_EVTQ_CONS);
    while ((prod & 0x1FF) != (cons & 0x1FF)) {
        const uint64_t* e = (const uint64_t*)S.smmu.evtq + (cons & 0xFF) * 4u;
        smmu_event_t ev; smmu_event_decode(e, &ev);
        total++;
        sdom_t* x = dom_find(ev.sid);
        if (x) x->faults++;
        if (n < FAULT_BATCH) out[n++] = ev; else dropped++;
        cons = (cons + 1) & 0x1FF;
    }
    wr32(R_EVTQ_CONS, cons);
    spin_unlock_irqrestore(&S.lock, f);
    for (uint32_t i = 0; i < n; i++) {
        f = spin_lock_irqsave(&S.lock);
        sdom_t* x = dom_find(out[i].sid);
        int skip = x && (x->quiet || x->logged >= SM_LOG_PER_DEVICE);
        if (x && !skip) x->logged++;
        spin_unlock_irqrestore(&S.lock, f);
        if (skip) continue;
        uart_print("[KIRA IOMMU] DMA fault: stream "); uart_print_dec(out[i].sid); uart_print(out[i].is_write ? " write " : " read ");
        uart_print_hex(out[i].addr); uart_print(": "); uart_print(smmu_event_name(out[i].id)); uart_print(" (event "); uart_print_dec(out[i].id); uart_print("), access blocked\n");
    }
    if (dropped) { uart_print("[KIRA IOMMU] "); uart_print_dec(dropped); uart_print(" more fault record(s) not listed\n"); }
    __atomic_store_n(&S.polling, 0, __ATOMIC_RELEASE);
    return total;
}

void iommu_expect_fault(const pci_dev_t* d, int on) {
    if (!d || !S.active) return;
    int64_t sid = sid_of(d);
    if (sid < 0) return;
    uint64_t f = spin_lock_irqsave(&S.lock);
    sdom_t* x = dom_find((uint32_t)sid);
    if (x) x->quiet = on ? 1 : 0;
    spin_unlock_irqrestore(&S.lock, f);
}

uint32_t iommu_fault_count(const pci_dev_t* d) {
    if (!d || !S.active) return 0;
    int64_t sid = sid_of(d);
    if (sid < 0) return 0;
    uint64_t f = spin_lock_irqsave(&S.lock);
    sdom_t* x = dom_find((uint32_t)sid);
    uint32_t r = x ? x->faults : 0;
    spin_unlock_irqrestore(&S.lock, f);
    return r;
}
