/* DMA-изоляция через AMD-Vi (x86-64): все функции iommu.h для платформ с таблицей IVRS. Устройства сначала получают запись «без
   трансляции» (DMA как прежде, честно помечено), драйвер с DMA/IRQ переводит своё устройство в домен с пустой таблицей страниц и
   дальше получает только то, что отображено через iommu_map. Опрос журнала нарушений - iommu_fault_poll (прерывания журнала не используются).
   Замок A.lock охраняет все таблицы; под ним зовутся только распределитель страниц и обращения к регистрам; печати под замком нет. */
#include "iommu_amd.h"
#include "iommu.h"
#include "drivers/iommu/amd_vi.h"
#include "acpi.h"
#include "uart.h"
#include "spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include "ramext.h"

#define AMD_MAX_DOMAINS 16
#define AMD_MAX_MAPS 32
#define AMD_LOG_PER_DEVICE 8
#define RING_ENTRIES 256u                       /* одна страница: 256 записей по 16 байт, ComLen/EventLen = 8 */
#define WAIT_SPINS 20000000u

/* регистры блока */
enum { R_DTBA = 0x00, R_CMDBASE = 0x08, R_EVTBASE = 0x10, R_CTRL = 0x18, R_EFR = 0x30,
       R_CMD_HEAD = 0x2000, R_CMD_TAIL = 0x2008, R_EVT_HEAD = 0x2010, R_EVT_TAIL = 0x2018, R_STATUS = 0x2020 };
#define CTRL_EN (1ull << 0)
#define CTRL_EVT_EN (1ull << 2)
#define CTRL_CMD_EN (1ull << 12)
#define ST_EVT_OVERFLOW (1ull << 0)
#define ST_EVT_INT (1ull << 1)
#define ST_CMD_RUN (1ull << 4)

typedef struct { uint64_t iova; uint32_t pages; uint32_t rw; } amap_t;
typedef struct {
    uint8_t used, bus, dev, fn, unit;
    uint16_t devid;
    uint64_t* root; uint64_t root_pa;
    amap_t maps[AMD_MAX_MAPS]; uint32_t nmaps;
    uint32_t faults, logged;
} adom_t;
typedef struct {
    volatile uint8_t* regs;
    uint32_t* cmd; uint64_t cmd_pa; uint32_t tail;           /* tail - номер записи */
    uint8_t* evt; uint64_t evt_pa;
    volatile uint64_t* wait; uint64_t wait_pa; uint64_t seq;
    uint8_t on;
} aunit_t;

static struct {
    int active;
    amdvi_info_t info;
    aunit_t unit[AMDVI_MAX_UNITS];
    uint64_t* dtab; uint64_t dtab_pa; uint32_t dtab_pages, devs;   /* devs - число записей таблицы устройств */
    adom_t dom[AMD_MAX_DOMAINS];
    uint32_t faults_total;
    spinlock_t lock;
    volatile int polling;
} A;

static void zero(void* p, size_t n) { uint8_t* b = p; while (n--) *b++ = 0; }
static void bdf(uint8_t bus, uint8_t dev, uint8_t fn) {
    static const char h[] = "0123456789abcdef";
    char b[9] = { h[bus >> 4], h[bus & 15], ':', h[dev >> 4], h[dev & 15], '.', h[fn & 15], 0 };
    uart_print(b);
}
static uint16_t devid_of(const pci_dev_t* d) { return (uint16_t)(d->bus << 8 | d->dev << 3 | d->fn); }

static uint64_t rd(const aunit_t* u, unsigned off) { return *(volatile uint64_t*)(u->regs + off); }
static void wr(const aunit_t* u, unsigned off, uint64_t v) { *(volatile uint64_t*)(u->regs + off) = v; }

/* страница, видимая процессору по тому же адресу, по которому её читает устройство */
static void* page_alloc(void* c, uint64_t* pa) {
    (void)c;
    uint64_t p = pmm_alloc_page(), t;
    *pa = 0;
    if (!p) return 0;
    if (vmm_translate(p, &t) != 0 || t != p) { pmm_free_page(p); return 0; }
    zero((void*)(uintptr_t)p, 4096);
    *pa = p;
    return (void*)(uintptr_t)p;
}
static void page_free(void* c, void* v, uint64_t pa) { (void)c; (void)v; pmm_free_page(pa); }
static void* page_virt(void* c, uint64_t pa) { (void)c; return (void*)(uintptr_t)pa; }
static const amdvi_mem_t MEM = { 0, page_alloc, page_free, page_virt };

