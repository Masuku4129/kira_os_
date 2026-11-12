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
#define REG_BMSR       0x64u
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
#define BMSR_LINK      (1u << 2)
#define RX_STATUS_FAE  0x0002u
#define RX_STATUS_CRC  0x0004u
#define RX_STATUS_LONG 0x0008u
#define RX_STATUS_RUNT 0x0010u
#define RX_STATUS_ISE  0x0020u

static int rtl_dma_address_valid(uint64_t physical, size_t size)
{
    return physical != 0 && size != 0 && (physical & 0xffu) == 0 &&
           physical <= UINT32_MAX &&
           (uint64_t)(size - 1u) <= (uint64_t)UINT32_MAX - physical;
}

static void rtl_release_buffers(struct kira_rtl8139 *d)
{
    unsigned int i;
    if (!d || !d->platform.dma_free)
        return;
    for (i = 0; i < KIRA_RTL8139_TX_SLOTS; ++i) {
        if (d->tx_buffer[i])
            d->platform.dma_free(d->platform.context, d->tx_buffer[i],
                d->tx_physical[i], KIRA_RTL8139_TX_BUFFER_SIZE);
        d->tx_buffer[i] = 0;
        d->tx_physical[i] = 0;
    }
    if (d->rx_buffer)
        d->platform.dma_free(d->platform.context, d->rx_buffer,
            d->rx_physical,
            KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE);
    d->rx_buffer = 0;
    d->rx_physical = 0;
}

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
    struct kira_rtl8139_platform platform;
    unsigned int i;
    uint32_t tcr;
    int failure = -1;

    if (!d || !p)
        return -1;
    platform = *p;
    if (platform.pci_vendor_id != 0x10ecu ||
        platform.pci_device_id != 0x8139u ||
        !io_base || io_base > UINT16_MAX - (REG_BMSR + 1u) ||
        platform.io_region_size <= REG_BMSR + 1u ||
        !platform.in8 || !platform.in16 || !platform.in32 ||
        !platform.out8 || !platform.out16 || !platform.out32 ||
        !platform.dma_alloc || !platform.dma_free ||
        !platform.cache_invalidate || !platform.cache_clean ||
        !platform.dma_read_barrier || !platform.dma_write_barrier ||
        !platform.delay_us || !platform.receive)
        return -1;

    /* The caller may pass an uninitialized object; never inspect stale DMA pointers. */
    for (i = 0; i < sizeof(*d); ++i)
        ((uint8_t *)d)[i] = 0;
    d->platform = platform;
    d->io_base = io_base;

    /* Bring the controller out of power-down and request a software reset. */
    d->platform.out8(d->platform.context, port(d, REG_CONFIG1), 0x00);
    d->platform.out8(d->platform.context, port(d, REG_COMMAND), CMD_RESET);
    for (i = 0; i < RESET_TIMEOUT; ++i) {
        if (!(d->platform.in8(d->platform.context, port(d, REG_COMMAND)) &
              CMD_RESET))
            break;
        d->platform.delay_us(d->platform.context, 1);
    }
    if (i == RESET_TIMEOUT) {
        failure = -2;
        goto fail;
    }

    for (i = 0; i < 6; ++i)
        d->mac[i] = d->platform.in8(d->platform.context,
                                    port(d, (uint16_t)(REG_MAC0 + i)));
    if ((d->mac[0] & 1u) ||
        !(d->mac[0] | d->mac[1] | d->mac[2] |
          d->mac[3] | d->mac[4] | d->mac[5])) {
        failure = -2;
        goto fail;
    }

    d->rx_buffer = d->platform.dma_alloc(d->platform.context,
        KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE,
        256, &d->rx_physical);
    if (!d->rx_buffer || !rtl_dma_address_valid(d->rx_physical,
        KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE)) {
        failure = -3;
        goto fail;
    }

    for (i = 0; i < KIRA_RTL8139_TX_SLOTS; ++i) {
        d->tx_buffer[i] = d->platform.dma_alloc(d->platform.context,
            KIRA_RTL8139_TX_BUFFER_SIZE, 256, &d->tx_physical[i]);
        if (!d->tx_buffer[i] ||
            !rtl_dma_address_valid(d->tx_physical[i],
                                   KIRA_RTL8139_TX_BUFFER_SIZE)) {
            failure = -4;
            goto fail;
        }
        d->tx_busy[i] = 0;
    }

    d->platform.cache_clean(d->platform.context, d->rx_buffer,
        KIRA_RTL8139_RX_RING_SIZE + KIRA_RTL8139_RX_WRAP_SIZE);
    d->platform.dma_write_barrier(d->platform.context);

    for (i = 0; i < KIRA_RTL8139_TX_SLOTS; ++i)
        d->platform.out32(d->platform.context,
            port(d, (uint16_t)(REG_TSAD0 + i * 4)),
            (uint32_t)d->tx_physical[i]);

    d->platform.out32(d->platform.context, port(d, REG_RBSTART),
                      (uint32_t)d->rx_physical);
    d->platform.out16(d->platform.context, port(d, REG_CAPR), 0xfff0u);

    /* Accept unicast-to-us, multicast and broadcast; use the 8 KiB ring. */
    d->platform.out32(d->platform.context, port(d, REG_RCR),
        RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_MXDMA_1024 |
        RCR_RBLEN_8K | RCR_RXFTH_NONE);

    tcr = TCR_IFG_NORMAL | TCR_DMA_1024;
    d->platform.out32(d->platform.context, port(d, REG_TCR), tcr);

    rtl_ack_interrupts(d, 0xffffu);
    /* This driver is polled; enabling IMR without an IRQ owner can storm INTx. */
    d->platform.out16(d->platform.context, port(d, REG_IMR), 0);
    d->platform.out8(d->platform.context, port(d, REG_COMMAND),
                     CMD_RX_ENABLE | CMD_TX_ENABLE);
    d->initialized = 1;
    return 0;

