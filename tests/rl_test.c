// Драйвер drivers/rl/rl.c против модели RTL8139 на хосте. Модель написана по описанию чипа (регистры, 93C46 побитово, кольцо приёма с переходом
// через конец, четыре передающих дескриптора) и по поведению, на которое рассчитывает rl(4); настоящий QEMU -device rtl8139 - только на стенде.
// Сборка (tests/run_host_tests.sh): rl.c с -Dmain=rl_main, kdrv_bsd.h/kira.h - копии из userland с подменой доступа к регистрам на вызовы модели.
// Роль ядра играет ksys(): IRQ_WAIT - ход сценария (кладёт кадры в кольцо передачи, читает кольцо приёма), MMIO_MAP/DMA_ALLOC/NETDEV - как kdrv.c.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#include "kdrv_format.h"

#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
int rl_main(void);

/* ---- модель чипа ---- */
enum { RXBUFLEN = 8192 << 3 };
static struct {
    u8 regs[256];
    u16 eeprom[64];
    int ee_bits, ee_cs, ee_sk, ee_do, ee_out_pos; u32 ee_acc; int ee_have;
    u32 rbstart, rx_addr, rx_ptr;        /* rx_addr - куда пишет чип, rx_ptr - откуда читает драйвер (CAPR + 16) */
    u32 txaddr[4], txstat[4];
    int rxen, txen, link_up;
    u32 rxcfg;
    unsigned tx_count, resets, rx_bad;
    struct { u8 d[2048]; u32 len; } tx[256]; unsigned ntx;
    u32 mar[2];
} M;

static u8* phys2ptr(u32 pa) { return (u8*)(uintptr_t)pa; }
static void model_reset(void) {
    u8 idr[6]; memcpy(idr, M.regs, 6);
    memset(M.regs, 0, sizeof M.regs);
    if (M.eeprom[7] == 0xffff) memcpy(M.regs, idr, 6);              /* EEPROM пуст: адрес станции остаётся тем, что записан в IDR */
    else for (int i = 0; i < 3; i++) { M.regs[i * 2] = (u8)(M.eeprom[7 + i] & 0xff); M.regs[i * 2 + 1] = (u8)(M.eeprom[7 + i] >> 8); }
    M.rx_addr = M.rx_ptr = 0; M.rxen = M.txen = 0; memset(M.txstat, 0, sizeof M.txstat); M.resets++; M.rxcfg = 0;
    M.ee_cs = 0; M.ee_sk = 0; M.ee_bits = 0;
}
static int rx_empty(void) { return ((RXBUFLEN + M.rx_addr - M.rx_ptr) % RXBUFLEN) == 0; }

static void ee_edge(u8 v) {
    int cs = !!(v & 0x08), sk = !!(v & 0x04), di = !!(v & 0x02);
    if (!(v & 0x80) || !cs) {                                       /* не режим «программирование» или CS снят - обмен с 93C46 прекращён */ M.ee_bits = 0; M.ee_acc = 0; M.ee_have = 0; M.ee_do = 0; M.ee_cs = 0; M.ee_sk = sk; return; }
    if (sk && !M.ee_sk) {                                           /* фронт тактового сигнала */
        if (M.ee_bits < 9) {
            M.ee_acc = (M.ee_acc << 1) | (u32)di; M.ee_bits++;
            if (M.ee_bits == 9) {
                if ((M.ee_acc >> 6) == 6) { M.ee_have = 1; M.ee_out_pos = 0; M.ee_do = 0; }   /* 1 10 + адрес: чтение; на выходе сначала нуль */
                else M.ee_have = 0;
            }
        } else if (M.ee_have && M.ee_out_pos < 16) {
            u16 w = M.eeprom[M.ee_acc & 63];
            M.ee_do = (w >> (15 - M.ee_out_pos)) & 1; M.ee_out_pos++;
        }
    }
    M.ee_cs = cs; M.ee_sk = sk;
}

static void tx_go(int i) {
    u32 st = M.txstat[i], len = st & 0x1fff;
    if (!M.txen || !len) return;
    CHECK(len <= 1792);
    CHECK(M.ntx < 256);
    memcpy(M.tx[M.ntx].d, phys2ptr(M.txaddr[i]), len); M.tx[M.ntx].len = len; M.ntx++;
    M.txstat[i] = len | 0x2000 | 0x8000;                                // OWN (DMA завершён) и TOK
    M.tx_count++;
}

