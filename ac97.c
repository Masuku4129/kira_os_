/* Intel AC'97 (ICH, 8086:2415 и родня; в Limbo и QEMU - «ac97»): регистры через порты ввода-вывода (BAR0 - микшер
   кодека, BAR1 - шина DMA). Вывод и запись - вечные кольца по 2 с из 32 буферов: карта останавливается на
   «последнем допустимом» буфере, поэтому микшер каждые ~10 мс сдвигает его на шаг позади текущего (service).
   48 кГц 16 бит стерео (частота задаётся, если кодек умеет VRA; иначе у AC'97 она и так 48 кГц). */
#include <stdint.h>
#include "ac97.h"
#include "pci.h"
#include "iommu.h"
#include "audio.h"
#include "usb.h"             /* usb_dma_alloc: память ниже 4 ГБ (адреса буферов AC'97 - 32 бита) */
#include "uart.h"
#include "sched.h"
#include "cpu.h"

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

enum { NAM_RESET = 0x00, NAM_MASTER = 0x02, NAM_PCM_OUT = 0x18, NAM_REC_SEL = 0x1A, NAM_REC_GAIN = 0x1C,
       NAM_POWER = 0x26, NAM_EXT_ID = 0x28, NAM_EXT_CTL = 0x2A, NAM_DAC_RATE = 0x2C, NAM_ADC_RATE = 0x32,
       NAM_VID1 = 0x7C, NAM_VID2 = 0x7E };
enum { BOX_IN = 0x00, BOX_OUT = 0x10, GLOB_CNT = 0x2C, GLOB_STA = 0x30 };
enum { B_BDBAR = 0x0, B_CIV = 0x4, B_LVI = 0x5, B_SR = 0x6, B_PICB = 0x8, B_CR = 0xB };
#define RATE 48000
#define BUFSZ (RATE * 4 * 2)                 /* 2 с */
#define NBD 32
#define PART (BUFSZ / NBD)                   /* 12000 байт = 6000 отсчётов */

static struct {
    uint16_t nam, nabm;
    uint8_t *out, *in; uint64_t out_p, in_p;
    uint32_t *obdl, *ibdl; uint64_t obdl_p, ibdl_p;
    uint32_t vid; int vra, in_up;
    uint64_t restarts;
} a;

static void box_start(uint16_t box, uint32_t bdl_p) {
    uint16_t b = (uint16_t)(a.nabm + box);
    outb((uint16_t)(b + B_CR), 0);
    outb((uint16_t)(b + B_CR), 2);                               /* сброс регистров ящика */
    for (int i = 0; i < 100000 && (inb((uint16_t)(b + B_CR)) & 2); i++) cpu_relax();
    outl((uint16_t)(b + B_BDBAR), bdl_p);
    outb((uint16_t)(b + B_LVI), NBD - 1);
    outw((uint16_t)(b + B_SR), 0x1C);
    outb((uint16_t)(b + B_CR), 1);                               /* пуск */
}
static uint32_t box_pos(uint16_t box) {
    uint16_t b = (uint16_t)(a.nabm + box);
    uint8_t civ, again; uint16_t left; int tries = 0;
    do {                                                         /* номер буфера и остаток - одной парой: если карта */
        civ = inb((uint16_t)(b + B_CIV)) & (NBD - 1);            /* перешла к следующему буферу между чтениями, */
        left = inw((uint16_t)(b + B_PICB));                      /* остаток относится к новому - позиция прыгала назад */
        again = inb((uint16_t)(b + B_CIV)) & (NBD - 1);
    } while (again != civ && ++tries < 4);
    uint32_t done = PART - (uint32_t)left * 2;
    if (done > PART) done = 0;
    return (civ * PART + done) % BUFSZ;
}
static void box_keep(uint16_t box) {                             /* последний допустимый - на шаг позади текущего */
    uint16_t b = (uint16_t)(a.nabm + box);
    uint8_t civ = inb((uint16_t)(b + B_CIV)) & (NBD - 1);
    outb((uint16_t)(b + B_LVI), (uint8_t)((civ + NBD - 1) & (NBD - 1)));
    uint16_t sr = inw((uint16_t)(b + B_SR));
    if (sr & 1) { outw((uint16_t)(b + B_SR), 0x1C); outb((uint16_t)(b + B_CR), 1); a.restarts++; }   /* встала - снова */
    else if (sr & 0x1C) outw((uint16_t)(b + B_SR), 0x1C);
}