/* ---- кольцо команд ---- */
static int cmd_put(aunit_t* u, const uint32_t c[4]) {
    uint32_t next = (u->tail + 1) % RING_ENTRIES;
    for (unsigned spin = 0; (uint32_t)(rd(u, R_CMD_HEAD) >> 4) % RING_ENTRIES == next; spin++)   /* кольцо полно - ждём, пока блок заберёт */
        if (spin > WAIT_SPINS) return IOMMU_E_HW;
    uint32_t* e = u->cmd + u->tail * 4u;
    for (int i = 0; i < 4; i++) e[i] = c[i];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    u->tail = next;
    wr(u, R_CMD_TAIL, (uint64_t)next << 4);
    return 0;
}
static int cmd_sync(aunit_t* u) {                                    /* вернуться, когда блок выполнил все команды */
    uint32_t c[4];
    uint64_t v = ++u->seq;
    *u->wait = 0;
    amdvi_cmd_completion_wait(c, u->wait_pa, v);
    if (cmd_put(u, c)) return IOMMU_E_HW;
    for (unsigned spin = 0; spin < WAIT_SPINS; spin++) {
        if (*u->wait == v) return IOMMU_OK;
        __asm__ volatile ("pause");
    }
    return IOMMU_E_HW;
}
static int inv_device(aunit_t* u, uint16_t devid, uint16_t domain) {
    uint32_t c[4];
    amdvi_cmd_inv_devtab(c, devid);
    if (cmd_put(u, c)) return IOMMU_E_HW;
    /* Кэш трансляций помечен номером домена: записи прежнего режима (без трансляции - домен 0) надо сбросить так же, как записи нового,
       иначе устройство ещё какое-то время ходит по старым разрешениям. */
    amdvi_cmd_inv_pages_all(c, 0);
    if (cmd_put(u, c)) return IOMMU_E_HW;
    if (domain) { amdvi_cmd_inv_pages_all(c, domain); if (cmd_put(u, c)) return IOMMU_E_HW; }
    return cmd_sync(u);
}

