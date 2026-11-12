// Драйвер xHCI против модели контроллера: регистры - обычная память, модель после каждой «миллисекунды» смотрит,
// что записал драйвер (USBCMD, PORTSC, дверные звонки), исполняет кольца команд и передач и пишет события.
// Проверяются: передача от BIOS, сброс, DCBAA и scratchpad, кольца с переходом по ссылке, Enable Slot,
// Address Device, Evaluate Context (FS), Configure Endpoint, управляющие передачи (в т.ч. короткий пакет),
// прерывающие отчёты, отключение (Disable Slot); хабы: строка маршрута, переводчик транзакций (TT) для LS/FS за
// HS-хабом, признаки хаба в контексте слота; флешка: массовые точки, Normal TRB до 16 КБ без пересечения 64 КБ,
// STALL -> Reset Endpoint + переконфигурация точки, снятие точек и слотов при извлечении.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "usb.h"
#include "pci.h"
#include "input.h"
#include "vfs.h"
#include "usbdev_model.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static uint8_t* arena; static size_t arena_used;
void* usb_dma_alloc(uint32_t b, uint64_t* phys, int lo) {
    (void)lo;
    size_t n = ((size_t)b + 4095) & ~(size_t)4095;
    if (arena_used + n > (32u << 20)) return 0;
    void* p = arena + arena_used; arena_used += n; memset(p, 0x5A, n); memset(p, 0, n);
    *phys = (uint64_t)(uintptr_t)p; return p;
}
#define P(a) ((void*)(uintptr_t)(a))

/* ---- регистры ---- */
static uint8_t regs[0x4000] __attribute__((aligned(64)));
#define CAPLEN 0x20
#define OP (regs + CAPLEN)
#define RT (regs + 0x2000)
#define DB ((volatile uint32_t*)(regs + 0x3000))
#define XECP 0x1000
static uint32_t* R(uint8_t* b, unsigned off) { return (uint32_t*)(b + off); }
static uint64_t R64(uint8_t* b, unsigned off) { return (uint64_t)*R(b, off) | (uint64_t)*R(b, off + 4) << 32; }

static mdev_t devm[4];                    /* порты 1..4 */
static uint32_t port_state[5], port_last[5];
static int running;
static uint64_t cr_deq; static uint32_t cr_cycle;
static uint64_t ev_base; static uint32_t ev_size, ev_enq, ev_cycle;
static struct { int used, port; mdev_t* dev; uint64_t ep_deq[32]; uint32_t ep_cycle[32]; int maxp0, halted[32], type[32], maxp[32], hub, hub_ports; } slots[17];
static int bulk_trbs, max_bulk_len, tt_checked, drops;
static int cmds_seen[64], events_written, link_follows, resets, handoff_done;
static uint8_t addr_next = 1;

static void event(uint64_t ptr, uint32_t st, uint32_t ctl) {
    volatile uint32_t* e = (volatile uint32_t*)P(ev_base + (uint64_t)ev_enq * 16);
    e[0] = (uint32_t)ptr; e[1] = (uint32_t)(ptr >> 32); e[2] = st; e[3] = (ctl & ~1u) | ev_cycle;
    if (++ev_enq == ev_size) { ev_enq = 0; ev_cycle ^= 1; }
    events_written++;
}

/* читает очередной TRB кольца; переходы по ссылке - здесь же. 0 - TRB нет (бит цикла не совпал). */
static volatile uint32_t* next_trb(uint64_t* deq, uint32_t* cyc) {
    for (int g = 0; g < 4; g++) {
        volatile uint32_t* t = (volatile uint32_t*)P(*deq);
        if ((t[3] & 1) != *cyc) return 0;
        if (((t[3] >> 10) & 63) == 6) {                     /* ссылка */
            *deq = (uint64_t)t[0] | (uint64_t)t[1] << 32;
            if (t[3] & 2) *cyc ^= 1;
            link_follows++;
            continue;
        }
        return t;
    }
    return 0;
}

static unsigned csz = 32;
static volatile uint32_t* ictx(uint64_t base, unsigned i) { return (volatile uint32_t*)P(base + i * csz); }

