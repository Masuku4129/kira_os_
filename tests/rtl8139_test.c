#define _GNU_SOURCE
/* Драйвер RTL8139 (drivers/net/rtl8139.c) против программной модели чипа по публичной спецификации:
   сброс, MAC, кольцо приёма 8 КиБ с переходом через конец, ошибочные кадры, переполнение, 4 дескриптора передачи по кругу. */
#include "../drivers/net/rtl8139.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define RING 8192u
typedef struct {
    uint8_t cmd; uint16_t capr, cbr, imr, isr; uint32_t rbstart, rcr, tcr, tsad[4], tsd[4]; uint8_t msr, config1;
    uint8_t mac[6];
    uint8_t *mem_lo;                       /* память DMA в младших 4 ГиБ: физический адрес = виртуальный */
    int next_tx_expect, tx_order_error, nframes; uint8_t txf[8][2048]; size_t txlen[8];
    int dropped, link;
    uint8_t rxf[256][1600]; size_t rxlen[256]; int nrx;
    unsigned txpend;
    int dma_high;                          /* 1 - dma_alloc отдаёт адрес выше 4 ГиБ */
    int frees;
} M;

static void complete_tx(M *m) { for (int i = 0; i < 4; i++) if (m->txpend & (1u << i)) { m->tsd[i] |= (1u << 13) | (1u << 15); m->isr |= 4; } m->txpend = 0; }
static uint8_t bufe(M *m) { return ((m->cbr & (RING - 1)) == ((uint16_t)(m->capr + 16) & (RING - 1))) ? 1 : 0; }
static uint8_t in8(void *c, uint16_t r) { M *m = c;
    if (r < 6) return m->mac[r];
    if (r == 0x37) return (uint8_t)((m->cmd & ~1) | bufe(m));
    if (r == 0x52) return m->config1;
    if (r == 0x58) return m->link ? 0 : 4;
    return 0; }
static uint16_t in16(void *c, uint16_t r) { M *m = c; return r == 0x3e ? m->isr : r == 0x3a ? m->cbr : 0; }
static uint32_t in32(void *c, uint16_t r) { M *m = c; if (r >= 0x10 && r < 0x20) return m->tsd[(r - 0x10) / 4]; return 0; }
static void out8(void *c, uint16_t r, uint8_t v) { M *m = c;
    if (r == 0x37) { if (v & 0x10) { m->cmd = 0; m->capr = 0; m->cbr = 0; m->rbstart = 0; m->next_tx_expect = 0; } else m->cmd = v; }
    else if (r == 0x52) m->config1 = v; }
static void out16(void *c, uint16_t r, uint16_t v) { M *m = c;
    if (r == 0x38) m->capr = v;
    else if (r == 0x3c) m->imr = v;
    else if (r == 0x3e) m->isr &= (uint16_t)~v; }
static void out32(void *c, uint16_t r, uint32_t v) { M *m = c;
    if (r == 0x30) { m->rbstart = v; m->cbr = 0; }
    else if (r == 0x44) m->rcr = v;
    else if (r == 0x40) m->tcr = v;
    else if (r >= 0x20 && r < 0x30) m->tsad[(r - 0x20) / 4] = v;
    else if (r >= 0x10 && r < 0x20) {
        int i = (r - 0x10) / 4;
        if (i != m->next_tx_expect) m->tx_order_error = 1;
        m->next_tx_expect = (i + 1) & 3;
        size_t len = v & 0x1fff;
        memcpy(m->txf[m->nframes & 7], (void *)(uintptr_t)m->tsad[i], len);
        m->txlen[m->nframes & 7] = len; m->nframes++;
        m->tsd[i] = v & 0x1fff;                               /* передача идёт: ни OWN, ни TOK */
        m->txpend |= 1u << i;
    } }
static void *dma_alloc(void *c, size_t size, size_t al, uint64_t *phys) { M *m = c; (void)al;
    static size_t used;
    if (m->dma_high) { *phys = 1ull << 33; return m->mem_lo; }
    used = (used + 255) & ~(size_t)255; uint8_t *p = m->mem_lo + used; used += size; if (used > (1u << 20)) return 0;
    *phys = (uint64_t)(uintptr_t)p; return p; }
static void dma_free(void *c, void *v, uint64_t p, size_t s) { M *m = c; (void)v; (void)p; (void)s; m->frees++; }
static void rxcb(void *c, const uint8_t *f, size_t n) { M *m = c; if (m->nrx < 256 && n <= 1600) { memcpy(m->rxf[m->nrx], f, n); m->rxlen[m->nrx++] = n; } }

