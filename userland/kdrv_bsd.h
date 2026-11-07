/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef KIRA_KDRV_BSD_H
#define KIRA_KDRV_BSD_H
/* Слой совместимости для портов BSD-драйверов (FreeBSD, OpenBSD, NetBSD) в драйвер Ring 3 Киры. Не вся ядерная часть BSD, а ровно то,
   что нужно портируемым сетевым драйверам: доступ к регистрам как bus_space, DELAY, память, мьютекс, DMA-буфер, "ifnet-лайт" поверх
   колец netdev_register (kdrv_format.h) и вывод. Собирается и kcc на устройстве, и clang на хосте; порядок байт - как у процессора
   (x86-64 и ARM64 little-endian, поэтому htole/le2h не меняют ничего). Подключать после <kira.h>, <stdint.h> не нужен.
   Честные границы: нет mbuf, нет MII/ifmedia, нет разделяемых прерываний INTx (ядро даёт драйверу только MSI/MSI-X), поэтому драйвер
   устройства без MSI опрашивает его по таймеру (kd_wait). */
#include "kira.h"
#include "kdrv.h"

#define htole16(x) ((u16)(x))
#define htole32(x) ((u32)(x))
#define le16toh(x) ((u16)(x))
#define le32toh(x) ((u32)(x))
#define ETHER_ADDR_LEN 6
#define ETHER_CRC_LEN 4
#define ETHER_MIN_LEN 64
#define ETHER_MAX_LEN 1518
#define ETHER_HDR_LEN 14

/* ---- вывод ---- */
static inline void kd_puts(const char* s) { k_puts(s); }
static inline void kd_putu(u64 v) { char b[24]; int n = 0; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[24]; int k = 0; while (n) o[k++] = b[--n]; k_write(o, (u64)k); }
static inline void kd_puthex(u64 v, int digits) { char o[17]; for (int i = 0; i < digits; i++) o[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15]; k_write(o, (u64)digits); }
static inline void kd_putmac(const u8* m) { for (int i = 0; i < 6; i++) { kd_puthex(m[i], 2); if (i < 5) kd_puts(":"); } }

/* ---- регистры: bus_space ---- */
typedef volatile u8* bus_space_handle_t;
#define bus_space_read_1(h, o)  (*(volatile u8*)((h) + (o)))
#define bus_space_read_2(h, o)  (*(volatile u16*)((h) + (o)))
#define bus_space_read_4(h, o)  (*(volatile u32*)((h) + (o)))
#define bus_space_write_1(h, o, v) (*(volatile u8*)((h) + (o)) = (u8)(v))
#define bus_space_write_2(h, o, v) (*(volatile u16*)((h) + (o)) = (u16)(v))
#define bus_space_write_4(h, o, v) (*(volatile u32*)((h) + (o)) = (u32)(v))
static inline void bus_space_barrier(void) { __sync_synchronize(); }
/* BAR устройства драйвера -> дескриптор доступа (0 - отказ ядра; код в *err) */
static inline bus_space_handle_t bus_space_map(u32 bar, u64 len, i64* err) { return (bus_space_handle_t)kdrv_mmio_map(bar, 0, len, err); }

/* ---- время ---- */
static inline void DELAY(u64 us) { u64 t = k_clock_ns(); while (k_clock_ns() - t < us * 1000) { } }
static inline u64 kd_now_ms(void) { return k_clock_ns() / 1000000; }

/* ---- память: куча программы, куски без возврата (драйвер выделяет всё при подключении) ---- */
static inline void* kd_malloc(u64 n) {
    static u8* cur; static u64 left;
    n = (n + 15) & ~15ull;
    if (n > left) { u64 pages = (n + 4095) / 4096; if (pages < 4) pages = 4; u8* p = (u8*)k_heap_grow(pages); if (!p) return (void*)0; cur = p; left = pages * 4096; }
    void* r = cur; cur += n; left -= n; return r;
}
static inline void kd_memcpy(void* d, const void* s, u64 n) { u8* a = (u8*)d; const u8* b = (const u8*)s; while (n--) *a++ = *b++; }
static inline void kd_memset(void* d, int c, u64 n) { u8* a = (u8*)d; while (n--) *a++ = (u8)c; }