static volatile uint32_t* out_ctx(uint32_t sl, unsigned i) {
    volatile uint64_t* dcbaa = (volatile uint64_t*)P(R64(OP, 0x30));
    return (volatile uint32_t*)P(dcbaa[sl] + i * csz);
}
static void ep_set_state(uint32_t sl, unsigned dci, uint32_t st) { volatile uint32_t* o = out_ctx(sl, dci); o[0] = (o[0] & ~7u) | st; }
static uint32_t ep_get_state(uint32_t sl, unsigned dci) { return out_ctx(sl, dci)[0] & 7; }
static uint32_t slot_of(mdev_t* d) { for (uint32_t s = 1; s <= 16; s++) if (slots[s].used && slots[s].dev == d) return s; return 0; }
static int mspeed(const mdev_t* d) { return d->low_speed ? 2 : d->high_speed ? 3 : 1; }

static void do_command(volatile uint32_t* t, uint64_t at) {
    uint32_t type = (t[3] >> 10) & 63, sl = t[3] >> 24, dci = (t[3] >> 16) & 31;
    uint64_t in = (uint64_t)t[0] | (uint64_t)t[1] << 32;
    if (type < 64) cmds_seen[type]++;
    uint32_t cc = 1, rslot = sl;
    volatile uint64_t* dcbaa = (volatile uint64_t*)P(R64(OP, 0x30));
    if (type == 9) {                                        /* Enable Slot */
        rslot = 0;
        for (int s = 1; s <= 16; s++) if (!slots[s].used) { memset(&slots[s], 0, sizeof slots[s]); slots[s].used = 1; rslot = (uint32_t)s; break; }
        if (!rslot) cc = 9;
    } else if (type == 11) {                                /* Address Device: маршрут -> устройство */
        CHECK(ictx(in, 0)[1] == 3);
        uint32_t s0 = ictx(in, 1)[0], port = (ictx(in, 1)[1] >> 16) & 0xFF, route = s0 & 0xFFFFF;
        CHECK(port >= 1 && port <= 4 && devm[port - 1].connected && (port_state[port] & 2));
        mdev_t* d = &devm[port - 1];
        mdev_t* hs_hub = 0; int hs_port = 0;
        for (int lvl = 0; lvl < 5 && ((route >> (4 * lvl)) & 15); lvl++) {
            int hp = (route >> (4 * lvl)) & 15;
            CHECK(d->kind == MK_HUB && d->child[hp]);
            if (d->high_speed) { hs_hub = d; hs_port = hp; }
            d = d->child[hp];
        }
        CHECK(mdev_behind_ok(d) && d->addr == 0);
        CHECK((int)((s0 >> 20) & 15) == mspeed(d));
        uint32_t s2 = ictx(in, 1)[2];
        if (hs_hub && (d->low_speed || !d->high_speed)) {   /* LS/FS за HS-хабом: TT - ближайший HS-хаб на пути */
            CHECK((s2 & 0xFF) == slot_of(hs_hub) && ((s2 >> 8) & 0xFF) == (uint32_t)hs_port);
            tt_checked++;
        } else CHECK((s2 & 0xFFFF) == 0);
        slots[sl].port = (int)port; slots[sl].dev = d;
        volatile uint32_t* ep0 = ictx(in, 2);
        CHECK(((ep0[1] >> 3) & 7) == 4);
        slots[sl].maxp0 = (int)(ep0[1] >> 16);
        slots[sl].ep_deq[1] = ((uint64_t)ep0[2] | (uint64_t)ep0[3] << 32) & ~15ull; slots[sl].ep_cycle[1] = ep0[2] & 1;
        d->addr = addr_next++;
        volatile uint32_t* out = (volatile uint32_t*)P(dcbaa[sl]);
        CHECK(out != 0);
        out[0] = s0; out[1] = ictx(in, 1)[1]; out[2] = s2; out[3] = (2u << 27) | (uint32_t)d->addr;
        ep_set_state(sl, 1, 1);
    } else if (type == 13) {                                /* Evaluate Context */
        CHECK(ictx(in, 0)[1] == 2);
        slots[sl].maxp0 = (int)(ictx(in, 2)[1] >> 16);
    } else if (type == 12) {                                /* Configure Endpoint */
        uint32_t drop = ictx(in, 0)[0], add = ictx(in, 0)[1];
        CHECK(add & 1);
        uint32_t s0 = ictx(in, 1)[0];
        if (s0 & (1u << 26)) {                              /* хаб: число портов и TTT */
            CHECK(slots[sl].dev->kind == MK_HUB);
            slots[sl].hub = 1; slots[sl].hub_ports = (int)(ictx(in, 1)[1] >> 24);
        }
        for (unsigned i = 2; i < 32; i++) if (drop & (1u << i)) {
            CHECK(slots[sl].type[i]);                       /* снимается то, что было создано */
            slots[sl].ep_deq[i] = 0; slots[sl].type[i] = 0; slots[sl].halted[i] = 0; ep_set_state(sl, i, 0); drops++;
        }
        for (unsigned i = 2; i < 32; i++) if (add & (1u << i)) {
            volatile uint32_t* e = ictx(in, 1 + i);
            int et = (e[1] >> 3) & 7;
            CHECK((et == 7 && (i & 1)) || (et == 6 && (i & 1)) || (et == 2 && !(i & 1)));
            CHECK((e[1] >> 16) > 0 && ((s0 >> 27) & 31) >= i);
            CHECK(!slots[sl].type[i] || (drop & (1u << i)));
            slots[sl].type[i] = et; slots[sl].maxp[i] = (int)(e[1] >> 16);
            slots[sl].ep_deq[i] = ((uint64_t)e[2] | (uint64_t)e[3] << 32) & ~15ull; slots[sl].ep_cycle[i] = e[2] & 1;
            ep_set_state(sl, i, 1);
        }
        volatile uint32_t* o = out_ctx(sl, 0);
        o[0] = s0; o[1] = ictx(in, 1)[1]; o[2] = ictx(in, 1)[2];
    } else if (type == 10) { slots[sl].used = 0; slots[sl].dev = 0; }   /* Disable Slot */
    else if (type == 14) { if (ep_get_state(sl, dci) != 2) cc = 19; else { slots[sl].halted[dci] = 0; ep_set_state(sl, dci, 3); } }   /* Reset Endpoint */
    else if (type == 15) { if (ep_get_state(sl, dci) != 1) cc = 19; else ep_set_state(sl, dci, 3); }   /* Stop Endpoint */
    else if (type == 16) {                                  /* Set TR Dequeue */
        if (ep_get_state(sl, dci) != 3) cc = 19;
        else { slots[sl].ep_deq[dci] = in & ~15ull; slots[sl].ep_cycle[dci] = (uint32_t)(in & 1); }
    }
    else cc = 5;
    event(at, cc << 24, (33u << 10) | (rslot << 24));
}

