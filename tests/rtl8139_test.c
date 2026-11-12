#include "../drivers/net/rtl8139.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define IO_BASE 0x1000u
#define REG_COMMAND 0x37u
#define REG_CAPR 0x38u
#define REG_IMR 0x3cu
#define REG_ISR 0x3eu
#define REG_RBSTART 0x30u
#define REG_TSD0 0x10u
#define CMD_RESET 0x10u
#define CMD_BUFE 0x01u
#define CMD_RX_ENABLE 0x08u
#define CMD_TX_ENABLE 0x04u
#define TSD_TOK (1u << 15)
#define TSD_TUN (1u << 14)
#define TSD_OWN (1u << 13)
#define TSD_TABT (1u << 30)
#define RX_ROK 0x0001u
#define RX_FAE 0x0002u
#define BMSR_LINK (1u << 2)

typedef struct {
    uint8_t regs[256];
    uint32_t tsd[KIRA_RTL8139_TX_SLOTS];
    uint32_t tsad[KIRA_RTL8139_TX_SLOTS];
    uint8_t rx[KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE]
        __attribute__((aligned(256)));
    uint8_t tx[KIRA_RTL8139_TX_SLOTS][KIRA_RTL8139_TX_BUFFER_SIZE]
        __attribute__((aligned(256)));
    uint64_t next_phys;
    unsigned alloc_count, free_count, fail_alloc_at;
    int bad_rx_phys, reset_stuck;
    unsigned clean_calls, invalidate_calls, read_barriers, write_barriers;
    unsigned receive_calls, tx_complete_calls;
    size_t received_length;
    uint8_t received[KIRA_RTL8139_MAX_FRAME];
    int tx_status[KIRA_RTL8139_TX_SLOTS];
    uint16_t last_capr, imr;
    uint32_t rx_phys_reg;
} mock_t;

static uint8_t in8(void *opaque, uint16_t p)
{
    mock_t *m = opaque;
    uint16_t r = (uint16_t)(p - IO_BASE);
    if (r == REG_COMMAND && (m->regs[r] & CMD_RESET) && !m->reset_stuck)
        m->regs[r] = 0;
    return m->regs[r];
}
static uint16_t in16(void *opaque, uint16_t p)
{
    mock_t *m = opaque;
    uint16_t r = (uint16_t)(p - IO_BASE);
    return (uint16_t)m->regs[r] | ((uint16_t)m->regs[r + 1u] << 8);
}
static uint32_t in32(void *opaque, uint16_t p)
{
    mock_t *m = opaque;
    uint16_t r = (uint16_t)(p - IO_BASE);
    if (r >= REG_TSD0 && r < REG_TSD0 + 16u && !(r & 3u))
        return m->tsd[(r - REG_TSD0) / 4u];
    return (uint32_t)m->regs[r] | ((uint32_t)m->regs[r + 1u] << 8) |
           ((uint32_t)m->regs[r + 2u] << 16) |
           ((uint32_t)m->regs[r + 3u] << 24);
}
static void out8(void *opaque, uint16_t p, uint8_t v)
{
    mock_t *m = opaque;
    uint16_t r = (uint16_t)(p - IO_BASE);
    m->regs[r] = v;
    if (r == REG_COMMAND && (v & CMD_RESET) && !m->reset_stuck)
        m->regs[r] = 0;
}
static void out16(void *opaque, uint16_t p, uint16_t v)
{
    mock_t *m = opaque;
    uint16_t r = (uint16_t)(p - IO_BASE);
    if (r == REG_ISR) {
        m->regs[r] &= (uint8_t)~v;
        m->regs[r + 1u] &= (uint8_t)~(v >> 8);
    } else {
        m->regs[r] = (uint8_t)v;
        m->regs[r + 1u] = (uint8_t)(v >> 8);
        if (r == REG_CAPR) m->last_capr = v;
        if (r == REG_IMR) m->imr = v;
    }
}
static void out32(void *opaque, uint16_t p, uint32_t v)
{
    mock_t *m = opaque;
    uint16_t r = (uint16_t)(p - IO_BASE);
    if (r >= 0x20u && r < 0x30u && !(r & 3u))
        m->tsad[(r - 0x20u) / 4u] = v;
    if (r == REG_RBSTART) m->rx_phys_reg = v;
    if (r >= REG_TSD0 && r < REG_TSD0 + 16u && !(r & 3u))
        m->tsd[(r - REG_TSD0) / 4u] = v & ~TSD_OWN;
    m->regs[r] = (uint8_t)v;
    m->regs[r + 1u] = (uint8_t)(v >> 8);
    m->regs[r + 2u] = (uint8_t)(v >> 16);
    m->regs[r + 3u] = (uint8_t)(v >> 24);
}
static void *dma_alloc(void *opaque, size_t size, size_t alignment,
                       uint64_t *physical)
{
    mock_t *m = opaque;
    unsigned index = ++m->alloc_count;
    assert(alignment == 256u);
    if (index == m->fail_alloc_at) return 0;
    if (index == 1u) {
        *physical = m->bad_rx_phys ? 0x100000000ull : 0x100000ull;
        assert(size == sizeof m->rx);
        return m->rx;
    }
    assert(index <= KIRA_RTL8139_TX_SLOTS + 1u);
    *physical = 0x200000ull + (uint64_t)(index - 2u) * 0x1000ull;
    assert(size == KIRA_RTL8139_TX_BUFFER_SIZE);
    return m->tx[index - 2u];
}
static void dma_free(void *opaque, void *v, uint64_t p, size_t size)
{
    mock_t *m = opaque;
    (void)p;
    assert(v != 0 && size != 0);
    ++m->free_count;
}
static void cache_invalidate(void *opaque, void *p, size_t n)
{
    mock_t *m = opaque;
    assert(p != 0 && n != 0);
    ++m->invalidate_calls;
}
static void cache_clean(void *opaque, const void *p, size_t n)
{
    mock_t *m = opaque;
    assert(p != 0 && n != 0);
    ++m->clean_calls;
}
static void dma_read_barrier(void *opaque) { ++((mock_t *)opaque)->read_barriers; }
static void dma_write_barrier(void *opaque) { ++((mock_t *)opaque)->write_barriers; }
static void delay_us(void *opaque, uint32_t us) { (void)opaque; (void)us; }
static void receive(void *opaque, const uint8_t *frame, size_t length)
{
    mock_t *m = opaque;
    assert(length <= sizeof m->received);
    memcpy(m->received, frame, length);
    m->received_length = length;
    ++m->receive_calls;
}
static void tx_complete(void *opaque, unsigned int slot, int status)
{
    mock_t *m = opaque;
    assert(slot < KIRA_RTL8139_TX_SLOTS);
    m->tx_status[slot] = status;
    ++m->tx_complete_calls;
}