static void dte_write(uint16_t devid, const uint64_t v[4]) {
    volatile uint64_t* d = A.dtab + (size_t)devid * 4;
    d[3] = v[3]; d[2] = v[2]; d[1] = v[1];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    d[0] = v[0];                                                      /* V - последним: блок не увидит наполовину записанную запись */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static const void* ivrs_view(uint64_t phys, uint64_t len) {
    if (ramext_map_read(phys, len)) return (const void*)(uintptr_t)phys;
    return (const void*)vmm_map_mmio(phys, len, VMM_MMIO_UC);
}

/* Регистры блока: таблица устройств, кольца команд и событий, включение. Общий порядок для первого пуска и пробуждения из сна (память колец и таблицы - прежняя). */
static int unit_program(aunit_t* u) {
    wr(u, R_CTRL, rd(u, R_CTRL) & ~(CTRL_EN | CTRL_CMD_EN | CTRL_EVT_EN));       /* на всякий случай: выключить, чем бы его ни оставила микропрограмма */
    wr(u, R_DTBA, A.dtab_pa | (A.dtab_pages - 1));
    wr(u, R_CMDBASE, u->cmd_pa | (8ull << 56));
    wr(u, R_EVTBASE, u->evt_pa | (8ull << 56));
    wr(u, R_CMD_HEAD, 0); wr(u, R_CMD_TAIL, 0); wr(u, R_EVT_HEAD, 0); wr(u, R_EVT_TAIL, 0);
    u->tail = 0;
    wr(u, R_CTRL, rd(u, R_CTRL) | CTRL_CMD_EN | CTRL_EVT_EN);
    wr(u, R_CTRL, rd(u, R_CTRL) | CTRL_EN);
    for (unsigned spin = 0; !(rd(u, R_STATUS) & ST_CMD_RUN); spin++) if (spin > WAIT_SPINS) return -1;
    return cmd_sync(u) == IOMMU_OK ? 0 : -1;                                       /* команда прошла кольцо: блок жив и читает таблицы */
}

static int unit_enable(aunit_t* u, unsigned idx) {
    u->regs = (volatile uint8_t*)vmm_map_mmio(A.info.unit[idx].base, 0x4000, VMM_MMIO_UC);
    if (!u->regs) return -1;
    uint64_t pa;
    u->cmd = page_alloc(0, &pa); u->cmd_pa = pa;
    u->evt = page_alloc(0, &pa); u->evt_pa = pa;
    u->wait = page_alloc(0, &pa); u->wait_pa = pa;
    if (!u->cmd || !u->evt || !u->wait) return -1;
    return unit_program(u);
}

/* Сон S3: после пробуждения блоки теряют регистры; таблица устройств, домены и кольца лежат в памяти. Регистры программируются заново, блок читает прежнюю
   таблицу - домены устройств действуют как до сна. Журнал событий (отказов за время сна быть не могло: устройства молчали) начинается с пустого кольца. */
int amd_iommu_resume(void) {
    int bad = 0;
    for (unsigned i = 0; i < A.info.nunits; i++) {
        aunit_t* u = &A.unit[i];
        if (!u->on) continue;
        if (unit_program(u) != 0) { bad++; uart_print("[KIRA SLEEP] AMD-Vi unit "); uart_print_dec(i); uart_print(" did not start after sleep: its devices are NOT isolated now.\n"); }
    }
    return bad ? -1 : 0;
}

int amd_iommu_active(void) { return A.active; }

int amd_iommu_init(void) {
    if (A.active) return 1;
    const acpi_info_t* ai = acpi_get();
    if (!ai || !ai->ivrs_phys) return 0;
    const void* t = ivrs_view(ai->ivrs_phys, ai->ivrs_len);
    if (!t) { uart_print("[KIRA IOMMU] IVRS table cannot be read: AMD-Vi left off, devices keep unrestricted DMA.\n"); return -1; }
    int rc = amdvi_ivrs_parse(t, ai->ivrs_len, &A.info);
    if (rc) {
        zero(&A.info, sizeof A.info);
        uart_print("[KIRA IOMMU] IVRS table rejected (code "); uart_print_dec((uint64_t)-rc); uart_print("): AMD-Vi left off, devices keep unrestricted DMA.\n");
        return -1;
    }
    uart_print("[KIRA IOMMU] IVRS: "); uart_print_dec(A.info.nunits); uart_print(" AMD-Vi unit(s), ");
    uart_print_dec(A.info.nivmd); uart_print(" firmware memory range(s), "); uart_print_dec(A.info.nspecial); uart_print(" special device(s)\n");
    /* таблица устройств: записи для всех шин, где есть PCI-устройства; остальное блок отвергает сам */
    unsigned maxbus = 0;
    for (int i = 0; i < pci_count(); i++) { const pci_dev_t* d = pci_get(i); if (d->bus > maxbus) maxbus = d->bus; }
    A.dtab_pages = (maxbus + 1) * 2;
    A.devs = (maxbus + 1) * 256;
    A.dtab_pa = pmm_alloc_contiguous(A.dtab_pages);
    uint64_t pa;
    if (!A.dtab_pa || vmm_translate(A.dtab_pa, &pa) != 0 || pa != A.dtab_pa) {
        if (A.dtab_pa) pmm_free_contiguous(A.dtab_pa, A.dtab_pages);
        uart_print("[KIRA IOMMU] AMD-Vi: no memory for the device table, left off.\n");
        return -1;
    }
    A.dtab = (uint64_t*)(uintptr_t)A.dtab_pa;
    zero(A.dtab, (size_t)A.dtab_pages * 4096);
    uint64_t pt[4];
    amdvi_dte_passthrough(pt);
    for (uint32_t d = 0; d < A.devs; d++) dte_write((uint16_t)d, pt);             /* сначала все - без трансляции */
    int n = 0;
    for (unsigned i = 0; i < A.info.nunits; i++) {
        if (unit_enable(&A.unit[i], i) == 0) { A.unit[i].on = 1; n++; }
        else { uart_print("[KIRA IOMMU] AMD-Vi unit "); uart_print_dec(i); uart_print(" did not start: its devices stay unrestricted\n"); }
    }
    if (!n) { return 0; }                                                          /* ни один блок не заработал: таблица остаётся, но она не читается */
    A.active = 1;
    uart_print("[KIRA IOMMU] AMD-Vi translation enabled on "); uart_print_dec((uint64_t)n); uart_print(" unit(s).\n");
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        int ui = amdvi_unit_of(&A.info, devid_of(d));
        uart_print("[KIRA IOMMU] "); bdf(d->bus, d->dev, d->fn);
        if (ui >= 0 && A.unit[ui].on) uart_print(" passthrough: unrestricted DMA, NOT isolated\n");
        else uart_print(" no AMD-Vi unit translates it: DMA unrestricted\n");
    }
    return n;
}