static int reachable(mdev_t* d) { mdev_t* r = d; while (r->parent) r = r->parent; return mdev_behind_ok(d) && r >= devm && r < devm + 4; }

static void do_transfers(uint32_t sl, uint32_t dci) {
    if (!slots[sl].used || !slots[sl].dev) return;
    mdev_t* d = slots[sl].dev;
    if (slots[sl].halted[dci] || ep_get_state(sl, dci) == 0) return;   /* остановленная/снятая точка не исполняет TRB */
    if (ep_get_state(sl, dci) == 3) { if (dci == 1) ep_set_state(sl, 1, 1); else return; }
    for (int g = 0; g < 16; g++) {
        uint64_t save = slots[sl].ep_deq[dci]; uint32_t savec = slots[sl].ep_cycle[dci];
        volatile uint32_t* t = next_trb(&slots[sl].ep_deq[dci], &slots[sl].ep_cycle[dci]);
        if (!t) return;
        uint64_t at = slots[sl].ep_deq[dci];
        uint32_t type = (t[3] >> 10) & 63, len = t[2] & 0x1FFFF;
        uint64_t buf = (uint64_t)t[0] | (uint64_t)t[1] << 32;
        uint32_t cc = 1, residual = 0; int ev = (t[3] >> 5) & 1;
        if (!reachable(d)) {                                /* устройство не отвечает: ошибка транзакции */
            event(at, 4u << 24, (32u << 10) | (sl << 24) | (dci << 16));
            slots[sl].ep_deq[dci] += 16; slots[sl].halted[dci] = 1; ep_set_state(sl, dci, 2);
            return;
        }
        if (dci == 1) {
            d->ep0_maxp = slots[sl].maxp0;
            if (type == 2) { CHECK(t[3] & (1u << 6)); uint8_t s8[8]; memcpy(s8, (const void*)t, 8); mdev_setup(d, s8); }
            else if (type == 3) {
                int in = (t[3] >> 16) & 1;
                if (in) {
                    uint32_t got = 0;
                    while (got < len) { int r = mdev_in(d, 0, (uint8_t*)P(buf) + got, len - got, 0); if (r < 0) { cc = 6; break; } got += (uint32_t)r; if (r < d->ep0_maxp) break; }
                    residual = len - got;
                    if (cc == 1 && residual) { cc = 13; if (t[3] & 4) ev = 1; }
                } else if (mdev_out(d, 0, (const uint8_t*)P(buf), len, -1) < 0) cc = 6;
            } else if (type == 4) {
                int in = (t[3] >> 16) & 1;
                int r = in ? mdev_in(d, 0, 0, 0, 0) : mdev_out(d, 0, 0, 0, -1);
                if (r < 0) cc = 6;
            } else cc = 5;
        } else {
            CHECK(type == 1);
            int epn = (int)(dci / 2), in = (int)(dci & 1);
            unsigned mp = (unsigned)slots[sl].maxp[dci];
            uint32_t got = 0;
            if (slots[sl].type[dci] != 7) {
                bulk_trbs++; if ((int)len > max_bulk_len) max_bulk_len = (int)len;
                CHECK((buf & 0xFFFF) + len <= 0x10000);     /* TRB не пересекает границу 64 КБ */
            }
            if (in) {
                while (got < len) {
                    unsigned c = len - got < mp ? len - got : mp;
                    int r = mdev_in(d, epn, (uint8_t*)P(buf) + got, c, -1);
                    if (r == M_NAK) { CHECK(got == 0); slots[sl].ep_deq[dci] = save; slots[sl].ep_cycle[dci] = savec; return; }   /* ждём данных */
                    if (r == M_STALL) { cc = 6; break; }
                    got += (uint32_t)r;
                    if ((unsigned)r < c) break;
                }
            } else {
                while (got < len) {
                    unsigned c = len - got < mp ? len - got : mp;
                    if (mdev_out(d, epn, (const uint8_t*)P(buf) + got, c, -1) < 0) { cc = 6; break; }
                    got += c;
                }
            }
            residual = len - got;
            if (cc == 1 && residual) { cc = 13; if (t[3] & 4) ev = 1; }
        }
        if (ev || cc == 6) event(at, (cc << 24) | residual, (32u << 10) | (sl << 24) | (dci << 16));
        slots[sl].ep_deq[dci] += 16;
        if (cc == 6) { slots[sl].halted[dci] = 1; ep_set_state(sl, dci, 2); return; }
    }
}

