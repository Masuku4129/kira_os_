/* Тестовый драйвер Ring 3 для устройства QEMU «edu» (1234:11e8, -device edu): регистры MMIO, прерывание MSI, DMA, а заодно
   попытки выйти за свои права (чужой BAR, защищённая конфигурация), которые ядро обязано отклонить. Обе архитектуры.
   Аргумент запуска «crash» - после проверки обратиться по нулевому адресу (ядро живо, ресурсы возвращаются). */
#include "kdrv.h"

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void put(i64 v) { char b[24]; int n = 0; u64 x = v < 0 ? (u64)-v : (u64)v; do { b[n++] = (char)('0' + x % 10); x /= 10; } while (x); if (v < 0) b[n++] = '-'; char o[26]; int k = 0; while (n) o[k++] = b[--n]; o[k] = 0; say(o); }
static void hex(u64 v) { char o[19]; int k = 0; o[k++] = '0'; o[k++] = 'x'; int started = 0; for (int s = 60; s >= 0; s -= 4) { int d = (int)((v >> s) & 15); if (d || started || s == 0) { o[k++] = "0123456789abcdef"[d]; started = 1; } } o[k] = 0; say(o); }

#define ID 0x00
#define LIVE 0x04
#define FACT 0x08
#define STATUS 0x20
#define IRQ_RAISED 0x24
#define IRQ_ACK 0x64
#define DMA_SRC 0x80
#define DMA_DST 0x88
#define DMA_CNT 0x90
#define DMA_CMD 0x98
#define DEV_BUF 0x40000
#define E_PERM (-12)

KIRA_ENTRY()
int umain(void) {
    kdrv_info_t in;
    if (kdrv_info(&in) != 0) { say("drvedu: info refused (FAIL)\n"); return 1; }
    say("drvedu: device "); hex(in.vendor); say(":"); hex(in.device); say(", rights "); hex(in.granted); say(in.flags & KDRV_INFO_MSI ? ", MSI\n" : ", no MSI\n");
    char args[16] = { 0 };
    k_args(args, sizeof args);
    int crash = args[0] == 'c';
    i64 err = 0;
    volatile void* m = kdrv_mmio_map(0, 0, 0x100000, &err);
    if (!m) { say("drvedu: mmio rc="); put(err); say("\n"); return 1; }
    int ok = 1;
    u32 id = KDRV_R32(m, ID);
    say("drvedu: id="); hex(id); say("\n");
    if ((id & 0xFF) != 0xED) ok = 0;
    KDRV_R32(m, LIVE) = 0x12345678;
    if (KDRV_R32(m, LIVE) != ~0x12345678u) { say("drvedu: liveness (FAIL)\n"); ok = 0; }
    KDRV_R32(m, FACT) = 10;
    for (int i = 0; i < 1000000 && (KDRV_R32(m, STATUS) & 1); i++) { __asm__ volatile ("" ::: "memory"); }
    u32 f = KDRV_R32(m, FACT);
    say("drvedu: 10!="); put(f); say("\n");
    if (f != 3628800) ok = 0;
    if (in.flags & KDRV_INFO_MSI) {
        KDRV_R32(m, STATUS) = 0x80;                          /* прерывание по окончании вычисления */
        KDRV_R32(m, FACT) = 5;
        i64 ev = kdrv_irq_wait(300);
        say("drvedu: irq wait="); put(ev); say(" 5!="); put(KDRV_R32(m, FACT)); say("\n");
        if (ev != KDRV_EV_IRQ || KDRV_R32(m, FACT) != 120) ok = 0;
        KDRV_R32(m, IRQ_ACK) = KDRV_R32(m, IRQ_RAISED);
        KDRV_R32(m, STATUS) = 0;
    } else say("drvedu: irq none (skipped)\n");
    kdrv_dma_t d;
    i64 rc = kdrv_dma_alloc(4096, 30, &d);                   /* адреса edu - 30 бит */
    say("drvedu: dma rc="); put(rc); say("\n");
    if (rc == -12) {
        say("drvedu: dma denied (expected without CAP_DMA/IOMMU)\n");
    } else if (rc == 0) {
        volatile u8* p = (volatile u8*)d.va;
        for (int i = 0; i < 256; i++) p[i] = (u8)(i * 7 + 1);
        KDRV_R32(m, DMA_SRC) = (u32)d.phys; KDRV_R32(m, DMA_SRC + 4) = 0;
        KDRV_R32(m, DMA_DST) = DEV_BUF; KDRV_R32(m, DMA_DST + 4) = 0;
        KDRV_R32(m, DMA_CNT) = 256; KDRV_R32(m, DMA_CNT + 4) = 0;
        KDRV_R32(m, DMA_CMD) = 1;
        for (int i = 0; i < 1000000 && (KDRV_R32(m, DMA_CMD) & 1); i++) { __asm__ volatile ("" ::: "memory"); }
        KDRV_R32(m, DMA_SRC) = DEV_BUF; KDRV_R32(m, DMA_DST) = (u32)d.phys + 2048; KDRV_R32(m, DMA_CNT) = 256;
        KDRV_R32(m, DMA_CMD) = 3;                            /* из устройства в память */
        for (int i = 0; i < 1000000 && (KDRV_R32(m, DMA_CMD) & 1); i++) { __asm__ volatile ("" ::: "memory"); }
        int same = 1;
        for (int i = 0; i < 256; i++) if (p[2048 + i] != p[i]) same = 0;
        say("drvedu: dma round trip "); say(same ? "equal\n" : "DIFFERENT (FAIL)\n");
        if (!same) ok = 0;
    } else ok = 0;
    /* то, что ядро обязано отказать */
    i64 r1 = ksys(KDRV_SYS_MMIO_MAP, 1, 0, 4096);            /* BAR, которого у устройства нет */
    i64 r2 = ksys(KDRV_SYS_MMIO_MAP, 0, 0x100000, 4096);     /* за концом своего BAR */
    i64 r3 = kdrv_cfg_write(0x10, 4, 0xFEE00000u);           /* перенести BAR на чужие адреса */
    i64 r4 = kdrv_cfg_write(0x04, 2, 0x8);                   /* бит команды, которого драйверу не дано */
    i64 r5 = kdrv_cfg_read(0x00, 4);
    say("drvedu: refused: other-bar="); put(r1); say(" past-bar="); put(r2); say(" bar-move="); put(r3); say(" bad-cmd="); put(r4); say(" cfg-id="); hex((u64)r5); say("\n");
    if (r1 >= 0 || r2 >= 0 || r3 != E_PERM || r4 != E_PERM || (r5 & 0xFFFF) != 0x1234) ok = 0;
    say(ok ? "drvedu: PASS\n" : "drvedu: partial (see above)\n");
    if (crash) { say("drvedu: crashing on purpose\n"); *(volatile u32*)0 = 1; }
    for (;;) { i64 e = kdrv_irq_wait(100); if (e == KE_INTR) return 0; if (e < 0) k_sleep(100); }
}
