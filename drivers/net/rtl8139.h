/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Kira OS authors. Original implementation from the public Realtek RTL8139 programming specification. */
#ifndef KIRA_RTL8139_H
#define KIRA_RTL8139_H

#include <stddef.h>
#include <stdint.h>

#define KIRA_RTL8139_RX_RING_SIZE 8192u
#define KIRA_RTL8139_RX_WRAP_SIZE 16u
#define KIRA_RTL8139_TX_SLOTS 4u
#define KIRA_RTL8139_TX_BUFFER_SIZE 2048u
#define KIRA_RTL8139_MAX_FRAME 1518u
/* Регистры доступны либо по портам (BAR0), либо по памяти (BAR1) - смещения одни; функции in и out платформы получают io_base + смещение. */

struct kira_rtl8139_platform {
    void *context;
    uint8_t (*in8)(void *context, uint16_t port);
    uint16_t (*in16)(void *context, uint16_t port);
    uint32_t (*in32)(void *context, uint16_t port);
    void (*out8)(void *context, uint16_t port, uint8_t value);
    void (*out16)(void *context, uint16_t port, uint16_t value);
    void (*out32)(void *context, uint16_t port, uint32_t value);
    void *(*dma_alloc)(void *context, size_t size, size_t alignment,
                       uint64_t *physical);
    void (*dma_free)(void *context, void *virtual_address,
                     uint64_t physical, size_t size);
    void (*cache_invalidate)(void *context, void *address, size_t size);
    void (*cache_clean)(void *context, const void *address, size_t size);
    void (*delay_us)(void *context, uint32_t microseconds);
    void (*receive)(void *context, const uint8_t *frame, size_t length);
    void (*transmit_complete)(void *context, unsigned int slot, int status);
};

struct kira_rtl8139 {
    struct kira_rtl8139_platform platform;
    uint16_t io_base;
    uint8_t mac[6];
    void *rx_buffer;
    uint64_t rx_physical;
    void *tx_buffer[KIRA_RTL8139_TX_SLOTS];
    uint64_t tx_physical[KIRA_RTL8139_TX_SLOTS];
    uint16_t rx_offset;
    uint8_t tx_next;                     /* следующий дескриптор передачи: чип обслуживает их строго по кругу */
    uint8_t tx_busy[KIRA_RTL8139_TX_SLOTS];
    uint8_t initialized;
};

int kira_rtl8139_init(struct kira_rtl8139 *device, uint16_t io_base,
                      const struct kira_rtl8139_platform *platform);
int kira_rtl8139_poll(struct kira_rtl8139 *device);
int kira_rtl8139_send(struct kira_rtl8139 *device,
                      const void *frame, size_t length);
int kira_rtl8139_link_up(struct kira_rtl8139 *device);   /* 1 - канал есть */
void kira_rtl8139_shutdown(struct kira_rtl8139 *device);

#endif