static void step(void) {
    /* передача от BIOS */
    uint32_t leg = *R(regs, XECP);
    if ((leg & (1u << 24)) && (leg & (1u << 16))) { *R(regs, XECP) = leg & ~(1u << 16); handoff_done = 1; }
    uint32_t c = *R(OP, 0);
    if (c & 2) {                                            /* HCRST */
        resets++; *R(OP, 0) = 0; *R(OP, 4) = 1; running = 0;
        for (int p = 1; p <= 4; p++) { port_state[p] = (1u << 9) | (devm[p - 1].connected ? 1u | (1u << 17) : 0); port_state[p] |= devm[p - 1].connected ? ((uint32_t)mspeed(&devm[p - 1]) << 10) : 0; port_last[p] = ~0u; }
        memset(slots, 0, sizeof slots);
        return;
    }
    if ((c & 1) && !running) {
        running = 1; *R(OP, 4) &= ~1u;
        uint64_t crcr = R64(OP, 0x18); cr_deq = crcr & ~63ull; cr_cycle = crcr & 1;
        CHECK(*R(RT, 0x28) == 1);
        volatile uint64_t* erst = (volatile uint64_t*)P(R64(RT, 0x30));
        ev_base = erst[0]; ev_size = (uint32_t)erst[1]; ev_enq = 0; ev_cycle = 1;
        CHECK(*R(OP, 0x38) >= 1 && R64(OP, 0x30) != 0);
        volatile uint64_t* dcbaa = (volatile uint64_t*)P(R64(OP, 0x30));
        CHECK(dcbaa[0] != 0 && ((volatile uint64_t*)P(dcbaa[0]))[1] != 0);       /* scratchpad: 2 страницы */
    } else if (!(c & 1) && running) { running = 0; *R(OP, 4) |= 1; }
    /* порты: запись драйвера отличается от того, что мы показали */
    for (int p = 1; p <= 4; p++) {
        uint32_t* reg = R(OP, 0x400 + 0x10 * (unsigned)(p - 1));
        if (port_last[p] != ~0u && *reg != port_last[p]) {
            uint32_t w = *reg;
            port_state[p] &= ~(w & (0x7Fu << 17));          /* W1C изменений */
            if (w & (1u << 4)) { port_state[p] |= 2u | (1u << 21); devm[p - 1].addr = 0; }   /* сброс: PED, PRC */
            if (w & 2u) port_state[p] &= ~2u;               /* запись 1 в PED - выключить порт */
        }
        uint32_t conn = devm[p - 1].connected;
        if (!conn && (port_state[p] & 1)) port_state[p] = (port_state[p] & ~3u & ~(15u << 10)) | (1u << 17);
        if (conn && !(port_state[p] & 1)) port_state[p] = (port_state[p] & ~(15u << 10)) | 1u | (1u << 17) | ((uint32_t)mspeed(&devm[p - 1]) << 10);
        *reg = port_state[p]; port_last[p] = port_state[p];
    }
    if (!running) return;
    {                                                       /* кольцо команд (звонок 0 пишет значение 0 - его не отличить: смотрим всегда) */
        for (int g = 0; g < 32; g++) {
            volatile uint32_t* t = next_trb(&cr_deq, &cr_cycle);
            if (!t) break;
            do_command(t, cr_deq);
            cr_deq += 16;
        }
    }
    for (uint32_t s = 1; s <= 16; s++) {
        uint32_t target = DB[s];
        if (target) { DB[s] = 0; do_transfers(s, target); }
        for (uint32_t dci = 2; dci < 32; dci++) if (slots[s].used && slots[s].ep_deq[dci]) do_transfers(s, dci);   /* ожидающие отчётов */
    }
}

