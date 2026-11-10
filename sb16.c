/* Creative Sound Blaster 16 (ISA): вывод 48 кГц 16 бит стерео за audio_card_t, как у HDA и AC'97.
   Откуда что берётся:
   - есть ли вообще шина ISA - от платформы: бит LEGACY_DEVICES в FADT (acpi_info_t.legacy_isa), шина ISA в таблицах MP
     или мост PCI-ISA/LPC (класс 06.01) на шине PCI;
     без этого порты не трогаются;
   - базовый порт - из набора, который задаёт сама SB16 перемычками (0x220, 0x240, 0x260, 0x280): карта признаётся по
     ответу на сброс DSP (0xAA) и версии DSP 4.xx; других источников порта у ISA нет (в QEMU карты нет в ACPI);
   - прерывание и каналы DMA - читаются из смесителя карты (регистры 0x80 и 0x81), ничего не зашито;
   - порты контроллера 8237 - из номера канала (стандарт ПК: ведущий 0x00-0x0F, ведомый 0xC0-0xDF, страницы 0x80-0x8F).
   Вывод - 16-битный канал DMA (5..7) в режиме автоповтора по кольцу 128 КБ (предел 8237 для 16-битного канала: 64 К слов)
   в памяти ниже 16 МБ, выровненной по 128 КБ. Позиция - текущий адрес канала 8237. Прерывание конца блока не ждём
   (микшер опрашивает карту каждые ~10 мс): его только подтверждаем (service), линия IRQ остаётся замаскированной. */
#include <stdint.h>
#include "sb16.h"

int sb16_irq_from_mixer(uint8_t r) {
    switch (r & 0x0F) { case 1: return 2; case 2: return 5; case 4: return 7; case 8: return 10; default: return -1; }
}
int sb16_dma8_from_mixer(uint8_t r) {
    switch (r & 0x0B) { case 1: return 0; case 2: return 1; case 8: return 3; default: return -1; }
}
int sb16_dma16_from_mixer(uint8_t r) {
    switch (r & 0xE0) { case 0x20: return 5; case 0x40: return 6; case 0x80: return 7; default: return -1; }
}
uint16_t sb16_dma16_port_addr(int ch)  { return ch >= 5 && ch <= 7 ? (uint16_t)(0xC0 + 4 * (ch - 4)) : 0; }
uint16_t sb16_dma16_port_count(int ch) { return ch >= 5 && ch <= 7 ? (uint16_t)(0xC2 + 4 * (ch - 4)) : 0; }
uint16_t sb16_dma16_port_page(int ch)  { return ch == 5 ? 0x8B : ch == 6 ? 0x89 : ch == 7 ? 0x8A : 0; }

#ifndef KIRA_HOST_TEST
#include "acpi.h"
#include "audio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "uart.h"
#include "cpu.h"

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

enum { DSP_RESET = 0x6, DSP_READ = 0xA, DSP_WRITE = 0xC, DSP_RSTAT = 0xE, DSP_ACK16 = 0xF, MIX_ADDR = 0x4, MIX_DATA = 0x5 };
enum { MIX_IRQ = 0x80, MIX_DMA = 0x81, MIX_ISTAT = 0x82 };
#define RATE 48000
#define BUFSZ 131072                         /* 128 КБ = 64 К слов: предел одного прохода 16-битного канала 8237 (~680 мс) */
#define BUFPAGES (BUFSZ / 4096)
#define ISA_DMA_TOP 0x1000000ull             /* 8237 адресует 24 бита */

static struct {
    uint16_t base; uint8_t vmaj, vmin;
    int irq, dma8, dma16;
    uint8_t* buf; uint64_t buf_p;
    uint64_t acks, pos_retries;
    uint16_t dma_addr, dma_cnt, dma_page;
} sb;

static int dsp_write(uint8_t v) {
    for (int i = 0; i < 20000; i++) {
        if (!(inb((uint16_t)(sb.base + DSP_WRITE)) & 0x80)) { outb((uint16_t)(sb.base + DSP_WRITE), v); return 0; }
        cpu_io_delay();
    }
    return -1;
}
static int dsp_read(void) {
    for (int i = 0; i < 20000; i++) {
        if (inb((uint16_t)(sb.base + DSP_RSTAT)) & 0x80) return inb((uint16_t)(sb.base + DSP_READ));
        cpu_io_delay();
    }
    return -1;
}
static uint8_t mix_read(uint8_t reg) { outb((uint16_t)(sb.base + MIX_ADDR), reg); return inb((uint16_t)(sb.base + MIX_DATA)); }
static void mix_write(uint8_t reg, uint8_t v) { outb((uint16_t)(sb.base + MIX_ADDR), reg); outb((uint16_t)(sb.base + MIX_DATA), v); }

