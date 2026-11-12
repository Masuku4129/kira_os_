/*
 * RTL8139 PCI Ethernet controller driver.
 *
 * Register definitions and device behavior follow the public RTL8139
 * programming specification. The platform supplies port I/O, DMA allocation,
 * cache maintenance and packet delivery. The driver uses the 8 KiB receive
 * ring and four independent transmit descriptors.
 */

#include "rtl8139.h"

#define REG_MAC0       0x00u
#define REG_TSD0       0x10u
#define REG_TSAD0      0x20u
#define REG_RBSTART    0x30u
#define REG_COMMAND    0x37u
#define REG_CAPR       0x38u
#define REG_CBR        0x3au
#define REG_IMR        0x3cu
#define REG_ISR        0x3eu
#define REG_TCR        0x40u
#define REG_RCR        0x44u
#define REG_CONFIG1    0x52u

#define CMD_RESET      0x10u
#define CMD_RX_ENABLE  0x08u
#define CMD_TX_ENABLE  0x04u
#define CMD_BUFE       0x01u

#define ISR_ROK        0x0001u
#define ISR_RER        0x0002u
#define ISR_TOK        0x0004u
#define ISR_TER        0x0008u
#define ISR_RXOVW      0x0010u
#define ISR_PUN        0x0020u
#define ISR_FOVW       0x0040u
#define ISR_TIMEOUT    0x4000u
#define ISR_SERR       0x8000u

#define TSD_OWN        (1u << 13)
#define TSD_TOK        (1u << 15)
#define TSD_TUN        (1u << 14)
#define TSD_TABT       (1u << 30)
#define TSD_SIZE_MASK  0x1fffu
#define TSD_ERTXTH     (0x3fu << 16)

#define RCR_AAP        (1u << 0)
#define RCR_APM        (1u << 1)
#define RCR_AM         (1u << 2)
#define RCR_AB         (1u << 3)
#define RCR_WRAP       (1u << 7)
#define RCR_MXDMA_1024 (6u << 8)
#define RCR_RBLEN_8K   (0u << 11)
#define RCR_RXFTH_NONE (7u << 13)

#define TCR_IFG_NORMAL (3u << 24)
#define TCR_DMA_1024   (6u << 8)

#define RESET_TIMEOUT  100000u
#define TX_TIMEOUT     100000u
#define RX_STATUS_ROK  0x0001u
#define RX_STATUS_FAE  0x0002u
#define RX_STATUS_CRC  0x0004u
#define RX_STATUS_LONG 0x0008u
#define RX_STATUS_RUNT 0x0010u
#define RX_STATUS_ISE  0x0020u

static uint16_t port(struct kira_rtl8139 *d, uint16_t reg)
{
    return (uint16_t)(d->io_base + reg);
}

static void rtl_delay(struct kira_rtl8139 *d, uint32_t us)
{
    if (d->platform.delay_us)
        d->platform.delay_us(d->platform.context, us);
}

static void rtl_ack_interrupts(struct kira_rtl8139 *d, uint16_t bits)
{
    d->platform.out16(d->platform.context, port(d, REG_ISR), bits);
}