static uint32_t now;
void usb_sleep_ms(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { now++; step(); } }
uint32_t usb_now_ms(void) { return now; }
static char logbuf[16384];
void usb_log(const char* s) { strncat(logbuf, s, sizeof logbuf - strlen(logbuf) - 1); }
void usb_log_dec(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); usb_log(b); }
void usb_log_hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); usb_log(b); }
static uint16_t pci_command_model, pci_msi_control_model, pci_msix_control_model;
static int pci_block_busmaster_clear, pci_block_msi_disable, pci_block_msix_mask, pci_block_intx_disable;
static uint8_t pci_cap_ptr_model = 0x50;
void pci_enable(const pci_dev_t* d, uint16_t b) { (void)d; if (pci_block_intx_disable) b &= (uint16_t)~PCI_CMD_INTX_OFF; pci_command_model |= b; }
uint8_t pci_read8(const pci_dev_t* d, uint16_t off) {
    (void)d;
    if (off == 0x34) return pci_cap_ptr_model;
    if (off == 0x50) return 0x05;
    if (off == 0x51) return 0x60;
    if (off == 0x60) return 0x11;
    if (off == 0x61) return 0;
    return 0;
}
uint16_t pci_read16(const pci_dev_t* d, uint16_t off) {
    (void)d;
    if (off == 0x06) return 0x10;
    if (off == 0x04) return pci_command_model;
    if (off == 0x52) return pci_msi_control_model;
    if (off == 0x62) return pci_msix_control_model;
    return 0;
}
uint16_t pci_next_cap(const pci_dev_t* d, uint16_t after, uint8_t* id) {
    (void)d;
    if (after == 0) { if (id) *id = 0x05; return 0x50; }
    if (after == 0x50) { if (id) *id = 0x11; return 0x60; }
    if (id) *id = 0;
    return 0;
}
void pci_write16(const pci_dev_t* d, uint16_t off, uint16_t v) {
    (void)d;
    if (off == 0x04) {
        if (pci_block_busmaster_clear) v |= PCI_CMD_MASTER;
        if (pci_block_intx_disable) v &= (uint16_t)~PCI_CMD_INTX_OFF;
        pci_command_model = v;
    } else if (off == 0x52) {
        if (!pci_block_msi_disable) pci_msi_control_model = v;
    } else if (off == 0x62) {
        if (!pci_block_msix_mask) pci_msix_control_model = v;
    }
}
uint32_t pci_read32(const pci_dev_t* d, uint16_t off) { (void)d; (void)off; return 0; }
void pci_write32(const pci_dev_t* d, uint16_t off, uint32_t v) { (void)d; (void)off; (void)v; }
volatile void* pci_map_bar(const pci_dev_t* d, int bar, uint64_t off, uint64_t len) { (void)d; (void)bar; (void)len; return regs + off; }