/* чип кладёт кадр в кольцо: статус(2) длина(2, с CRC) данные CRC, выравнивание до 4; WRAP выключен - переход через конец */
static int inject(M *m, const uint8_t *f, size_t n, uint16_t status, int bad_len) {
    uint8_t *ring = (uint8_t *)(uintptr_t)m->rbstart;
    size_t total = (4 + n + 4 + 3) & ~(size_t)3;
    size_t used = ((m->cbr & (RING - 1)) + RING - ((uint16_t)(m->capr + 16) & (RING - 1))) & (RING - 1);
    if (total + 4 > RING - used) { m->isr |= 0x10; m->dropped++; return -1; }     /* RXOVW */
    size_t o = m->cbr & (RING - 1);
    uint16_t len = (uint16_t)(bad_len ? 0 : n + 4);
    uint8_t h[4] = { (uint8_t)status, (uint8_t)(status >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    uint8_t crc[4] = { 1, 2, 3, 4 };
    for (int i = 0; i < 4; i++) ring[(o + i) & (RING - 1)] = h[i];
    for (size_t i = 0; i < n; i++) ring[(o + 4 + i) & (RING - 1)] = f[i];
    for (int i = 0; i < 4; i++) ring[(o + 4 + n + i) & (RING - 1)] = crc[i];
    m->cbr = (uint16_t)((o + total) & (RING - 1));
    m->isr |= 1;
    return 0;
}
static void frame(uint8_t *b, size_t n, int seed) { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed * 31 + i * 7); }

int main(void) {
    static M m; struct kira_rtl8139 d; struct kira_rtl8139_platform p;
    m.mem_lo = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(m.mem_lo != MAP_FAILED);
    memcpy(m.mac, "\x52\x54\x00\x12\x34\x56", 6); m.link = 1;
    memset(&p, 0, sizeof p);
    p.context = &m; p.in8 = in8; p.in16 = in16; p.in32 = in32; p.out8 = out8; p.out16 = out16; p.out32 = out32;
    p.dma_alloc = dma_alloc; p.dma_free = dma_free; p.receive = rxcb;

    assert(kira_rtl8139_init(&d, 0, &p) == 0);
    assert(!memcmp(d.mac, m.mac, 6));
    assert(!(m.rcr & (1u << 7)));                       /* WRAP выключен: чип не пишет за пределы кольца */
    assert((m.cmd & 0x0c) == 0x0c);
    assert(kira_rtl8139_link_up(&d) == 1); m.link = 0; assert(kira_rtl8139_link_up(&d) == 0); m.link = 1;

    /* приём: много кадров разной длины, кольцо проходит конец не один раз */
    uint8_t f[1600], want[1600]; int total = 0;
    for (int round = 0; round < 40; round++) {
        m.nrx = 0;
        int k = 0;
        for (int i = 0; i < 6; i++) { size_t n = 60 + (size_t)(round * 37 + i * 211) % 1400; frame(f, n, round * 8 + i);
            if (inject(&m, f, n, 1, 0) == 0) k++; }
        assert(kira_rtl8139_poll(&d) >= 0);
        assert(m.nrx == k);
        for (int i = 0; i < m.nrx; i++) { size_t n = 60 + (size_t)(round * 37 + i * 211) % 1400; frame(want, n, round * 8 + i);
            assert(m.rxlen[i] == n && !memcmp(m.rxf[i], want, n)); }
        total += k;
    }
    assert(total > 200);

    /* кадр с ошибкой CRC пропускается, следующий принимается */
    m.nrx = 0; frame(f, 100, 1); inject(&m, f, 100, 0x0005, 0); frame(f, 120, 2); inject(&m, f, 120, 1, 0);
    kira_rtl8139_poll(&d); assert(m.nrx == 1 && m.rxlen[0] == 120);

    /* испорченный заголовок (длина 0): приём перезапускается, дальше работает */
    m.nrx = 0; frame(f, 100, 3); inject(&m, f, 100, 1, 1);
    kira_rtl8139_poll(&d); assert(m.nrx == 0);
    assert(m.rbstart && m.capr == 0xfff0);
    frame(f, 200, 4); inject(&m, f, 200, 1, 0); kira_rtl8139_poll(&d);
    assert(m.nrx == 1 && m.rxlen[0] == 200);

    /* переполнение кольца: потери, но после перезапуска приём продолжается */
    m.nrx = 0; int dropped0 = m.dropped;
    for (int i = 0; i < 12; i++) { frame(f, 1500, i); inject(&m, f, 1500, 1, 0); }
    assert(m.dropped > dropped0);
    kira_rtl8139_poll(&d);
    m.nrx = 0; frame(f, 300, 9); assert(inject(&m, f, 300, 1, 0) == 0); kira_rtl8139_poll(&d);
    assert(m.nrx == 1 && m.rxlen[0] == 300);

    /* передача: строго по кругу, 4 дескриптора, дополнение до 60 байт */
    m.nframes = 0;
    uint8_t t[200]; frame(t, 100, 5);
    for (int i = 0; i < 4; i++) assert(kira_rtl8139_send(&d, t, 100) == i);
    assert(!m.tx_order_error);
    assert(kira_rtl8139_send(&d, t, 100) == -2);        /* все заняты: чип ещё не закончил */
    complete_tx(&m);
    kira_rtl8139_poll(&d);
    assert(kira_rtl8139_send(&d, t, 100) == 0);         /* круг начался заново с 0 */
    assert(kira_rtl8139_send(&d, t, 10) == -1 && kira_rtl8139_send(&d, t, 1519) == -1 && kira_rtl8139_send(&d, 0, 100) == -1);
    complete_tx(&m);
    kira_rtl8139_poll(&d);
    assert(kira_rtl8139_send(&d, t, 14) == 1);
    assert(m.txlen[(m.nframes - 1) & 7] == 60);
    for (int i = 14; i < 60; i++) assert(m.txf[(m.nframes - 1) & 7][i] == 0);
    assert(!m.tx_order_error);

    kira_rtl8139_shutdown(&d);
    assert(m.frees == 5 && !d.initialized);

    /* DMA выше 4 ГиБ чип адресовать не может: отказ */
    static M m2; m2.mem_lo = m.mem_lo; m2.dma_high = 1; memcpy(m2.mac, m.mac, 6); p.context = &m2;
    assert(kira_rtl8139_init(&d, 0, &p) == -3);
    printf("rtl8139: init, rx ring wrap, errors, overrun, tx order, DMA limits passed\n");
    return 0;
}