int kira_rtl8139_init(struct kira_rtl8139 *d, uint16_t io_base,
                      const struct kira_rtl8139_platform *p)
{
    unsigned int i;
    uint32_t tcr;

    if (!d || !p || !io_base || !p->in8 || !p->in16 || !p->in32 ||
        !p->out8 || !p->out16 || !p->out32 || !p->dma_alloc ||
        !p->dma_free || !p->receive)
        return -1;

    d->platform = *p;
    d->io_base = io_base;
    d->rx_offset = 0;
    d->initialized = 0;

    /* Bring the controller out of power-down and request a software reset. */
    d->platform.out8(d->platform.context, port(d, REG_CONFIG1), 0x00);
    d->platform.out8(d->platform.context, port(d, REG_COMMAND), CMD_RESET);

    for (i = 0; i < RESET_TIMEOUT; ++i) {
        if (!(d->platform.in8(d->platform.context, port(d, REG_COMMAND)) &
              CMD_RESET))
            break;
        rtl_delay(d, 1);
    }
    if (i == RESET_TIMEOUT)
        return -2;

    for (i = 0; i < 6; ++i)
        d->mac[i] = d->platform.in8(d->platform.context,
                                    port(d, (uint16_t)(REG_MAC0 + i)));

    d->rx_buffer = d->platform.dma_alloc(d->platform.context,
        KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE,
        256, &d->rx_physical);
    if (!d->rx_buffer || !d->rx_physical ||
        (d->rx_physical & 0xffu))
        return -3;

    if (d->platform.cache_clean)
        d->platform.cache_clean(d->platform.context, d->rx_buffer,
            KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE);

    for (i = 0; i < KIRA_RTL8139_TX_SLOTS; ++i) {
        d->tx_buffer[i] = d->platform.dma_alloc(d->platform.context,
            KIRA_RTL8139_TX_BUFFER_SIZE, 256, &d->tx_physical[i]);
        if (!d->tx_buffer[i] || !d->tx_physical[i] ||
            (d->tx_physical[i] & 0xffu))
            return -4;
        d->tx_busy[i] = 0;
        d->platform.out32(d->platform.context,
            port(d, (uint16_t)(REG_TSAD0 + i * 4)),
            (uint32_t)d->tx_physical[i]);
    }

    d->platform.out32(d->platform.context, port(d, REG_RBSTART),
                      (uint32_t)d->rx_physical);
    d->platform.out16(d->platform.context, port(d, REG_CAPR), 0xfff0u);

    /* Enable broadcast, physical-match and multicast reception. */
    d->platform.out32(d->platform.context, port(d, REG_RCR),
        RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_MXDMA_1024 |
        RCR_RBLEN_8K | RCR_RXFTH_NONE);

    tcr = TCR_IFG_NORMAL | TCR_DMA_1024;
    d->platform.out32(d->platform.context, port(d, REG_TCR), tcr);

    rtl_ack_interrupts(d, 0xffffu);
    d->platform.out16(d->platform.context, port(d, REG_IMR),
        ISR_ROK | ISR_RER | ISR_TOK | ISR_TER | ISR_RXOVW | ISR_FOVW |
        ISR_TIMEOUT | ISR_SERR);

    d->platform.out8(d->platform.context, port(d, REG_COMMAND),
                     CMD_RX_ENABLE | CMD_TX_ENABLE);
    d->initialized = 1;
    return 0;
}

static int rtl_receive_one(struct kira_rtl8139 *d)
{
    uint8_t *ring = (uint8_t *)d->rx_buffer;
    uint16_t command = d->platform.in8(d->platform.context,
                                       port(d, REG_COMMAND));
    uint16_t status;
    uint16_t length;
    uint16_t offset;
    uint8_t frame[KIRA_RTL8139_MAX_FRAME];
    unsigned int i;

    if (command & CMD_BUFE)
        return 0;

    offset = d->rx_offset;
    if (d->platform.cache_invalidate)
        d->platform.cache_invalidate(d->platform.context,
            ring + offset, 4);

    status = (uint16_t)ring[offset] |
             ((uint16_t)ring[(offset + 1u) & 0x1fffu] << 8);
    length = (uint16_t)ring[(offset + 2u) & 0x1fffu] |
             ((uint16_t)ring[(offset + 3u) & 0x1fffu] << 8);

    if (!(status & RX_STATUS_ROK) || (status & (RX_STATUS_CRC |
        RX_STATUS_LONG | RX_STATUS_RUNT | RX_STATUS_ISE)) ||
        length < 4 || length > KIRA_RTL8139_MAX_FRAME + 4) {
        d->rx_offset = (uint16_t)((offset + 4u + length + 3u) & ~3u);
        d->rx_offset &= (KIRA_RTL8139_RX_RING_SIZE - 1u);
        d->platform.out16(d->platform.context, port(d, REG_CAPR),
                          (uint16_t)(d->rx_offset - 16u));
        return 1;
    }

    /* Length includes the four-byte FCS, which is not delivered to the stack. */
    length = (uint16_t)(length - 4u);
    for (i = 0; i < length; ++i)
        frame[i] = ring[(offset + 4u + i) &
                        (KIRA_RTL8139_RX_RING_SIZE - 1u)];

    d->rx_offset = (uint16_t)((offset + 4u + length + 4u + 3u) & ~3u);
    d->rx_offset &= (KIRA_RTL8139_RX_RING_SIZE - 1u);
    d->platform.out16(d->platform.context, port(d, REG_CAPR),
                      (uint16_t)(d->rx_offset - 16u));

    d->platform.receive(d->platform.context, frame, length);
    return 1;
}