int main(void) {
    arena = mmap((void*)0x20000000, 32u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(arena != MAP_FAILED);
    input_init();
    regs[0] = CAPLEN;
    *R(regs, 4) = 16u | (1u << 8) | (4u << 24);             /* 16 слотов, 1 прерыватель, 4 порта */
    *R(regs, 8) = 2u << 27;                                 /* 2 страницы scratchpad */
    *R(regs, 0x10) = 1u | (1u << 3) | ((XECP / 4) << 16);   /* AC64, PPC, xECP */
    *R(regs, 0x14) = 0x3000; *R(regs, 0x18) = 0x2000;
    *R(regs, XECP) = 1u | (1u << 16);                       /* Legacy Support, BIOS владеет */
    *R(OP, 4) = 1;                                          /* остановлен */
    mdev_kbd(&devm[0]); mdev_tablet(&devm[1]);              /* порт 1 - LS клавиатура, порт 2 - FS планшет (EP0 64) */
    for (int p = 1; p <= 4; p++) port_last[p] = ~0u;
    pci_dev_t pd = { 0 }; pd.vendor = 0x1B36; pd.device = 0x000D; pd.bar[0].base = 0xFEB00000; pd.bar[0].size = 0x4000;
    usb_hc_t hc;
    pci_command_model = PCI_CMD_MASTER;                       /* inherited firmware DMA: halt before clearing */
    pci_msi_control_model = 1u;                                /* inherited MSI enabled */
    pci_msix_control_model = 0x8000u;                           /* inherited MSI-X enabled */
    pci_block_busmaster_clear = 1;                              /* emulate a stuck PCI Command.BusMaster bit */
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) != 0);           /* refusal must not pretend DMA was stopped */
    pci_block_busmaster_clear = 0;

    pci_command_model = 0; pci_msi_control_model = 1u; pci_msix_control_model = 0x8000u;
    pci_block_msi_disable = 1;
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    pci_block_msi_disable = 0;

    pci_command_model = 0; pci_msi_control_model = 1u; pci_msix_control_model = 0x8000u;
    pci_block_msix_mask = 1;
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    pci_block_msix_mask = 0;

    pci_command_model = 0; pci_msi_control_model = 1u; pci_msix_control_model = 0x8000u;
    pci_block_intx_disable = 1;
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    pci_block_intx_disable = 0;

    pci_command_model = PCI_CMD_MASTER;
    pci_cap_ptr_model = 0x20;                                  /* malformed capability-list pointer */
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    pci_cap_ptr_model = 0x50;

    pci_command_model = PCI_CMD_MASTER;
    *R(regs, 0x18) = 0x2001u;                               /* misaligned runtime register offset */
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    *R(regs, 0x18) = 0x2000u;
    pci_command_model = PCI_CMD_MASTER;
    *R(regs, 0x14) = 0x2000u;                               /* doorbell array overlaps runtime registers */
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    *R(regs, 0x14) = 0x3000u;

    pci_command_model = PCI_CMD_MASTER;
    pci_msi_control_model = 1u;
    pci_msix_control_model = 0x8000u;
    *R(regs, 0x10) = 1u | (1u << 3) | ((0x5000u / 4u) << 16); /* xECP outside mapped BAR */
    CHECK(xhci_start(&hc, &pd) == -2);
    CHECK((pci_command_model & PCI_CMD_MASTER) == 0);
    *R(regs, 0x10) = 1u | (1u << 3) | ((XECP / 4) << 16);

    pci_command_model = PCI_CMD_MASTER;
    pci_msi_control_model = 1u;
    pci_msix_control_model = 0x8000u;
    int rc = xhci_start(&hc, &pd);
    CHECK(rc == 0);
    CHECK((pci_msi_control_model & 1u) == 0);
    CHECK((pci_msix_control_model & 0xC000u) == 0x4000u);
    CHECK((pci_command_model & (PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF)) ==
          (PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF));
    if (rc) printf("%s", logbuf);
    CHECK(rc == 0 && handoff_done && resets == 1 && running);
    hc.ports(&hc);
    if (usb_hid_count() != 2) printf("%s", logbuf);
    CHECK(cmds_seen[9] == 2 && cmds_seen[11] == 2 && cmds_seen[12] == 2);
    CHECK(cmds_seen[13] == 1);                              /* FS планшет: EP0 8 -> 64 */
    CHECK(devm[0].configured == 1 && devm[0].protocol == 0 && devm[1].configured == 1);
    CHECK(usb_hid_count() == 2 && strstr(logbuf, "keyboard ready") && strstr(logbuf, "absolute"));

    uint8_t r_o[8] = { 0, 0, 0x12 }, r_k[8] = { 0, 0, 0x0E }, r0[8] = { 0 };
    for (int rep = 0; rep < 12; rep++) { mdev_report(&devm[0], r_o, 8); mdev_report(&devm[0], r0, 8); }   /* через ссылку кольца */
    mdev_report(&devm[0], r_k, 8); mdev_report(&devm[0], r0, 8);
    uint8_t t1[6] = { 3, 0xFF, 0x7F, 0x00, 0x00, 0 };
    mdev_report(&devm[1], t1, 6);
    for (int i = 0; i < 100; i++) { usb_sleep_ms(2); usb_hid_poll(now); }
    char typed[32] = { 0 }; int nt = 0; input_event_t ev;
    while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch && nt < 31) typed[nt++] = (char)ev.ch;
    if (strcmp(typed, "ooooooooooook")) printf("typed=[%s] rpos=%d nrep=%d\n", typed, devm[0].rpos, devm[0].nrep);
    CHECK(!strcmp(typed, "ooooooooooook"));
    mouse_event_t me;
    CHECK(input_pop_mouse(&me) && me.abs && me.buttons == 3 && me.ax == 32767 && me.ay == 0);

    /* короткий пакет на управляющей передаче: просим 64 байта дескриптора устройства, а их 18 */
    usb_dev_t* d = usb_dev_on_port(&hc, 2);
    CHECK(d);
    uint8_t buf[64]; usb_setup_t s = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 64 };
    CHECK(hc.control(&hc, d, &s, buf) == 18 && buf[0] == 18 && buf[1] == 1);
    /* STALL: неизвестный дескриптор */
    usb_setup_t bad = { 0x80, USB_REQ_GET_DESCRIPTOR, 0x0700, 0, 8 };
    CHECK(hc.control(&hc, d, &bad, buf) < 0);
    CHECK(cmds_seen[14] == 1 && cmds_seen[16] == 1);
    CHECK(hc.control(&hc, d, &s, buf) == 18);              /* после STALL EP0 снова работает */

    /* кольцо прерывающей точки проходит ссылку несколько раз: 4 x 30 отчётов */
    int total = 0;
    for (int b = 0; b < 4; b++) {
        devm[0].nrep = devm[0].rpos = 0;
        for (int rep = 0; rep < 15; rep++) { mdev_report(&devm[0], r_o, 8); mdev_report(&devm[0], r0, 8); }
        for (int i = 0; i < 100; i++) { usb_sleep_ms(2); usb_hid_poll(now); }
        CHECK(devm[0].rpos == 30);
        while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch == 'o') total++;
    }
    CHECK(total == 60 && link_follows >= 2);
    /* отключили клавиатуру: слот освобождён командой Disable Slot */
    devm[0].connected = 0;
    usb_sleep_ms(1);
    hc.ports(&hc);
    CHECK(usb_hid_count() == 1 && cmds_seen[10] == 1 && strstr(logbuf, "device removed"));
    CHECK(link_follows > 0);

    /* HS-хаб на порту 3: 1 - флешка USB 2.0, 2 - LS-клавиатура (через TT хаба), 3 - FS-хаб с LS-клавиатурой (TT унаследован) */
    static mdev_t stick, kbd2, fshub, kbd3;
    uint8_t* img = mstick_image();
    mdev_hub(&devm[2], 1); mdev_stick(&stick, img, MSTICK_SECTORS, 1); mdev_kbd(&kbd2); mdev_hub(&fshub, 0); mdev_kbd(&kbd3);
    stick.notready = 1;
    mhub_plug(&devm[2], 1, &stick); mhub_plug(&devm[2], 2, &kbd2); mhub_plug(&devm[2], 3, &fshub); mhub_plug(&fshub, 1, &kbd3);
    int slots_before = cmds_seen[9];
    for (int i = 0; i < 30; i++) { usb_sleep_ms(4); if (i % 5 == 0) hc.ports(&hc); usb_hid_poll(now); hub_poll(); msc_poll(); }
    if (!strstr(logbuf, "mounted at /usb0")) printf("%s", logbuf);
    CHECK(cmds_seen[9] - slots_before == 5 && hub_count() == 2 && msc_count() == 1 && usb_hid_count() == 3);
    CHECK(tt_checked == 3);                                 /* kbd2, FS-хаб, kbd3 - через TT HS-хаба */
    uint32_t hs = slot_of(&devm[2]), fs = slot_of(&fshub);
    CHECK(hs && fs && slots[hs].hub && slots[hs].hub_ports == 4 && slots[fs].hub && slots[fs].hub_ports == 4);
    CHECK(strstr(logbuf, "port 3.3.1: device") && strstr(logbuf, "storage: FAT16 'KIRASTICK' mounted at /usb0"));
    char sb[32] = { 0 };
    CHECK(vfs_read("/usb0/hello.txt", 0, sb, sizeof sb) == 12 && !strcmp(sb, "privet, kira"));
    static uint8_t big[100000], back[100000];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 13u + 5u);
    vfs_info_t vi[4]; CHECK(vfs_mounts(vi, 4) == 1 && vi[0].read_only);
    int writes_before = stick.writes;
    CHECK(vfs_read("/usb0/big.bin", 0, back, sizeof back) == (int64_t)sizeof back && !memcmp(big, back, sizeof big));
    CHECK(stick.writes == writes_before);
    CHECK(max_bulk_len == 16384 && bulk_trbs > 20);
    /* STALL на данных: Reset Endpoint, точка пересоздана (Configure Endpoint drop+add), повтор */
    int rst0 = cmds_seen[14], cfg0 = cmds_seen[12], dr0 = drops;
    stick.stall_data_once = 1; memset(back, 0, sizeof back);
    CHECK(vfs_read("/usb0/big.bin", 0, back, sizeof back) == (int64_t)sizeof back && !memcmp(big, back, sizeof big));
    CHECK(cmds_seen[14] > rst0 && cmds_seen[12] > cfg0 && drops > dr0 && stick.clear_halts >= 1);
    /* устройство отдало меньше обещанного: событие короткого пакета, повтор */
    stick.short_read_once = 1;
    CHECK(vfs_read("/usb0/big.bin", 0, back, sizeof back) == (int64_t)sizeof back && !memcmp(big, back, sizeof big) && !stick.short_read_once);
    /* клавиатуры за хабами */
    uint8_t r_x[8] = { 0, 0, 0x1B }, r_y[8] = { 0, 0, 0x1C };
    mdev_report(&kbd2, r_x, 8); mdev_report(&kbd2, r0, 8); mdev_report(&kbd3, r_y, 8); mdev_report(&kbd3, r0, 8);
    for (int i = 0; i < 20; i++) { usb_sleep_ms(2); usb_hid_poll(now); }
    nt = 0; memset(typed, 0, sizeof typed);
    while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch && nt < 31) typed[nt++] = (char)ev.ch;
    CHECK(!strcmp(typed, "xy"));
    /* флешку вынули: точки сняты, слот освобождён, /usb0 исчез */
    int dis0 = cmds_seen[10]; dr0 = drops;
    mhub_unplug(&devm[2], 1);
    for (int i = 0; i < 5; i++) { usb_sleep_ms(4); hub_poll(); msc_poll(); }
    CHECK(msc_count() == 0 && cmds_seen[10] == dis0 + 1 && drops >= dr0 + 2 && vfs_read("/usb0/hello.txt", 0, sb, 12) < 0);
    /* хаб выдернули из корневого порта: всё дерево снято */
    dis0 = cmds_seen[10];
    devm[2].connected = 0;
    usb_sleep_ms(1); hc.ports(&hc);
    CHECK(hub_count() == 0 && usb_hid_count() == 1 && cmds_seen[10] == dis0 + 4);
    printf("xhci: hubs (route string, TT for LS/FS behind HS hub, hub slot context), flash stick (bulk 16 KB TRBs, STALL recovery, short data), /usb0 read-only, unplug from hub, whole tree removal passed\n");
    printf("xhci: BIOS handoff, reset, DCBAA+scratchpad, command/event rings, Enable Slot, Address Device, Evaluate Context, Configure Endpoint, control (short, STALL), interrupt reports across ring wrap, unplug/Disable Slot passed (%d events)\n", events_written);
    return 0;
}
