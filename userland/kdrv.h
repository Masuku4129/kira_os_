#ifndef KIRA_USER_KDRV_H
#define KIRA_USER_KDRV_H
/* Библиотека драйвера Ring 3: обёртки над системными вызовами 48..54 (kdrv_format.h). Подключать после kira.h.
   Каждый вызов действует только на устройство, за которым ядро закрепило этот процесс; права - из пакета и .rec. */
#include "kira.h"
#include "kdrv_format.h"

static inline i64 kdrv_info(kdrv_info_t* i) { return ksys(KDRV_SYS_INFO, (u64)i, sizeof *i, 0); }
static inline i64 kdrv_cfg_read(u32 off, u32 width) { return ksys(KDRV_SYS_CFG_READ, off, width, 0); }
static inline i64 kdrv_cfg_write(u32 off, u32 width, u32 v) { return ksys(KDRV_SYS_CFG_WRITE, off, width, v); }
/* BAR (или его часть, смещение кратно 4 КБ) в адресное пространство драйвера: без кэша, без исполнения. 0 - отказ (код в *err). */
static inline volatile void* kdrv_mmio_map(u32 bar, u64 off, u64 len, i64* err) {
    i64 r = ksys(KDRV_SYS_MMIO_MAP, bar, off, len);
    if (err) *err = r;
    return r < 0 ? (volatile void*)0 : (volatile void*)(u64)r;
}
/* Непрерывная память для DMA, обнулена. addr_bits - сколько битов адреса достаёт устройство (0 - без предела). */
static inline i64 kdrv_dma_alloc(u64 bytes, u32 addr_bits, kdrv_dma_t* out) { return ksys(KDRV_SYS_DMA_ALLOC, bytes, addr_bits, (u64)out); }
/* Ждать прерывание устройства (или звонок передачи у сетевой карты): маска KDRV_EV_*, KE_TIMEOUT по сроку (в 10 мс; K_FOREVER - без срока). */
static inline i64 kdrv_irq_wait(u64 timeout10ms) { return ksys(KDRV_SYS_IRQ_WAIT, timeout10ms, 0, 0); }
/* Сетевая карта: ядро создаёт кольца и отображает их в драйвере; результат - область kdrv_netctl_t или 0. */
static inline kdrv_netctl_t* kdrv_net_register(const u8 mac[6]) {
    i64 r = ksys(KDRV_SYS_NETDEV, KDRV_NET_REGISTER, (u64)mac, 0);
    return r < 0 ? (kdrv_netctl_t*)0 : (kdrv_netctl_t*)(u64)r;
}
static inline i64 kdrv_net_kick(void) { return ksys(KDRV_SYS_NETDEV, KDRV_NET_KICK, 0, 0); }

#define KDRV_R32(p, off) (*(volatile u32*)((volatile u8*)(p) + (off)))
#endif