/* ---- мьютекс (futex) ---- */
struct mtx { volatile u32 m; };
#define mtx_init(x) ((x)->m = 0)
#define mtx_lock(x) k_mutex_lock(&(x)->m)
#define mtx_unlock(x) k_mutex_unlock(&(x)->m)

/* ---- DMA: непрерывный буфер (bus_dma упрощённо) ---- */
struct kd_dmabuf { u8* va; u64 pa; u64 size; };
static inline i64 kd_dma_alloc(struct kd_dmabuf* b, u64 size, u32 addr_bits) {
    kdrv_dma_t d; i64 rc = kdrv_dma_alloc(size, addr_bits, &d);
    if (rc) return rc;
    b->va = (u8*)d.va; b->pa = d.phys; b->size = d.bytes; return 0;
}

/* ---- ifnet-лайт: сетевая карта драйвера для сетевого стека ядра (кольца создаёт ядро) ---- */
struct ifnet {
    void* if_softc;
    u8 if_lladdr[ETHER_ADDR_LEN];
    kdrv_netctl_t* ctl;
    u32 if_opackets, if_ipackets, if_oerrors, if_ierrors, if_iqdrops;
};
/* регистрация карты: 0 - готово. Ядро отдаст кольца; после этого сетевой стек берёт карту как встроенную. */
static inline int if_attach(struct ifnet* ifp) {
    ifp->ctl = kdrv_net_register(ifp->if_lladdr);
    return ifp->ctl ? 0 : -1;
}
static inline void if_link_state_change(struct ifnet* ifp, int up) { if (ifp->ctl) { __atomic_store_n(&ifp->ctl->link, up ? 1u : 0u, __ATOMIC_RELEASE); } }
/* принятый кадр -> ядру (без CRC); 0 - положен, -1 - кольцо полно (кадр потерян, учтён) */
static inline int if_input(struct ifnet* ifp, const u8* frame, u32 len) {
    kdrv_netctl_t* c = ifp->ctl;
    u32 head = c->rx_head, tail = __atomic_load_n(&c->rx_tail, __ATOMIC_ACQUIRE);
    if (len < ETHER_HDR_LEN || len > c->max_frame || head - tail >= c->slots) { ifp->if_iqdrops++; return -1; }
    kdrv_slot_t* s = (kdrv_slot_t*)((u8*)c + KDRV_NET_RX_OFF) + (head % c->slots);
    kd_memcpy(s->data, frame, len);
    s->len = len;
    __atomic_store_n(&c->rx_head, head + 1, __ATOMIC_RELEASE);
    ifp->if_ipackets++;
    return 0;
}
/* кадр от ядра на передачу -> buf; длина или 0 (пусто). Кадр снимается с кольца. */
static inline u32 if_dequeue(struct ifnet* ifp, u8* buf, u32 max) {
    kdrv_netctl_t* c = ifp->ctl;
    u32 head = __atomic_load_n(&c->tx_head, __ATOMIC_ACQUIRE), tail = c->tx_tail;
    if (head == tail) return 0;
    kdrv_slot_t* s = (kdrv_slot_t*)((u8*)c + KDRV_NET_TX_OFF) + (tail % c->slots);
    u32 len = s->len;
    if (len < ETHER_HDR_LEN || len > c->max_frame || len > max) len = 0;
    else kd_memcpy(buf, s->data, len);
    __atomic_store_n(&c->tx_tail, tail + 1, __ATOMIC_RELEASE);
    return len;
}
static inline int if_tx_pending(struct ifnet* ifp) { return __atomic_load_n(&ifp->ctl->tx_head, __ATOMIC_ACQUIRE) != ifp->ctl->tx_tail; }
/* ждать: звонок передачи от ядра (KDRV_EV_TX), прерывание или срок (в 10 мс); результат - события, KE_TIMEOUT или KE_INTR (драйвер завершают) */
static inline i64 kd_wait(u64 ticks10ms) {
    i64 r = kdrv_irq_wait(ticks10ms);
    if (r == KE_PERM) {
        i64 sr = ksys(K_SLEEP, ticks10ms ? ticks10ms : 1u, 0, 0);
        return sr < 0 ? sr : KE_TIMEOUT;
    }
    return r;
}
#endif