u32 rtl_model_read(int size, int off) {
    u32 v = 0;
    if (off == 0x37) return (u32)((M.txen ? 4 : 0) | (M.rxen ? 8 : 0) | (rx_empty() ? 1 : 0));
    if (off == 0x38 && size == 2) return (M.rx_ptr - 16) & 0xffff;
    if (off == 0x3a && size == 2) return M.rx_addr & 0xffff;
    if (off >= 0x10 && off < 0x20 && size == 4) return M.txstat[(off - 0x10) / 4];
    if (off >= 0x20 && off < 0x30 && size == 4) return M.txaddr[(off - 0x20) / 4];
    if (off == 0x30 && size == 4) return M.rbstart;
    if (off == 0x44 && size == 4) return M.rxcfg;
    if (off == 0x50 && size == 1) return (u32)((M.regs[0x50] & ~1) | (M.ee_cs ? M.ee_do : 0));
    if (off == 0x64 && size == 2) return M.link_up ? 0x782d : 0x7829;
    if (off == 0x58 && size == 1) return M.link_up ? 0x90 : 0x94;
    for (int i = 0; i < size; i++) v |= (u32)M.regs[off + i] << (8 * i);
    return v;
}

void rtl_model_write(int size, int off, u32 val) {
    if (off == 0x37 && size == 1) {
        if (val & 0x10) { model_reset(); return; }                  // сброс: бит сам снимается
        M.txen = !!(val & 4); M.rxen = !!(val & 8); return;
    }
    if (off == 0x38 && size == 2) { M.rx_ptr = ((val & 0xffff) + 16) % RXBUFLEN; return; }
    if (off >= 0x10 && off < 0x20 && size == 4) { int i = (off - 0x10) / 4; M.txstat[i] = val & ~0xa000u; tx_go(i); return; }
    if (off >= 0x20 && off < 0x30 && size == 4) { M.txaddr[(off - 0x20) / 4] = val; return; }
    if (off == 0x30 && size == 4) { M.rbstart = val; return; }
    if (off == 0x44 && size == 4) { M.rxcfg = val; return; }
    if (off == 0x50 && size == 1) { M.regs[0x50] = (u8)val; ee_edge((u8)val); return; }
    if (off == 0x3e && size == 2) return;                           // ISR сбрасывается записью единиц
    if (off >= 0x08 && off < 0x10 && size == 4) { M.mar[(off - 8) / 4] = val; }
    for (int i = 0; i < size; i++) M.regs[off + i] = (u8)(val >> (8 * i));
}

static int model_rx_push(const u8* f, u32 len) {
    if (!M.rxen) return -1;
    u32 total = len + 4;
    u32 need = (4 + total + 3) & ~3u;
    u32 free_ = (RXBUFLEN + M.rx_ptr - M.rx_addr - 1) % RXBUFLEN + 0;
    if (M.rx_addr == M.rx_ptr) free_ = RXBUFLEN - 1;
    if (need > free_) return -1;                                    // кольцо полно: чип теряет кадр
    u8* rb = phys2ptr(M.rbstart);
    u32 hdr = 1u | (total << 16);
    u8 tmp[4 + 1600 + 4]; memcpy(tmp, &hdr, 4); memcpy(tmp + 4, f, len); memset(tmp + 4 + len, 0xC5, 4);
    for (u32 i = 0; i < 4 + total; i++) rb[(M.rx_addr + i) % RXBUFLEN] = tmp[i];
    M.rx_addr = (M.rx_addr + need) % RXBUFLEN;
    return 0;
}

/* ---- «ядро» ---- */
static kdrv_netctl_t* ctl;
static u8 mac_reg[6];
static int mode, iter;
static unsigned got_rx, sent_tx;
static u8 rxlog[4096][64]; static u32 rxlen[4096];
static char outlog[8192]; static unsigned outn;
static u64 clock_ns;
static int inject_done;
static unsigned bad_injected;