static struct kira_rtl8139_platform platform(mock_t *m)
{
    struct kira_rtl8139_platform p;
    memset(&p, 0, sizeof p);
    p.context = m;
    p.pci_vendor_id = 0x10ec;
    p.pci_device_id = 0x8139;
    p.io_region_size = 0x54;
    p.in8 = in8; p.in16 = in16; p.in32 = in32;
    p.out8 = out8; p.out16 = out16; p.out32 = out32;
    p.dma_alloc = dma_alloc; p.dma_free = dma_free;
    p.cache_invalidate = cache_invalidate; p.cache_clean = cache_clean;
    p.dma_read_barrier = dma_read_barrier;
    p.dma_write_barrier = dma_write_barrier;
    p.delay_us = delay_us; p.receive = receive;
    p.transmit_complete = tx_complete;
    return p;
}
static void init_mock(mock_t *m)
{
    memset(m, 0, sizeof *m);
    m->next_phys = 0x100000;
    m->regs[0] = 0x02; m->regs[1] = 0x11; m->regs[2] = 0x22;
    m->regs[3] = 0x33; m->regs[4] = 0x44; m->regs[5] = 0x55;
    m->regs[REG_COMMAND] = CMD_BUFE;
    m->regs[0x64] = (uint8_t)BMSR_LINK;
}
static void rx_packet(mock_t *m, uint16_t offset, uint16_t status,
                      uint16_t raw_length, const uint8_t *payload,
                      size_t payload_length)
{
    m->rx[offset] = (uint8_t)status;
    m->rx[offset + 1u] = (uint8_t)(status >> 8);
    m->rx[offset + 2u] = (uint8_t)raw_length;
    m->rx[offset + 3u] = (uint8_t)(raw_length >> 8);
    for (size_t i = 0; i < payload_length; ++i)
        m->rx[(offset + 4u + i) & (KIRA_RTL8139_RX_RING_SIZE - 1u)] = payload[i];
    m->regs[REG_COMMAND] &= (uint8_t)~CMD_BUFE;
}

