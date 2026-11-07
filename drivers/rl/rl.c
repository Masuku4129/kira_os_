/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * RTL8139 clean-room driver for KIRA.
 *
 * Register-level implementation based on the public Realtek RTL8139
 * programming model. It intentionally does not include or derive from
 * FreeBSD rl(4), Linux 8139too, or another operating-system driver.
 *
 * DMA is requested only through KDRV's isolated DMA allocator. There is no
 * unsafe physical-address fallback when the IOMMU policy denies DMA.
 */
#include <kdrv_bsd.h>
#include "kira_rtl8139_core.h"

#define RTL_VENDOR_ID       0x10ecu
#define RTL_DEVICE_ID       0x8139u
#define RTL_REG_IDR0        0x00u
#define RTL_REG_MAR0        0x08u
#define RTL_REG_TSD0        0x10u
#define RTL_REG_TSAD0       0x20u
#define RTL_REG_RBSTART     0x30u
#define RTL_REG_CR          0x37u
#define RTL_REG_CAPR        0x38u
#define RTL_REG_CBR         0x3au
#define RTL_REG_IMR         0x3cu
#define RTL_REG_ISR         0x3eu
#define RTL_REG_TCR         0x40u
#define RTL_REG_RCR         0x44u
#define RTL_REG_CONFIG1     0x52u
#define RTL_REG_MSR         0x58u

#define RTL_CR_RESET        0x10u
#define RTL_CR_RX_ENABLE    0x08u
#define RTL_CR_TX_ENABLE    0x04u
#define RTL_MSR_LINK_DOWN   0x04u

#define RTL_RCR_APM         (1u << 1)
#define RTL_RCR_AM          (1u << 2)
#define RTL_RCR_AB          (1u << 3)
#define RTL_RCR_WRAP        (1u << 7)
#define RTL_RCR_MXDMA_1024  (6u << 8)
#define RTL_RCR_RBLEN_64K   (3u << 11)
#define RTL_RCR_RXFTH_1024  (6u << 13)

#define RTL_TCR_MXDMA_1024  (6u << 8)
#define RTL_TCR_IFG_NORMAL  (3u << 24)

#define RTL_TSD_TOK         (1u << 15)
#define RTL_TSD_TUN         (1u << 14)
#define RTL_TSD_CRS         (1u << 16)
#define RTL_TSD_TABT        (1u << 30)
#define RTL_TSD_OWC         (1u << 31)
#define RTL_TSD_ERRORS      (RTL_TSD_TUN | RTL_TSD_CRS | RTL_TSD_TABT | RTL_TSD_OWC)

#define RTL_RX_ALLOC_BYTES  (KIRA_RL_RX_RING_BYTES + KIRA_RL_RX_WRAP_BYTES)
#define RTL_TX_ALLOC_BYTES  (KIRA_RL_TX_SLOTS * KIRA_RL_TX_BUF_BYTES)
#define RTL_TX_MIN_FRAME    60u
#define RTL_POLL_LIMIT      64u

typedef struct {
    bus_space_handle_t io;
    struct kd_dmabuf rx_dma;
    struct kd_dmabuf tx_dma;
    struct ifnet net;
    kira_rl_rx_state_t rx;
    kira_rl_tx_state_t tx;
    u8 mac[6];
    u8 link_up;
    u32 poll_count;
} rtl8139_t;

static rtl8139_t g_rtl;

static u8 rtl_read8(rtl8139_t *s, u32 reg) {
    return bus_space_read_1(s->io, reg);
}
static u16 rtl_read16(rtl8139_t *s, u32 reg) {
    return bus_space_read_2(s->io, reg);
}
static u32 rtl_read32(rtl8139_t *s, u32 reg) {
    return bus_space_read_4(s->io, reg);
}
static void rtl_write8(rtl8139_t *s, u32 reg, u8 value) {
    bus_space_write_1(s->io, reg, value);
}
static void rtl_write16(rtl8139_t *s, u32 reg, u16 value) {
    bus_space_write_2(s->io, reg, value);
}
static void rtl_write32(rtl8139_t *s, u32 reg, u32 value) {
    bus_space_write_4(s->io, reg, value);
}