static int out_start(void) { box_start(BOX_OUT, (uint32_t)a.obdl_p); return 0; }
static uint32_t out_pos(void) { return box_pos(BOX_OUT); }
static uint32_t in_pos(void) { return box_pos(BOX_IN); }
static void service(void) { box_keep(BOX_OUT); if (a.in_up) box_keep(BOX_IN); }
static void print(void) {
    uart_print("AC'97: кодек "); uart_print_hex(a.vid); uart_print(a.vra ? ", частота задана 48 кГц (VRA)" : ", 48 кГц (без VRA)");
    uart_print(", порты "); uart_print_hex(a.nam); uart_print("/"); uart_print_hex(a.nabm);
    uart_print(", перезапусков DMA "); uart_print_dec(a.restarts); uart_print("\n");
}

static void fill_bdl(uint32_t* bdl, uint64_t buf_p) {
    for (unsigned i = 0; i < NBD; i++) { bdl[2 * i] = (uint32_t)(buf_p + (uint64_t)i * PART); bdl[2 * i + 1] = PART / 2; }   /* отсчётов, без флагов */
}

int ac97_probe(void) {
    const pci_dev_t* p = 0;
    for (int i = 0; i < pci_count() && !p; i++) { const pci_dev_t* d = pci_get(i); if (d->class_code == 0x04 && d->subclass == 0x01) p = d; }
    if (!p) return -1;
    if (!p->bar[0].io || !p->bar[1].io || !p->bar[0].base || !p->bar[1].base) { uart_print("[KIRA AC97] controller without I/O port BARs - not supported.\n"); return -2; }
    int iommu_rc = iommu_isolate_device(p);
    if (iommu_rc != IOMMU_OK && iommu_rc != IOMMU_E_NONE &&
        iommu_rc != IOMMU_E_NOCOVER) {
        uart_print("[KIRA AC97] IOMMU isolation failed; controller disabled.\n");
        return -3;
    }
#if defined(__aarch64__) || defined(__x86_64__)
    if (!iommu_device_ready(p)) {
        uart_print("[KIRA AC97] requester is not isolated or safely unrestricted; controller disabled.\n");
        return -3;
    }
#endif
    a.nam = (uint16_t)p->bar[0].base; a.nabm = (uint16_t)p->bar[1].base;
    pci_enable(p, PCI_CMD_IO | PCI_CMD_INTX_OFF);
    outl((uint16_t)(a.nabm + GLOB_CNT), 2);                     /* снять холодный сброс, прерывания выключены */
    int ready = 0;
    for (int i = 0; i < 200 && !ready; i++) { if (inl((uint16_t)(a.nabm + GLOB_STA)) & (1u << 8)) ready = 1; else sched_sleep_ticks(1); }
    if (!ready) { uart_print("[KIRA AC97] codec did not become ready.\n"); return -3; }
    outw((uint16_t)(a.nam + NAM_RESET), 1);
    for (int i = 0; i < 200 && (inw((uint16_t)(a.nam + NAM_POWER)) & 0xF) != 0xF; i++) sched_sleep_ticks(1);
    a.vid = (uint32_t)inw((uint16_t)(a.nam + NAM_VID1)) << 16 | inw((uint16_t)(a.nam + NAM_VID2));
    outw((uint16_t)(a.nam + NAM_MASTER), 0x0000);               /* 0 дБ, без глушения */
    outw((uint16_t)(a.nam + NAM_PCM_OUT), 0x0808);
    outw((uint16_t)(a.nam + NAM_REC_SEL), 0x0000);               /* запись: микрофон */
    outw((uint16_t)(a.nam + NAM_REC_GAIN), 0x0000);
    if (inw((uint16_t)(a.nam + NAM_EXT_ID)) & 1) {               /* VRA: частоту можно задать - 48 кГц */
        outw((uint16_t)(a.nam + NAM_EXT_CTL), (uint16_t)(inw((uint16_t)(a.nam + NAM_EXT_CTL)) | 1));
        outw((uint16_t)(a.nam + NAM_DAC_RATE), RATE); outw((uint16_t)(a.nam + NAM_ADC_RATE), RATE);
        a.vra = inw((uint16_t)(a.nam + NAM_DAC_RATE)) == RATE;
    }
    a.out = (uint8_t*)usb_dma_alloc(BUFSZ, &a.out_p, 1);
    a.in = (uint8_t*)usb_dma_alloc(BUFSZ, &a.in_p, 1);
    a.obdl = (uint32_t*)usb_dma_alloc(NBD * 8, &a.obdl_p, 1);
    a.ibdl = (uint32_t*)usb_dma_alloc(NBD * 8, &a.ibdl_p, 1);
    if (!a.out || !a.in || !a.obdl || !a.ibdl) { uart_print("[KIRA AC97] no memory below 4 GB for DMA buffers.\n"); return -4; }
#if defined(__aarch64__) || defined(__x86_64__)
    if (iommu_device_isolated(p) &&
        (iommu_map(p, a.out_p, a.out_p, BUFSZ, IOMMU_R | IOMMU_W) != IOMMU_OK ||
         iommu_map(p, a.in_p, a.in_p, BUFSZ, IOMMU_R | IOMMU_W) != IOMMU_OK ||
         iommu_map(p, a.obdl_p, a.obdl_p, NBD * 8, IOMMU_R | IOMMU_W) != IOMMU_OK ||
         iommu_map(p, a.ibdl_p, a.ibdl_p, NBD * 8, IOMMU_R | IOMMU_W) != IOMMU_OK)) {
        iommu_unmap_all(p);
        uart_print("[KIRA AC97] IOMMU DMA mapping failed; controller disabled.\n");
        return -4;
    }
#endif
    pci_enable(p, PCI_CMD_MASTER);
    if (!(pci_read16(p, 0x04) & PCI_CMD_MASTER)) {
        pci_disable_bus_master(p);
        iommu_unmap_all(p);
        uart_print("[KIRA AC97] PCI bus mastering was not granted.\n");
        return -4;
    }
    fill_bdl(a.obdl, a.out_p); fill_bdl(a.ibdl, a.in_p);
    box_start(BOX_IN, (uint32_t)a.ibdl_p);
    a.in_up = 1;
    uart_print("[KIRA AC97] Sound: codec "); uart_print_hex(a.vid); uart_print(", 48 kHz 16-bit stereo output and microphone input, ");
    uart_print(a.vra ? "rate set by VRA" : "fixed 48 kHz"); uart_print(" (commands: sound, beep, record).\n");
    static audio_card_t card;
    card.name = "AC'97"; card.start = out_start; card.pos = out_pos; card.buf = (int16_t*)a.out; card.bytes = BUFSZ;
    card.service = service; card.in_ready = 1; card.in_pos = in_pos; card.in_buf = (const int16_t*)a.in; card.in_bytes = BUFSZ;
    card.in_name = "microphone"; card.print = print;
    int attach_rc = audio_attach(&card);
    if (attach_rc != 0) {
        outb((uint16_t)(a.nabm + BOX_IN + B_CR), 0);
        outb((uint16_t)(a.nabm + BOX_OUT + B_CR), 0);
        pci_disable_bus_master(p);
        iommu_unmap_all(p);
    }
    return attach_rc;
}
