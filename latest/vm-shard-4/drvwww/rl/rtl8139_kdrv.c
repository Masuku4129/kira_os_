/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) Kira OS authors. Original implementation from the public Realtek RTL8139 programming specification. */
/* Драйвер RTL8139 (10ec:8139) для Киры, Ring 3. Оригинальный код: логика чипа - в rtl8139.c (по публичной спецификации
   Realtek), здесь только привязка к системным вызовам kdrv: регистры через BAR в памяти, DMA, кольца сетевой карты. */
#include "kdrv_bsd.h"
#include "rtl8139.h"
#include "rtl8139.c"

static struct {
    volatile u8 *regs;
    struct ifnet ifp;
    u32 rx_drop;
} g;

static u8 p_in8(void *c, uint16_t r) { (void)c; return *(volatile u8 *)(g.regs + r); }
static uint16_t p_in16(void *c, uint16_t r) { (void)c; return *(volatile uint16_t *)(g.regs + r); }
static uint32_t p_in32(void *c, uint16_t r) { (void)c; return *(volatile uint32_t *)(g.regs + r); }
static void p_out8(void *c, uint16_t r, u8 v) { (void)c; *(volatile u8 *)(g.regs + r) = v; }
static void p_out16(void *c, uint16_t r, uint16_t v) { (void)c; *(volatile uint16_t *)(g.regs + r) = v; }
static void p_out32(void *c, uint16_t r, uint32_t v) { (void)c; *(volatile uint32_t *)(g.regs + r) = v; }
static void *p_dma_alloc(void *c, size_t size, size_t align, uint64_t *phys) {
    struct kd_dmabuf b; (void)c; (void)align;               /* блоки DMA выровнены по странице */
    if (kd_dma_alloc(&b, size, 32)) return 0;                /* RBSTART и TSAD - 32 бита */
    *phys = b.pa;
    return b.va;
}
static void p_dma_free(void *c, void *v, uint64_t p, size_t s) { (void)c; (void)v; (void)p; (void)s; }   /* память вернёт ядро при гибели драйвера */
static void p_delay(void *c, uint32_t us) { (void)c; (void)us; k_yield(); }
static void p_receive(void *c, const u8 *f, size_t n) { (void)c; if (if_input(&g.ifp, f, (u32)n) < 0) g.rx_drop++; }

int main(void) {
    kdrv_info_t in;
    i64 err = 0;
    u32 bar;
    struct kira_rtl8139 dev;
    struct kira_rtl8139_platform pl;
    u8 pend[KIRA_RTL8139_MAX_FRAME];
    u32 pend_len = 0;
    int link = -1, ticks = 0;

    if (kdrv_info(&in) != 0) { kd_puts("rtl8139: no device bound\n"); return 1; }
    if (in.vendor != 0x10ec || in.device != 0x8139) { kd_puts("rtl8139: not an RTL8139\n"); return 1; }
    for (bar = 0; bar < 6; bar++)                         /* порты отобразить нельзя; регистры те же в BAR1 (память) */
        if (in.bar[bar].size && !(in.bar[bar].flags & KDRV_BAR_IO)) break;
    if (bar == 6) { kd_puts("rtl8139: no memory BAR\n"); return 1; }
    g.regs = (volatile u8 *)kdrv_mmio_map(bar, 0, in.bar[bar].size, &err);
    if (!g.regs) { kd_puts("rtl8139: cannot map BAR\n"); return 1; }
    {
        i64 cmd = kdrv_cfg_read(0x04, 2);                 /* INTx не используется: опрос по таймеру */
        if (cmd >= 0 && kdrv_cfg_write(0x04, 2, (u32)cmd | 0x400) != 0) kd_puts("rtl8139: cannot disable INTx\n");
    }
    for (u32 i = 0; i < sizeof pl; i++) ((u8 *)&pl)[i] = 0;
    pl.in8 = p_in8; pl.in16 = p_in16; pl.in32 = p_in32; pl.out8 = p_out8; pl.out16 = p_out16; pl.out32 = p_out32;
    pl.dma_alloc = p_dma_alloc; pl.dma_free = p_dma_free; pl.delay_us = p_delay; pl.receive = p_receive;
    if (kira_rtl8139_init(&dev, 0, &pl) != 0) { kd_puts("rtl8139: init failed\n"); return 1; }
    kd_memcpy(g.ifp.if_lladdr, dev.mac, ETHER_ADDR_LEN);
    if (if_attach(&g.ifp) != 0) { kd_puts("rtl8139: netdev_register refused\n"); kira_rtl8139_shutdown(&dev); return 1; }
    kd_puts("rtl8139: BAR"); kd_putu(bar); kd_puts(", MAC "); kd_putmac(dev.mac); kd_puts("\n");

    for (;;) {
        i64 rc = kd_wait(1);                                   /* до 10 мс; звонок передачи будит сразу */
        if (rc == KE_INTR) break;
        if (rc < 0 && rc != KE_TIMEOUT) k_sleep(1);
        for (;;) {                                             /* кадры от ядра на передачу; полный круг дескрипторов - ждём */
            if (!pend_len) pend_len = if_dequeue(&g.ifp, pend, sizeof pend);
            if (!pend_len) break;
            if (kira_rtl8139_send(&dev, pend, pend_len) == -2) break;
            pend_len = 0;
        }
        kira_rtl8139_poll(&dev);
        if (g.rx_drop) { g.rx_drop = 0; kira_rtl8139_poll(&dev); }
        if (++ticks >= 50 || link < 0) {                       /* канал - раз в полсекунды */
            int up = kira_rtl8139_link_up(&dev);
            ticks = 0;
            if (up != link) { link = up; if_link_state_change(&g.ifp, up); kd_puts(up ? "rtl8139: link up\n" : "rtl8139: link down\n"); }
        }
    }
    kira_rtl8139_shutdown(&dev);
    kd_puts("rtl8139: stopped\n");
    return 0;
}