static void test_reject_and_cleanup(void)
{
    mock_t m; struct kira_rtl8139 d; struct kira_rtl8139_platform p;
    init_mock(&m); p = platform(&m); p.pci_device_id = 0x8138;
    assert(kira_rtl8139_init(&d, IO_BASE, &p) != 0 && m.alloc_count == 0);

    init_mock(&m); p = platform(&m); m.reset_stuck = 1;
    assert(kira_rtl8139_init(&d, IO_BASE, &p) == -2);
    assert(m.alloc_count == 0 && m.free_count == 0);

    init_mock(&m); p = platform(&m); m.fail_alloc_at = 3;
    assert(kira_rtl8139_init(&d, IO_BASE, &p) == -4);
    assert(m.alloc_count == 3 && m.free_count == 2 && !d.initialized);

    init_mock(&m); p = platform(&m); m.bad_rx_phys = 1;
    assert(kira_rtl8139_init(&d, IO_BASE, &p) == -3);
    assert(m.alloc_count == 1 && m.free_count == 1 && !d.initialized);
}

static void test_rx_tx_lifecycle(void)
{
    mock_t m; struct kira_rtl8139 d; struct kira_rtl8139_platform p;
    uint8_t payload[60], frame[42];
    init_mock(&m); p = platform(&m);
    memset(&d, 0xa5, sizeof d); /* init must not trust old pointer fields */
    assert(kira_rtl8139_init(&d, IO_BASE, &p) == 0);
    assert(d.initialized && m.free_count == 0 && m.imr == 0);
    assert(m.rx_phys_reg == 0x100000u && m.tsad[0] == 0x200000u);
    assert(kira_rtl8139_link_up(&d) == 1);

    memset(frame, 0x5a, sizeof frame);
    assert(kira_rtl8139_send(&d, frame, 13) == -1);
    assert(kira_rtl8139_send(&d, frame, sizeof frame) == 0);
    assert(d.tx_busy[0] && m.clean_calls > 0 && m.write_barriers > 0);
    assert(memcmp(m.tx[0], frame, sizeof frame) == 0);
    for (size_t i = sizeof frame; i < 60; ++i) assert(m.tx[0][i] == 0);
    m.tsd[0] = TSD_OWN | TSD_TOK;
    assert(kira_rtl8139_poll(&d) == 0);
    assert(!d.tx_busy[0] && m.tx_complete_calls == 1 && m.tx_status[0] == 0);

    assert(kira_rtl8139_send(&d, frame, sizeof frame) == 0);
    m.tsd[0] = TSD_OWN | TSD_TUN | TSD_TABT;
    (void)kira_rtl8139_poll(&d);
    assert(!d.tx_busy[0] && m.tx_complete_calls == 2 && m.tx_status[0] == -1);

    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = (uint8_t)(i + 1u);
    rx_packet(&m, 0, RX_ROK, 64, payload, sizeof payload);
    assert(kira_rtl8139_poll(&d) == 1);
    assert(m.receive_calls == 1 && m.received_length == sizeof payload);
    assert(memcmp(m.received, payload, sizeof payload) == 0);
    assert(m.invalidate_calls >= 3 && m.read_barriers >= 2);

    /* The payload crosses the 8 KiB ring boundary and must be copied in two spans. */
    d.rx_offset = 8176;
    for (size_t i = 0; i < 20; ++i) payload[i] = (uint8_t)(0xa0u + i);
    rx_packet(&m, 8176, RX_ROK, 24, payload, 20);
    assert(kira_rtl8139_poll(&d) == 1);
    assert(m.receive_calls == 2 && m.received_length == 20);
    assert(memcmp(m.received, payload, 20) == 0);

    /* Frame-alignment errors are dropped and consumed, never delivered. */
    rx_packet(&m, d.rx_offset, RX_ROK | RX_FAE, 64, payload, 60);
    assert(kira_rtl8139_poll(&d) == 0);
    assert(m.receive_calls == 2);

    /* Impossible packet lengths reset ring ownership rather than trusting metadata. */
    d.rx_offset = 0;
    rx_packet(&m, 0, RX_ROK, 0xffffu, payload, 0);
    assert(kira_rtl8139_poll(&d) == 0);
    assert(d.rx_offset == 0 && m.last_capr == 0xfff0u);

    m.regs[0x64] = 0;
    assert(kira_rtl8139_link_up(&d) == 0);
    kira_rtl8139_shutdown(&d);
    assert(!d.initialized && m.free_count == KIRA_RTL8139_TX_SLOTS + 1u);
    kira_rtl8139_shutdown(&d);
    assert(m.free_count == KIRA_RTL8139_TX_SLOTS + 1u);
}

int main(void)
{
    test_reject_and_cleanup();
    test_rx_tx_lifecycle();
    puts("rtl8139 clean-room driver: PCI gate, reset, DMA cleanup, RX/TX, wrap and link tests passed");
    return 0;
}