/* ---- домены ---- */
static adom_t* dom_find(uint16_t devid) {
    for (int i = 0; i < AMD_MAX_DOMAINS; i++) if (A.dom[i].used && A.dom[i].devid == devid) return &A.dom[i];
    return 0;
}
int amd_iommu_isolated(const pci_dev_t* d) {
    if (!d || !A.active) return 0;
    uint64_t f = spin_lock_irqsave(&A.lock);
    int r = dom_find(devid_of(d)) != 0;
    spin_unlock_irqrestore(&A.lock, f);
    return r;
}

int amd_iommu_isolate(const pci_dev_t* d) {
    if (!d) return IOMMU_E_ARG;
    if (!A.active) return IOMMU_E_NONE;
    uint16_t id = devid_of(d);
    int ui = amdvi_unit_of(&A.info, id), rc = IOMMU_OK;
    uint64_t f = spin_lock_irqsave(&A.lock);
    if (dom_find(id)) { spin_unlock_irqrestore(&A.lock, f); return IOMMU_OK; }
    if (ui < 0 || !A.unit[ui].on || id >= A.devs || amdvi_is_special(&A.info, id)) rc = IOMMU_E_NOCOVER;
    else if (amdvi_ivmd_hit(&A.info, id)) rc = IOMMU_E_RMRR;
    else {
        adom_t* x = 0;
        for (int i = 0; i < AMD_MAX_DOMAINS && !x; i++) if (!A.dom[i].used) x = &A.dom[i];
        uint64_t rpa = 0;
        uint64_t* root = x ? (uint64_t*)page_alloc(0, &rpa) : 0;
        if (!x) rc = IOMMU_E_EXIST;
        else if (!root) rc = IOMMU_E_NOMEM;
        else {
            zero(x, sizeof *x);
            x->used = 1; x->bus = d->bus; x->dev = d->dev; x->fn = d->fn; x->unit = (uint8_t)ui; x->devid = id; x->root = root; x->root_pa = rpa;
            uint64_t dte[4];
            amdvi_dte_translated(dte, rpa, (uint16_t)(id + 1));
            dte_write(id, dte);
            if (inv_device(&A.unit[ui], id, (uint16_t)(id + 1)) != IOMMU_OK) rc = IOMMU_E_HW;   /* домен живой, даже если сброс кэша не подтвердился */
        }
    }
    spin_unlock_irqrestore(&A.lock, f);
    uart_print("[KIRA IOMMU] "); bdf(d->bus, d->dev, d->fn);
    if (rc == IOMMU_OK) uart_print(" isolated: own translation domain, no DMA allowed until buffers are mapped\n");
    else if (rc == IOMMU_E_RMRR) uart_print(" NOT isolated: firmware needs a memory range for it (IVMD), stays passthrough\n");
    else if (rc == IOMMU_E_NOCOVER) uart_print(" NOT isolated: no active AMD-Vi unit covers it (or it is aliased/special), stays unrestricted\n");
    else { uart_print(" NOT isolated: domain setup failed (code "); uart_print_dec((uint64_t)(rc < 0 ? -rc : rc)); uart_print("), stays passthrough\n"); }
    return rc;
}

int amd_iommu_release(const pci_dev_t* d) {
    if (!d) return IOMMU_E_ARG;
    uint64_t f = spin_lock_irqsave(&A.lock);
    adom_t* x = dom_find(devid_of(d));
    int rc = IOMMU_E_NOENT;
    if (x) {
        uint64_t pt[4];
        amdvi_dte_passthrough(pt);
        dte_write(x->devid, pt);
        rc = inv_device(&A.unit[x->unit], x->devid, (uint16_t)(x->devid + 1));
        if (rc == IOMMU_OK) { amdvi_pt_free(x->root, x->root_pa, &MEM); zero(x, sizeof *x); }   /* таблицы освобождаются только после подтверждённого сброса */
    }
    spin_unlock_irqrestore(&A.lock, f);
    if (rc == IOMMU_OK) { uart_print("[KIRA IOMMU] "); bdf(d->bus, d->dev, d->fn); uart_print(" back to passthrough: unrestricted DMA, NOT isolated\n"); }
    return rc;
}