/* Сброс DSP по порту base: запись 1, пауза >= 3 мкс, запись 0, через <= 100 мкс в порту чтения лежит 0xAA. */
static int dsp_reset(uint16_t base) {
    outb((uint16_t)(base + DSP_RESET), 1);
    for (int i = 0; i < 8; i++) cpu_io_delay();
    outb((uint16_t)(base + DSP_RESET), 0);
    for (int i = 0; i < 2000; i++) {
        if (inb((uint16_t)(base + DSP_RSTAT)) & 0x80) return inb((uint16_t)(base + DSP_READ)) == 0xAA ? 0 : -1;
        cpu_io_delay();
    }
    return -1;
}

/* Кольцо: ровно 32 страницы, физический адрес кратен 128 КБ и целиком ниже 16 МБ. */
static uint8_t* dma_ring_alloc(uint64_t* phys) {
    uint64_t pa = pmm_alloc_contiguous_aligned_below(BUFPAGES, BUFPAGES, ISA_DMA_TOP);
    if (!pa || pa > ISA_DMA_TOP - BUFSZ) return 0;
    for (uint32_t i = 0; i < BUFPAGES; i++) {                     /* процессор и устройство видят один адрес */
        uint64_t t;
        if (vmm_translate(pa + (uint64_t)i * 4096, &t) != 0 || t != pa + (uint64_t)i * 4096) {
            pmm_free_contiguous(pa, BUFPAGES);
            return 0;
        }
    }
    uint8_t* p = (uint8_t*)(uintptr_t)pa;
    for (uint32_t i = 0; i < BUFSZ; i++) p[i] = 0;
    *phys = pa;
    return p;
}

static void dma_mask(int on) { outb(0xD4, (uint8_t)((on ? 4 : 0) | (sb.dma16 - 4))); }

static int out_start(void) {
    /* 8237 (ведомый, канал sb.dma16): режим одиночный + автоповтор + чтение из памяти; адрес в словах, счётчик 64 К слов */
    dma_mask(1);
    outb(0xD8, 0);                                                /* сбросить триггер старший/младший байт */
    outb(0xD6, (uint8_t)(0x40 | 0x10 | 0x08 | (sb.dma16 - 4)));
    outb(sb.dma_page, (uint8_t)((sb.buf_p >> 16) & 0xFE));
    outb(sb.dma_addr, (uint8_t)((sb.buf_p >> 1) & 0xFF)); outb(sb.dma_addr, (uint8_t)((sb.buf_p >> 9) & 0xFF));
    outb(sb.dma_cnt, 0xFF); outb(sb.dma_cnt, 0xFF);
    dma_mask(0);
    mix_write(0x22, 0xFF); mix_write(0x04, 0xFF);                 /* общая и голос: максимум; громкостью управляет микшер ядра */
    mix_write(0x30, 0xF8); mix_write(0x31, 0xF8); mix_write(0x32, 0xF8); mix_write(0x33, 0xF8);
    const uint32_t words = BUFSZ / 2 / 2 - 1;                     /* прерывание карты - раз в полкольца (в словах, 16 бит) */
    if (dsp_write(0xD1) || dsp_write(0x41) || dsp_write(RATE >> 8) || dsp_write(RATE & 0xFF)) return -1;
    if (dsp_write(0xB6) || dsp_write(0x30) || dsp_write((uint8_t)(words & 0xFF)) || dsp_write((uint8_t)(words >> 8))) return -1;
    return 0;                                                     /* 0xB6: 16 бит, вывод, автоповтор, FIFO; 0x30: со знаком, стерео */
}

static void out_stop(void) { dsp_write(0xD5); dma_mask(1); }      /* пауза 16-битного вывода и маска канала */

/* Текущий адрес канала 8237 (в словах). Младший байт читается дважды: если он оказался меньше (перескочил между чтениями
   младшего и старшего), значение разорвано - читаем снова. */
static uint32_t out_pos(void) {
    uint16_t v = 0;
    for (int t = 0; t < 8; t++) {
        outb(0xD8, 0);
        uint8_t lo = inb(sb.dma_addr), hi = inb(sb.dma_addr), lo2 = inb(sb.dma_addr);
        v = (uint16_t)((uint16_t)hi << 8 | lo);
        if (lo2 >= lo) break;
        sb.pos_retries++;
    }
    return ((uint32_t)v * 2) % BUFSZ;
}

static void service(void) {                                       /* подтвердить прерывание конца блока, чтобы карта не держала линию */
    uint8_t st = mix_read(MIX_ISTAT);
    if (st & 2) { inb((uint16_t)(sb.base + DSP_ACK16)); sb.acks++; }
    if (st & 1) inb((uint16_t)(sb.base + DSP_RSTAT));
}