static int rtl_mac_valid(const u8 mac[6]) {
    u8 any = 0, all_ff = 1;
    if (mac[0] & 1u) return 0;
    for (u32 i = 0; i < 6; i++) {
        any |= mac[i];
        if (mac[i] != 0xffu) all_ff = 0;
    }
    return any != 0 && !all_ff;
}

static int rtl_find_memory_bar(const kdrv_info_t *info, u32 *bar_out) {
    if (!info || !bar_out) return -1;
    for (u32 i = 0; i < 6; i++) {
        if (!(info->bar[i].flags & KDRV_BAR_IO) &&
            info->bar[i].size >= 0x100u) {
            *bar_out = i;
            return 0;
        }
    }
    return -2;
}

static int rtl_reset(rtl8139_t *s) {
    rtl_write16(s, RTL_REG_IMR, 0);
    rtl_write16(s, RTL_REG_ISR, 0xffffu);
    rtl_write8(s, RTL_REG_CONFIG1, 0);
    rtl_write8(s, RTL_REG_CR, RTL_CR_RESET);
    for (u32 i = 0; i < 100000u; i++) {
        if (!(rtl_read8(s, RTL_REG_CR) & RTL_CR_RESET)) return 0;
        __asm__ volatile("" ::: "memory");
    }
    return -1;
}

static int rtl_dma_setup(rtl8139_t *s) {
    i64 rc = kd_dma_alloc(&s->rx_dma, RTL_RX_ALLOC_BYTES, 32);
    if (rc) {
        kd_puts("rtl8139: isolated RX DMA allocation denied\n");
        return -1;
    }
    rc = kd_dma_alloc(&s->tx_dma, RTL_TX_ALLOC_BYTES, 32);
    if (rc) {
        kd_puts("rtl8139: isolated TX DMA allocation denied\n");
        return -2;
    }
    if (!s->rx_dma.va || !s->tx_dma.va ||
        s->rx_dma.size < RTL_RX_ALLOC_BYTES ||
        s->tx_dma.size < RTL_TX_ALLOC_BYTES ||
        !kira_rl_dma32_valid(s->rx_dma.pa, RTL_RX_ALLOC_BYTES) ||
        !kira_rl_dma32_valid(s->tx_dma.pa, RTL_TX_ALLOC_BYTES)) {
        kd_puts("rtl8139: DMA allocator returned an invalid 32-bit range\n");
        return -3;
    }
    kd_memset(s->rx_dma.va, 0, RTL_RX_ALLOC_BYTES);
    kd_memset(s->tx_dma.va, 0, RTL_TX_ALLOC_BYTES);
    return 0;
}

static int rtl_hw_start(rtl8139_t *s) {
    if (rtl_reset(s) != 0) {
        kd_puts("rtl8139: reset timeout\n");
        return -1;
    }

    for (u32 i = 0; i < 6; i++)
        s->mac[i] = rtl_read8(s, RTL_REG_IDR0 + i);
    if (!rtl_mac_valid(s->mac)) {
        kd_puts("rtl8139: invalid station address\n");
        return -2;
    }

    rtl_write32(s, RTL_REG_RBSTART, (u32)s->rx_dma.pa);
    for (u32 i = 0; i < KIRA_RL_TX_SLOTS; i++)
        rtl_write32(s, RTL_REG_TSAD0 + i * 4u,
                    (u32)(s->tx_dma.pa + (u64)i * KIRA_RL_TX_BUF_BYTES));

    /* Accept station, broadcast, and multicast frames; use the 64 KiB ring. */
    rtl_write32(s, RTL_REG_MAR0, 0xffffffffu);
    rtl_write32(s, RTL_REG_MAR0 + 4u, 0xffffffffu);
    rtl_write32(s, RTL_REG_RCR, RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB |
                RTL_RCR_WRAP | RTL_RCR_MXDMA_1024 | RTL_RCR_RBLEN_64K |
                RTL_RCR_RXFTH_1024);
    rtl_write32(s, RTL_REG_TCR, RTL_TCR_MXDMA_1024 | RTL_TCR_IFG_NORMAL);

    s->rx.offset = 0;
    s->rx.packets = 0;
    s->rx.dropped = 0;
    s->tx.producer = 0;
    s->tx.consumer = 0;
    s->tx.used = 0;
    s->tx.completed = 0;
    s->tx.errors = 0;
    for (u32 i = 0; i < KIRA_RL_TX_SLOTS; i++) s->tx.busy[i] = 0;

    rtl_write16(s, RTL_REG_CAPR, 0xfff0u);
    rtl_write16(s, RTL_REG_ISR, 0xffffu);
    rtl_write16(s, RTL_REG_IMR, 0); /* polling mode; no IRQ capability needed */
    bus_space_barrier();
    rtl_write8(s, RTL_REG_CR, RTL_CR_RX_ENABLE | RTL_CR_TX_ENABLE);
    bus_space_barrier();
    return 0;
}