static int range_pages(uint64_t iova, uint64_t len, uint64_t* first, uint32_t* pages) {
    if (!len || iova + len < iova || iova + len > (1ull << AMDVI_IOVA_BITS)) return -1;
    *first = iova & ~0xFFFull;
    uint64_t n = (iova + len - *first + 0xFFF) >> 12;
    if (n > 0x100000) return -1;
    *pages = (uint32_t)n;
    return 0;
}
static int overlaps(const adom_t* x, uint64_t first, uint32_t pages) {
    uint64_t e = first + ((uint64_t)pages << 12);
    for (uint32_t i = 0; i < x->nmaps; i++) {
        uint64_t a = x->maps[i].iova, b = a + ((uint64_t)x->maps[i].pages << 12);
        if (first < b && a < e) return 1;
    }
    return 0;
}

int amd_iommu_map(const pci_dev_t* d, uint64_t iova, uint64_t phys, uint64_t len, unsigned rw) {
    uint64_t first; uint32_t pages;
    if (!d) return IOMMU_E_ARG;
    if (!A.active) return IOMMU_E_NONE;
    if ((rw & ~(IOMMU_R | IOMMU_W)) || !rw || ((iova ^ phys) & 0xFFF)) return IOMMU_E_ARG;
    if (range_pages(iova, len, &first, &pages) != 0 || phys + len < phys || phys + len > (1ull << 52)) return IOMMU_E_RANGE;
    unsigned prw = (rw & IOMMU_R ? AMDVI_MAP_R : 0) | (rw & IOMMU_W ? AMDVI_MAP_W : 0);
    uint64_t pfirst = phys & ~0xFFFull;
    int rc = IOMMU_OK;
    uint64_t f = spin_lock_irqsave(&A.lock);
    adom_t* x = dom_find(devid_of(d));
    if (!x) rc = IOMMU_E_NOENT;
    else if (x->nmaps >= AMD_MAX_MAPS || overlaps(x, first, pages)) rc = IOMMU_E_EXIST;
    else {
        uint32_t i;
        for (i = 0; i < pages; i++) {
            int hr = amdvi_pt_map(x->root, first + ((uint64_t)i << 12), pfirst + ((uint64_t)i << 12), prw, &MEM);
            if (hr) { rc = hr == AMDVI_E_NOMEM ? IOMMU_E_NOMEM : IOMMU_E_HW; break; }
        }
        if (rc != IOMMU_OK) { for (uint32_t k = 0; k < i; k++) (void)amdvi_pt_unmap(x->root, first + ((uint64_t)k << 12), &MEM); }
        else { x->maps[x->nmaps].iova = first; x->maps[x->nmaps].pages = pages; x->maps[x->nmaps].rw = rw; x->nmaps++; }
    }
    spin_unlock_irqrestore(&A.lock, f);
    return rc;
}

static int unmap_range(adom_t* x, uint64_t first, uint32_t pages) {       /* под замком: снять страницы и дождаться сброса кэша блока */
    int rc = IOMMU_OK;
    for (uint32_t p = 0; p < pages; p++) if (amdvi_pt_unmap(x->root, first + ((uint64_t)p << 12), &MEM)) rc = IOMMU_E_HW;
    if (inv_device(&A.unit[x->unit], x->devid, (uint16_t)(x->devid + 1)) != IOMMU_OK) rc = IOMMU_E_HW;
    return rc;
}

int amd_iommu_unmap(const pci_dev_t* d, uint64_t iova, uint64_t len) {
    uint64_t first; uint32_t pages;
    if (!d) return IOMMU_E_ARG;
    if (!A.active) return IOMMU_E_NONE;
    if (range_pages(iova, len, &first, &pages) != 0) return IOMMU_E_RANGE;
    int rc = IOMMU_E_NOENT;
    uint64_t f = spin_lock_irqsave(&A.lock);
    adom_t* x = dom_find(devid_of(d));
    for (uint32_t i = 0; x && i < x->nmaps; i++) {
        if (x->maps[i].iova != first || x->maps[i].pages != pages) continue;
        rc = unmap_range(x, first, pages);
        x->maps[i] = x->maps[--x->nmaps];
        break;
    }
    spin_unlock_irqrestore(&A.lock, f);
    return rc;
}