static void print(void) {
    uart_print("Sound Blaster 16: DSP "); uart_print_dec(sb.vmaj); uart_print("."); uart_print_dec(sb.vmin);
    uart_print(", порт "); uart_print_hex(sb.base);
    uart_print(", IRQ "); if (sb.irq >= 0) uart_print_dec((uint64_t)sb.irq); else uart_print("не сообщён");
    uart_print(" (не используется: опрос), DMA 8 бит "); if (sb.dma8 >= 0) uart_print_dec((uint64_t)sb.dma8); else uart_print("нет");
    uart_print(", 16 бит "); uart_print_dec((uint64_t)sb.dma16);
    uart_print(", кольцо "); uart_print_hex(sb.buf_p); uart_print(" "); uart_print_dec(BUFSZ / 1024);
    uart_print(" КБ, блоков подтверждено "); uart_print_dec(sb.acks); uart_print("\n");
}

int sb16_probe(void) {
    const acpi_info_t* ai = acpi_get();
    int isa = ai && ai->legacy_isa;                               /* FADT / таблицы MP */
    for (int i = 0; i < pci_count() && !isa; i++) { const pci_dev_t* d = pci_get(i); if (d->class_code == 0x06 && d->subclass == 0x01) isa = 1; }   /* мост PCI-ISA/LPC */
    if (!isa) return -1;                                          /* платформа не сообщает об ISA: портов не трогаем */
    static const uint16_t ports[4] = { 0x220, 0x240, 0x260, 0x280 };   /* перемычки SB16 */
    sb.base = 0;
    for (int i = 0; i < 4 && !sb.base; i++) if (dsp_reset(ports[i]) == 0) sb.base = ports[i];
    if (!sb.base) return -1;
    if (dsp_write(0xE1)) { uart_print("[KIRA SB16] DSP does not accept the version command.\n"); return -2; }
    int vmaj = dsp_read(), vmin = dsp_read();
    if (vmaj < 0 || vmin < 0) { uart_print("[KIRA SB16] DSP did not report its version.\n"); return -2; }
    sb.vmaj = (uint8_t)vmaj; sb.vmin = (uint8_t)vmin;
    if (sb.vmaj < 4) {
        uart_print("[KIRA SB16] Sound Blaster DSP "); uart_print_dec(sb.vmaj); uart_print("."); uart_print_dec(sb.vmin);
        uart_print(" at "); uart_print_hex(sb.base); uart_print(" is older than SB16: no 16-bit output.\n");
        return -3;
    }
    uint8_t r80 = mix_read(MIX_IRQ), r81 = mix_read(MIX_DMA);
    sb.irq = sb16_irq_from_mixer(r80); sb.dma8 = sb16_dma8_from_mixer(r81); sb.dma16 = sb16_dma16_from_mixer(r81);
    if (sb.dma16 < 0) {
        uart_print("[KIRA SB16] the card reports no 16-bit DMA channel (mixer register 0x81 = "); uart_print_hex(r81); uart_print("): 16-bit output is impossible.\n");
        return -4;
    }
    sb.dma_addr = sb16_dma16_port_addr(sb.dma16); sb.dma_cnt = sb16_dma16_port_count(sb.dma16); sb.dma_page = sb16_dma16_port_page(sb.dma16);
    sb.buf = dma_ring_alloc(&sb.buf_p);
    if (!sb.buf) { uart_print("[KIRA SB16] no 128 KB buffer below 16 MB aligned to 128 KB for ISA DMA.\n"); return -5; }
    uart_print("[KIRA SB16] Sound: Sound Blaster 16 DSP "); uart_print_dec(sb.vmaj); uart_print("."); uart_print_dec(sb.vmin);
    uart_print(" at port "); uart_print_hex(sb.base); uart_print(" (DSP reset answered), IRQ ");
    if (sb.irq >= 0) uart_print_dec((uint64_t)sb.irq); else uart_print("not reported");
    uart_print(", DMA 8-bit "); if (sb.dma8 >= 0) uart_print_dec((uint64_t)sb.dma8); else uart_print("none");
    uart_print(", 16-bit "); uart_print_dec((uint64_t)sb.dma16);
    uart_print(" (from the card's mixer registers 0x80/0x81), 48 kHz 16-bit stereo output (commands: sound, beep).\n");
    static audio_card_t card;
    card.name = "Sound Blaster 16"; card.start = out_start; card.pos = out_pos; card.buf = (int16_t*)sb.buf; card.bytes = BUFSZ;
    card.service = service; card.in_ready = 0; card.print = print;
    int rc = audio_attach(&card);
    if (rc != 0) {                                                /* карта не принята: остановить DMA и вернуть память */
        out_stop();
        pmm_free_contiguous(sb.buf_p, BUFPAGES);
        sb.buf = 0;
    }
    return rc;
}
#endif