fail:
    if (d->platform.out8 && d->io_base)
        d->platform.out8(d->platform.context, port(d, REG_COMMAND), 0);
    rtl_release_buffers(d);
    return failure;
}

static void rtl_advance_rx(struct kira_rtl8139 *d, uint16_t raw_length)
{
    d->rx_offset = (uint16_t)((d->rx_offset + 4u + raw_length + 3u) & ~3u);
    d->rx_offset &= (KIRA_RTL8139_RX_RING_SIZE - 1u);
    d->platform.out16(d->platform.context, port(d, REG_CAPR),
                      (uint16_t)(d->rx_offset - 16u));
}

static int rtl_receive_one(struct kira_rtl8139 *d)
{
    uint8_t *ring = (uint8_t *)d->rx_buffer;
    uint8_t frame[KIRA_RTL8139_MAX_FRAME];
    uint16_t command = d->platform.in8(d->platform.context,
                                       port(d, REG_COMMAND));
    uint16_t status, raw_length, length, offset, data_offset, first;
    unsigned int i;

    if (command & CMD_BUFE)
        return 0;

    offset = d->rx_offset;
    d->platform.cache_invalidate(d->platform.context, ring + offset, 4);
    d->platform.dma_read_barrier(d->platform.context);
    status = (uint16_t)ring[offset] |
             ((uint16_t)ring[offset + 1u] << 8);
    raw_length = (uint16_t)ring[offset + 2u] |
                 ((uint16_t)ring[offset + 3u] << 8);

    /* An impossible length means ring metadata is no longer trustworthy. */
    if (raw_length < 4u || raw_length > KIRA_RTL8139_MAX_FRAME + 4u) {
        d->rx_offset = 0;
        d->platform.out16(d->platform.context, port(d, REG_CAPR), 0xfff0u);
        return 1;
    }

    if (!(status & RX_STATUS_ROK) ||
        (status & (RX_STATUS_FAE | RX_STATUS_CRC | RX_STATUS_LONG |
                   RX_STATUS_RUNT | RX_STATUS_ISE))) {
        rtl_advance_rx(d, raw_length);
        return 1;
    }

    length = (uint16_t)(raw_length - 4u); /* Strip the FCS. */
    data_offset = (uint16_t)((offset + 4u) &
                             (KIRA_RTL8139_RX_RING_SIZE - 1u));
    first = (uint16_t)(KIRA_RTL8139_RX_RING_SIZE - data_offset);
    if (first > length)
        first = length;
    if (first)
        d->platform.cache_invalidate(d->platform.context, ring + data_offset,
                                     first);
    if (length > first)
        d->platform.cache_invalidate(d->platform.context, ring,
                                     (size_t)(length - first));
    d->platform.dma_read_barrier(d->platform.context);

    for (i = 0; i < first; ++i)
        frame[i] = ring[data_offset + i];
    for (i = first; i < length; ++i)
        frame[i] = ring[i - first];

    rtl_advance_rx(d, raw_length);
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
        if (status & TSD_OWN) {
            d->platform.dma_read_barrier(d->platform.context);
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

    d->platform.cache_clean(d->platform.context, d->tx_buffer[slot], length);
    d->platform.dma_write_barrier(d->platform.context);

    d->tx_busy[slot] = 1;
    d->platform.out32(d->platform.context,
        port(d, (uint16_t)(REG_TSD0 + slot * 4)),
        TSD_ERTXTH | (uint32_t)length);

    return (int)slot;
}

int kira_rtl8139_link_up(struct kira_rtl8139 *d)
{
    if (!d || !d->initialized)
        return 0;
    return (d->platform.in16(d->platform.context, port(d, REG_BMSR)) &
            BMSR_LINK) != 0;
}

void kira_rtl8139_shutdown(struct kira_rtl8139 *d)
{
    if (!d)
        return;

    if (d->io_base && d->platform.out16 && d->platform.out8) {
        d->platform.out16(d->platform.context, port(d, REG_IMR), 0);
        d->platform.out8(d->platform.context, port(d, REG_COMMAND), 0);
        rtl_ack_interrupts(d, 0xffffu);
        if (d->platform.dma_write_barrier)
            d->platform.dma_write_barrier(d->platform.context);
    }

    rtl_release_buffers(d);
    d->initialized = 0;
}