static void rtl_tx_reap(rtl8139_t *s) {
    while (s->tx.used) {
        u32 slot = s->tx.consumer;
        u32 status = rtl_read32(s, RTL_REG_TSD0 + slot * 4u);
        if (!(status & (RTL_TSD_TOK | RTL_TSD_ERRORS))) break;
        if (kira_rl_tx_reap(&s->tx, slot, status,
                            RTL_TSD_TOK, RTL_TSD_ERRORS) != 1) break;
        if ((status & RTL_TSD_TOK) && !(status & RTL_TSD_ERRORS))
            s->net.if_opackets++;
        else
            s->net.if_oerrors++;
    }
}

static void rtl_tx_start(rtl8139_t *s) {
    while (s->tx.used < KIRA_RL_TX_SLOTS && if_tx_pending(&s->net)) {
        u32 slot;
        if (kira_rl_tx_reserve(&s->tx, &slot) != 0) break;

        u8 *dst = (u8 *)s->tx_dma.va + slot * KIRA_RL_TX_BUF_BYTES;
        u32 length = if_dequeue(&s->net, dst, KDRV_NET_MAXFRAME);
        if (!length) {
            (void)kira_rl_tx_complete(&s->tx, slot, 0);
            s->net.if_oerrors++;
            break;
        }
        if (kira_rl_tx_length(length) != 0) {
            (void)kira_rl_tx_complete(&s->tx, slot, 0);
            s->net.if_oerrors++;
            continue;
        }
        u32 wire_length = length < RTL_TX_MIN_FRAME ? RTL_TX_MIN_FRAME : length;
        if (wire_length > length) kd_memset(dst + length, 0, wire_length - length);

        bus_space_barrier();
        rtl_write32(s, RTL_REG_TSD0 + slot * 4u, wire_length);
        bus_space_barrier();
    }
}

static void rtl_rx_poll(rtl8139_t *s) {
    u8 header[4];
    u8 frame[KDRV_NET_MAXFRAME];
    int delivered = 0;
    u16 cbr = rtl_read16(s, RTL_REG_CBR);
    cbr = (u16)(cbr & ~3u);

    for (u32 n = 0; n < RTL_POLL_LIMIT && s->rx.offset != cbr; n++) {
        u32 start = s->rx.offset;
        if (kira_rl_ring_copy_out((const u8 *)s->rx_dma.va,
                                  KIRA_RL_RX_RING_BYTES,
                                  start, header, sizeof header) != 0) {
            s->rx.dropped++;
            s->net.if_ierrors++;
            s->rx.offset = cbr;
            rtl_write16(s, RTL_REG_CAPR, (u16)(s->rx.offset - 16u));
            break;
        }

        kira_rl_rx_info_t info;
        if (kira_rl_rx_decode(header, sizeof header, &info) != 0) {
            /* A malformed descriptor is not trusted; discard to the HW cursor. */
            s->rx.dropped++;
            s->net.if_ierrors++;
            s->rx.offset = cbr;
            rtl_write16(s, RTL_REG_CAPR, (u16)(s->rx.offset - 16u));
            break;
        }

        if (!info.accepted) s->net.if_ierrors++;
        int accepted = info.accepted && info.payload_length >= 14u &&
                       info.payload_length <= KDRV_NET_MAXFRAME;
        if (accepted &&
            kira_rl_ring_copy_out((const u8 *)s->rx_dma.va,
                                  KIRA_RL_RX_RING_BYTES,
                                  start + 4u, frame, info.payload_length) != 0)
            accepted = 0;

        if (accepted) {
            if (if_input(&s->net, frame, info.payload_length) == 0)
                delivered = 1;
            else
                accepted = 0;
        }

        int accounted = kira_rl_rx_account(&s->rx, info.status,
                                           info.wire_length, accepted);
        if (accounted < 0) {
            s->rx.dropped++;
            s->net.if_ierrors++;
            s->rx.offset = cbr;
            rtl_write16(s, RTL_REG_CAPR, (u16)(s->rx.offset - 16u));
            break;
        }

        rtl_write16(s, RTL_REG_CAPR, (u16)(s->rx.offset - 16u));
        cbr = rtl_read16(s, RTL_REG_CBR);
        cbr = (u16)(cbr & ~3u);
    }
    if (delivered) kdrv_net_kick();
}