void amd_iommu_unmap_all(const pci_dev_t* d) {
    if (!d || !A.active) return;
    uint64_t f = spin_lock_irqsave(&A.lock);
    adom_t* x = dom_find(devid_of(d));
    while (x && x->nmaps) { amap_t m = x->maps[--x->nmaps]; (void)unmap_range(x, m.iova, m.pages); }
    spin_unlock_irqrestore(&A.lock, f);
}

/* ---- журнал нарушений ---- */
typedef struct { uint16_t devid; uint8_t type; uint16_t flags; uint64_t address; } afault_t;
#define FAULT_BATCH 32

int amd_iommu_fault_poll(void) {
    if (!A.active) return 0;
    if (__atomic_exchange_n(&A.polling, 1, __ATOMIC_ACQUIRE)) return 0;
    afault_t out[FAULT_BATCH]; uint32_t n = 0, dropped = 0; int total = 0;
    uint64_t f = spin_lock_irqsave(&A.lock);
    for (unsigned i = 0; i < A.info.nunits; i++) {
        aunit_t* u = &A.unit[i];
        if (!u->on) continue;
        uint64_t st = rd(u, R_STATUS);
        if (st & ST_EVT_OVERFLOW) { wr(u, R_EVT_HEAD, rd(u, R_EVT_TAIL)); wr(u, R_STATUS, ST_EVT_OVERFLOW); dropped++; }   /* переполнение: записи потеряны */
        uint32_t head = (uint32_t)(rd(u, R_EVT_HEAD) >> 4) % RING_ENTRIES, tail = (uint32_t)(rd(u, R_EVT_TAIL) >> 4) % RING_ENTRIES;
        while (head != tail) {
            const uint32_t* e = (const uint32_t*)(u->evt + head * 16u);
            afault_t ft = { (uint16_t)(e[0] & 0xFFFF), (uint8_t)(e[1] >> 28), (uint16_t)((e[1] >> 16) & 0xFFF), (uint64_t)e[3] << 32 | e[2] };
            total++; A.faults_total++;
            adom_t* x = dom_find(ft.devid);
            if (x) x->faults++;
            if (n < FAULT_BATCH) out[n++] = ft; else dropped++;
            head = (head + 1) % RING_ENTRIES;
        }
        wr(u, R_EVT_HEAD, (uint64_t)head << 4);
        if (st & ST_EVT_INT) wr(u, R_STATUS, ST_EVT_INT);
    }
    spin_unlock_irqrestore(&A.lock, f);
    for (uint32_t i = 0; i < n; i++) {
        f = spin_lock_irqsave(&A.lock);
        adom_t* x = dom_find(out[i].devid);
        int skip = x && x->logged >= AMD_LOG_PER_DEVICE;
        if (x && !skip) x->logged++;
        spin_unlock_irqrestore(&A.lock, f);
        if (skip) continue;
        if (out[i].type == 2) {                                               /* IO_PAGE_FAULT */
            uart_print("[KIRA IOMMU] DMA fault: device "); bdf((uint8_t)(out[i].devid >> 8), (uint8_t)((out[i].devid >> 3) & 31), (uint8_t)(out[i].devid & 7));
            uart_print(out[i].flags & 0x20 ? " write " : " read "); uart_print_hex(out[i].address);
            uart_print(": page not mapped for this device (event IO_PAGE_FAULT), access blocked\n");
        } else {
            uart_print("[KIRA IOMMU] AMD-Vi event "); uart_print_dec(out[i].type); uart_print(" for device ");
            bdf((uint8_t)(out[i].devid >> 8), (uint8_t)((out[i].devid >> 3) & 31), (uint8_t)(out[i].devid & 7)); uart_print("\n");
        }
    }
    if (dropped) { uart_print("[KIRA IOMMU] "); uart_print_dec(dropped); uart_print(" more fault record(s) not listed\n"); }
    __atomic_store_n(&A.polling, 0, __ATOMIC_RELEASE);
    return total;
}

uint32_t amd_iommu_fault_count(const pci_dev_t* d) {
    if (!d || !A.active) return 0;
    uint64_t f = spin_lock_irqsave(&A.lock);
    adom_t* x = dom_find(devid_of(d));
    uint32_t r = x ? x->faults : 0;
    spin_unlock_irqrestore(&A.lock, f);
    return r;
}