static void kern_send(const u8* f, u32 len) {
    u32 head = ctl->tx_head;
    CHECK(head - ctl->tx_tail < ctl->slots);
    kdrv_slot_t* s = (kdrv_slot_t*)((u8*)ctl + KDRV_NET_TX_OFF) + (head % ctl->slots);
    memcpy(s->data, f, len); s->len = len;
    __atomic_store_n(&ctl->tx_head, head + 1, __ATOMIC_RELEASE);
}
static void kern_drain_rx(void) {
    while (ctl->rx_tail != ctl->rx_head) {
        kdrv_slot_t* s = (kdrv_slot_t*)((u8*)ctl + KDRV_NET_RX_OFF) + (ctl->rx_tail % ctl->slots);
        CHECK(s->len >= 14 && s->len <= 1536);
        if (got_rx < 4096) { memcpy(rxlog[got_rx], s->data, 64 < s->len ? 64 : s->len); rxlen[got_rx] = s->len; }
        got_rx++;
        __atomic_store_n(&ctl->rx_tail, ctl->rx_tail + 1, __ATOMIC_RELEASE);
    }
}
static void mkframe(u8* f, u32 len, u32 seq) {
    memset(f, 0, len);
    memcpy(f, "\x52\x54\x00\x12\x34\x56", 6); memcpy(f + 6, "\x52\x55\x0a\x00\x02\x02", 6); f[12] = 0x08; f[13] = 0x00;
    for (u32 i = 14; i < len; i++) f[i] = (u8)(seq * 31 + i);
    if (len >= 18) memcpy(f + 14, &seq, 4);
}