static void rtl_link_poll(rtl8139_t *s) {
    int up = (rtl_read8(s, RTL_REG_MSR) & RTL_MSR_LINK_DOWN) == 0;
    if ((u8)up != s->link_up) {
        s->link_up = (u8)up;
        if_link_state_change(&s->net, up);
    }
}

int main(void) {
    kdrv_info_t info;
    u32 bar = 0;
    i64 err = 0;

    kd_puts("rtl8139: clean-room KIRA driver starting\n");
    if (kdrv_info(&info) != 0 || info.vendor != RTL_VENDOR_ID ||
        info.device != RTL_DEVICE_ID) {
        kd_puts("rtl8139: device identity mismatch\n");
        return 1;
    }

    u32 required = CAP_PCIDEV | CAP_MMIO | CAP_DMA | CAP_NETDEV;
    if ((info.granted & required) != required) {
        kd_puts("rtl8139: missing PCI/MMIO/isolated-DMA/network capability\n");
        return 2;
    }
    if (rtl_find_memory_bar(&info, &bar) != 0) {
        kd_puts("rtl8139: no usable memory BAR\n");
        return 3;
    }

    i64 command = kdrv_cfg_read(0x04u, 2u);
    if (command < 0 ||
        kdrv_cfg_write(0x04u, 2u, (u32)command | 0x0006u | 0x0400u) != 0) {
        kd_puts("rtl8139: cannot enable PCI memory/bus-master mode\n");
        return 4;
    }

    g_rtl.io = bus_space_map(bar, 0x100u, &err);
    if (err || !g_rtl.io) {
        kd_puts("rtl8139: BAR mapping failed\n");
        return 5;
    }
    if (rtl_dma_setup(&g_rtl) != 0) return 6;
    if (rtl_hw_start(&g_rtl) != 0) return 7;

    for (u32 i = 0; i < 6; i++) g_rtl.net.if_lladdr[i] = g_rtl.mac[i];
    g_rtl.net.if_softc = &g_rtl;
    if_attach(&g_rtl.net) != 0) {
        kd_puts("rtl8139: network interface registration failed\n");
        rtl_write8(&g_rtl, RTL_REG_CR, 0);
        return 8;
    }

    rtl_link_poll(&g_rtl);
    for (;;) {
        i64 event = kd_wait(1u);
        if (event == KE_INTR) break;
        rtl_tx_reap(&g_rtl);
        rtl_rx_poll(&g_rtl);
        rtl_tx_start(&g_rtl);
        if (++g_rtl.poll_count >= 50u) {
            g_rtl.poll_count = 0;
            rtl_link_poll(&g_rtl);
        }
    }

    rtl_write16(&g_rtl, RTL_REG_IMR, 0);
    rtl_write8(&g_rtl, RTL_REG_CR, 0);
    if_link_state_change(&g_rtl.net, 0);
    return 0;
}