static void rtl_reap_tx(struct kira_rtl8139 *d)
{
    unsigned int i;

    for (i = 0; i < KIRA_RTL8139_TX_SLOTS; ++i) {
        uint32_t status;
        if (!d->tx_busy[i])
            continue;

        status = d->platform.in32(d->platform.context,
            port(d, (uint16_t)(REG_TSD0 + i * 4)));
        if (status & (TSD_TOK | TSD_TUN | TSD_TABT)) {
            d->tx_busy[i] = 0;
            if (d->platform.transmit_complete)
                d->platform.transmit_complete(d->platform.context, i,
                                               (status & TSD_TOK) ? 0 : -1);
        }
    }
}

int kira_rtl8139_poll(struct kira_rtl8139 *d)
{
    uint16_t status;
    int work = 0;
    unsigned int budget = 64;

    if (!d || !d->initialized)
        return -1;

    status = d->platform.in16(d->platform.context, port(d, REG_ISR));
    if (status) {
        rtl_ack_interrupts(d, status);
        if (status & (ISR_RXOVW | ISR_FOVW)) {
            /* Reset receive pointer after overrun to recover ring ownership. */
            d->rx_offset = 0;
            d->platform.out16(d->platform.context, port(d, REG_CAPR), 0xfff0u);
        }
    }

    rtl_reap_tx(d);

    while (budget-- && rtl_receive_one(d) > 0)
        ++work;

    return work;
}

int kira_rtl8139_send(struct kira_rtl8139 *d,
                      const void *frame, size_t length)
{
    unsigned int slot;
    unsigned int i;

    if (!d || !d->initialized || !frame ||
        length < 14 || length > KIRA_RTL8139_MAX_FRAME)
        return -1;

    rtl_reap_tx(d);

    for (slot = 0; slot < KIRA_RTL8139_TX_SLOTS; ++slot)
        if (!d->tx_busy[slot])
            break;
    if (slot == KIRA_RTL8139_TX_SLOTS)
        return -2;

    for (i = 0; i < length; ++i)
        ((uint8_t *)d->tx_buffer[slot])[i] = ((const uint8_t *)frame)[i];

    if (length < 60) {
        for (i = (unsigned int)length; i < 60; ++i)
            ((uint8_t *)d->tx_buffer[slot])[i] = 0;
        length = 60;
    }

    if (d->platform.cache_clean)
        d->platform.cache_clean(d->platform.context, d->tx_buffer[slot],
                                length);

    d->tx_busy[slot] = 1;
    d->platform.out32(d->platform.context,
        port(d, (uint16_t)(REG_TSD0 + slot * 4)),
        TSD_ERTXTH | (uint32_t)length);

    return (int)slot;
}

void kira_rtl8139_shutdown(struct kira_rtl8139 *d)
{
    unsigned int i;

    if (!d)
        return;

    if (d->initialized) {
        d->platform.out16(d->platform.context, port(d, REG_IMR), 0);
        d->platform.out8(d->platform.context, port(d, REG_COMMAND), 0);
        rtl_ack_interrupts(d, 0xffffu);
    }

    for (i = 0; i < KIRA_RTL8139_TX_SLOTS; ++i) {
        if (d->tx_buffer[i] && d->platform.dma_free)
            d->platform.dma_free(d->platform.context, d->tx_buffer[i],
                d->tx_physical[i], KIRA_RTL8139_TX_BUFFER_SIZE);
        d->tx_buffer[i] = 0;
    }

    if (d->rx_buffer && d->platform.dma_free)
        d->platform.dma_free(d->platform.context, d->rx_buffer,
            d->rx_physical,
            KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE);

    d->rx_buffer = 0;
    d->initialized = 0;
}
