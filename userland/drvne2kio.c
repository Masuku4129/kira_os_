/* Тестовый драйвер Ring 3 для NE2000 PCI (10ec:8029, QEMU -device ne2k_pci): единственная цель - проверить KDRV_SYS_IO на живом устройстве.
   Читает MAC из PROM карты через порты I/O BAR 0 (ширины 1/2/4, одиночные и ins), затем пробует то, что ядро обязано отклонить:
   чужой номер BAR, выход за BAR, невыровненный доступ, неверная ширина, отображение портов как памяти. Только x86. */
#include "kdrv.h"

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void put(i64 v) { char b[24]; int n = 0; u64 x = v < 0 ? (u64)-v : (u64)v; do { b[n++] = (char)('0' + x % 10); x /= 10; } while (x); if (v < 0) b[n++] = '-'; char o[26]; int k = 0; while (n) o[k++] = b[--n]; o[k] = 0; say(o); }
static void hx(u32 v) { char o[3]; o[0] = "0123456789abcdef"[(v >> 4) & 15]; o[1] = "0123456789abcdef"[v & 15]; o[2] = 0; say(o); }

#define CR 0x00
#define PSTART 0x01
#define RSAR0 0x08
#define RSAR1 0x09
#define RBCR0 0x0A
#define RBCR1 0x0B
#define ISR 0x07
#define DCR 0x0E
#define DATA 0x10
#define RESET 0x1F

KIRA_ENTRY()
int umain(void) {
    kdrv_info_t in;
    if (kdrv_info(&in) != 0) { say("drvne2kio: info refused (FAIL)\n"); return 1; }
    say("drvne2kio: bar0 flags "); put(in.bar[0].flags); say(" size "); put((i64)in.bar[0].size); say("\n");
    int ok = 1;
    if (!(in.bar[0].flags & KDRV_BAR_IO)) { say("drvne2kio: bar0 is not I/O (FAIL)\n"); return 1; }
    i64 r = kdrv_io_in(0, RESET, 1);                         /* сброс: чтение и запись порта сброса */
    if (r < 0) { say("drvne2kio: reset read rc="); put(r); say(" (FAIL)\n"); return 1; }
    kdrv_io_out(0, RESET, 1, (u32)r);
    for (int i = 0; i < 100000 && !(kdrv_io_in(0, ISR, 1) & 0x80); i++) { }
    i64 isr = kdrv_io_in(0, ISR, 1);
    say("drvne2kio: isr after reset="); put(isr); say("\n");
    if (!(isr & 0x80)) ok = 0;
    kdrv_io_out(0, CR, 1, 0x21);                             /* стоп, страница 0 */
    kdrv_io_out(0, DCR, 1, 0x48);                            /* побайтный режим */
    kdrv_io_out(0, RBCR0, 1, 12); kdrv_io_out(0, RBCR1, 1, 0);
    kdrv_io_out(0, RSAR0, 1, 0); kdrv_io_out(0, RSAR1, 1, 0);
    kdrv_io_out(0, CR, 1, 0x0A);                             /* чтение удалённым DMA */
    u8 prom[12];
    i64 rc = kdrv_io_ins(0, DATA, 1, prom, 12);
    say("drvne2kio: ins rc="); put(rc); say(" mac=");
    for (int i = 0; i < 12; i += 2) { hx(prom[i]); if (i < 10) say(":"); }
    say("\n");
    if (rc != 0) ok = 0;
    /* то, что ядро обязано отказать */
    i64 a = kdrv_io_in(1, 0, 1);                             /* BAR 1 - память: не порты */
    i64 b = kdrv_io_in(0, (u32)in.bar[0].size, 1);           /* за концом BAR */
    i64 c = kdrv_io_in(0, 1, 4);                             /* невыровнено */
    i64 d = kdrv_io_in(0, 0, 3);                             /* ширина 3 */
    i64 e = kdrv_io_in(7, 0, 1);                             /* нет такого BAR */
    i64 f = kdrv_io_ins(0, DATA, 1, (void*)0x10, 4);         /* буфер не в памяти драйвера */
    i64 g = ksys(KDRV_SYS_MMIO_MAP, 0, 0, 4096);             /* порты как память: отказ */
    say("drvne2kio: refused: mem-bar="); put(a); say(" past="); put(b); say(" unaligned="); put(c); say(" width3="); put(d);
    say(" no-bar="); put(e); say(" bad-buf="); put(f); say(" map-ports="); put(g); say("\n");
    if (a >= 0 || b >= 0 || c >= 0 || d >= 0 || e >= 0 || f >= 0 || g >= 0) ok = 0;
    say(ok ? "drvne2kio: PASS\n" : "drvne2kio: partial (see above)\n");
    for (;;) { i64 w = kdrv_irq_wait(100); if (w == KE_INTR) return 0; if (w < 0) k_sleep(100); }
}