i64 ksys(u64 n, u64 a, u64 b, u64 c) {
    switch (n) {
    case 1: { unsigned k = (unsigned)b; if (outn + k < sizeof outlog) { memcpy(outlog + outn, (const char*)a, k); outn += k; outlog[outn] = 0; } fwrite((const char*)a, 1, k, stdout); return (i64)b; }
    case 2: printf("exit %d\n", (int)a); exit((int)a);
    case 11: { void* p = mmap(0, a * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return p == MAP_FAILED ? -7 : (i64)p; }
    case 24: clock_ns += 200; return (i64)clock_ns;
    case KDRV_SYS_INFO: {
        kdrv_info_t* in = (kdrv_info_t*)a; memset(in, 0, sizeof *in);
        in->size = sizeof *in; in->vendor = 0x10ec; in->device = 0x8139; in->granted = CAP_MMIO | CAP_DMA | CAP_PCIDEV | CAP_NETDEV;
        in->bar[0].size = 256; in->bar[0].flags = KDRV_BAR_IO; in->bar[1].size = 256;
        return 0;
    }
    case KDRV_SYS_CFG_READ: return a == 4 ? 0x0006 : -10;
    case KDRV_SYS_CFG_WRITE: return a == 4 ? 0 : -12;
    case KDRV_SYS_MMIO_MAP: return a == 1 ? 0x1000 : -2;           // адрес-метка: доступ идёт через модель
    case KDRV_SYS_DMA_ALLOC: {
        kdrv_dma_t* d = (kdrv_dma_t*)c;
        if (b && b < 40 && (b != 32)) return -10;
        u64 bytes = (a + 4095) & ~4095ull;
        void* p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);   // физический адрес = адрес в первых 2 ГБ
        if (p == MAP_FAILED) return -7;
        d->va = (u64)p; d->phys = (u64)p; d->bytes = bytes; return 0;
    }
    case KDRV_SYS_NETDEV:
        if (a == KDRV_NET_REGISTER) {
            memcpy(mac_reg, (const void*)b, 6);
            ctl = mmap(0, KDRV_NET_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            ctl->slots = KDRV_NET_SLOTS; ctl->slot_size = KDRV_NET_SLOT; ctl->max_frame = KDRV_NET_MAXFRAME;
            return (i64)ctl;
        }
        return 0;
    case KDRV_SYS_IRQ_WAIT: {
        iter++;
        if (!ctl) return -5;
        kern_drain_rx();
        u8 f[1600];
        if (mode == 0) {                                            // обычный ход: передача и приём разных размеров, кольцо обходит конец
            if (iter == 2) { mkframe(f, 42, 1000); kern_send(f, 42); sent_tx++; }                 // ARP-размер: должен уйти дополненным до 60
            if (iter == 3) { for (unsigned i = 0; i < 12; i++) { mkframe(f, 60 + i * 120, 2000 + i); kern_send(f, 60 + i * 120); sent_tx++; } }   // больше четырёх дескрипторов
            if (iter == 6) { mkframe(f, 1514, 3000); kern_send(f, 1514); sent_tx++; }
            if (iter >= 5 && iter < 5 + 90 && inject_done < 300) {
                for (int k = 0; k < 4 && inject_done < 300; k++) {
                    u32 len = 60 + (u32)(inject_done * 37) % 1455;
                    mkframe(f, len, 7000 + (u32)inject_done);
                    if (model_rx_push(f, len) == 0) inject_done++; else break;
                }
            }
            if (iter == 110) { M.link_up = 0; }
            if (iter == 160) { CHECK(ctl->link == 0); M.link_up = 1; }
            if (iter == 210) { CHECK(ctl->link == 1); return -11; }
        } else if (mode == 1) {                                     // испорченная голова кадра: драйвер перезапускает чип и продолжает
            if (iter == 3) { u8* rb = phys2ptr(M.rbstart); u32 bad = 1 | (3u << 16); memcpy(rb + M.rx_addr, &bad, 4); M.rx_addr = (M.rx_addr + 8) % RXBUFLEN; bad_injected = 1; }
            if (iter == 10) { mkframe(f, 100, 9000); CHECK(model_rx_push(f, 100) == 0); }
            if (iter == 20) return -11;
        } else if (mode == 2) {                                     // EEPROM пуст: MAC берётся из IDR
            if (iter == 5) return -11;
        }
        return -5;                                                  // срок вышел
    }
    case 4: return 0;
    default: printf("ksys: unexpected call %llu\n", n); exit(3);
    }
}

int main(int argc, char** argv) {
    mode = argc > 1 ? atoi(argv[1]) : 0;
    const u8 mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    for (int i = 0; i < 64; i++) M.eeprom[i] = 0xffff;
    M.eeprom[0] = 0x8129;
    if (mode != 2) for (int i = 0; i < 3; i++) M.eeprom[7 + i] = (u16)(mac[i * 2] | (mac[i * 2 + 1] << 8));
    M.link_up = 1;
    if (mode == 2) { static const u8 idr[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc }; memcpy(M.regs, idr, 6); }
    else for (int i = 0; i < 6; i++) M.regs[i] = mac[i];
    int rc = rl_main();
    CHECK(rc == 0);
    CHECK(ctl);
    if (mode == 2) {
        CHECK(!memcmp(mac_reg, "\x52\x54\x00\xaa\xbb\xcc", 6));
        CHECK(strstr(outlog, "(IDR)"));
        printf("rl_test mode 2 (empty EEPROM -> MAC from IDR) passed\n");
        return 0;
    }
    CHECK(!memcmp(mac_reg, mac, 6) && strstr(outlog, "(EEPROM)"));
    CHECK(strstr(outlog, "EEPROM id 8129"));
    CHECK(M.mar[0] == 0xffffffff && M.mar[1] == 0xffffffff);
    CHECK((M.rxcfg & 0x1800) == 0x1800 && (M.rxcfg & 0xE000) == 0xE000 && (M.rxcfg & 0xe) == 0xe);
    if (mode == 0) {
        kern_drain_rx();
        CHECK(sent_tx == 14 && M.ntx == 14);
        CHECK(M.tx[0].len == 60 && !memcmp(M.tx[0].d + 42, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 18));        // дополнено нулями
        u8 f[1600], g[1600];
        mkframe(f, 42, 1000); CHECK(!memcmp(M.tx[0].d, f, 42));
        for (unsigned i = 0; i < 12; i++) { u32 len = 60 + i * 120; mkframe(g, len, 2000 + i); CHECK(M.tx[1 + i].len == len && !memcmp(M.tx[1 + i].d, g, len)); }
        mkframe(g, 1514, 3000); CHECK(M.tx[13].len == 1514 && !memcmp(M.tx[13].d, g, 1514));
        CHECK(inject_done == 300);
        CHECK(got_rx == 300);
        for (unsigned i = 0; i < 300; i++) {
            u32 len = 60 + (i * 37) % 1455; mkframe(f, len, 7000 + i);
            CHECK(rxlen[i] == len && !memcmp(rxlog[i], f, 64 < len ? 64 : len));
        }
        CHECK(M.resets == 2);                                       // чтение MAC и rl_init: по одному сбросу чипа
    } else {
        CHECK(bad_injected && M.resets == 3);               // перезапуск после испорченной головы
        CHECK(got_rx == 1 && rxlen[0] == 100);
        u8 f[100]; mkframe(f, 100, 9000); CHECK(!memcmp(rxlog[0], f, 64));
    }
    printf("rl_test mode %d passed: MAC from EEPROM, rings, padding, wrap-around, link, restart on bad header (%u tx, %u rx, %u chip resets)\n", mode, M.tx_count, got_rx, M.resets);
    return 0;
}
