/* Intel High Definition Audio: контроллер по PCI (класс 04.03) и кодек на шине HDA. Команды кодеку - через кольца
   CORB/RIRB; путь звука ищется по графу виджетов: вывод (pin) с выходом -> (смеситель/переключатель) -> ЦАП.
   Поток вывода: 48 кГц, 16 бит, стерео, список буферов (BDL) в памяти, DMA без прерываний (позиция - LPIB). */
#include "hda.h"
#ifdef KIRA_HOST_TEST
/* хост: драйвера нет (тест консоли видит только команды) */
void hdacmd_sound(const char* a) { (void)a; }
void hdacmd_beep(const char* a) { (void)a; }
#else
#include "pci.h"
#include "virtio.h"          /* virtio_dma_alloc, MMIO_* */
#include "vmm.h"
#include "uart.h"
#include "sched.h"
#include "timer.h"
#include "kmutex.h"
#include "cpu.h"
#include "audio.h"

enum { GCAP = 0x00, GCTL = 0x08, STATESTS = 0x0E, INTCTL = 0x20, CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48,
       CORBRP = 0x4A, CORBCTL = 0x4C, CORBSIZE = 0x4E, RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58,
       RINTCNT = 0x5A, RIRBCTL = 0x5C, RIRBSTS = 0x5D, RIRBSIZE = 0x5E };
enum { SD_CTL = 0x00, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FMT = 0x12, SD_BDPL = 0x18, SD_BDPU = 0x1C };
#define RATE   48000
#define BUFSZ  (RATE * 4 * 2)        /* 2 с: 48 кГц x 2 канала x 2 байта */
#define NBDL   4

static struct {
    volatile uint8_t* r;
    volatile uint8_t* sd;            /* дескриптор первого потока вывода */
    uint32_t* corb; uint64_t corb_p;
    uint64_t* rirb; uint64_t rirb_p;
    uint16_t rirb_rp;
    unsigned cad;
    uint32_t vendor;
    unsigned pin, dac, path[4], plen;
    uint8_t* buf; uint64_t buf_p;
    uint32_t* bdl; uint64_t bdl_p;
    int up;
    kmutex_t mx;
} h = { .mx = KMUTEX_INIT };

static uint8_t  r8(unsigned o)  { return MMIO_R8(h.r + o); }
static uint16_t r16(unsigned o) { return MMIO_R16(h.r + o); }
static uint32_t r32(unsigned o) { return MMIO_R32(h.r + o); }
static void w8(unsigned o, uint8_t v)   { MMIO_W8(h.r + o, v); }
static void w16(unsigned o, uint16_t v) { MMIO_W16(h.r + o, v); }
static void w32(unsigned o, uint32_t v) { MMIO_W32(h.r + o, v); }

static int wait_bit(unsigned o, uint32_t mask, uint32_t want, int bytes) {
    for (int i = 0; i < 200000; i++) {
        uint32_t v = bytes == 1 ? r8(o) : bytes == 2 ? r16(o) : r32(o);
        if ((v & mask) == want) return 0;
        cpu_relax();
    }
    return -1;
}

/* Команда кодеку и его ответ (через CORB/RIRB). ~0u - ответа нет. */
static uint32_t verb(unsigned nid, uint32_t v) {
    uint32_t cmd = (uint32_t)h.cad << 28 | (uint32_t)nid << 20 | v;
    uint16_t wp = (uint16_t)((r16(CORBWP) + 1) & 0xFF);
    h.corb[wp] = cmd;
    cpu_mb();
    w16(CORBWP, wp);
    for (int i = 0; i < 1000000; i++) {
        uint16_t rw = r16(RIRBWP) & 0xFF;
        if (rw != h.rirb_rp) {
            h.rirb_rp = (uint16_t)((h.rirb_rp + 1) & 0xFF);
            cpu_mb();
            w8(RIRBSTS, 5);
            return (uint32_t)h.rirb[h.rirb_rp];
        }
        cpu_relax();
    }
    return ~0u;
}
static uint32_t param(unsigned nid, unsigned id) { return verb(nid, 0xF0000u | id); }
static unsigned wtype(unsigned nid) { return (param(nid, 0x09) >> 20) & 15; }

/* Связи узла: до 16 соседей (короткая форма списка). */
static unsigned conns(unsigned nid, unsigned* out) {
    uint32_t len = param(nid, 0x0E);
    if (len & 0x80) return 0;                                   /* длинная форма - не встречается у выходов */
    unsigned n = len & 0x7F, k = 0;
    for (unsigned i = 0; i < n && k < 16; i += 4) {
        uint32_t e = verb(nid, 0xF0200u | i);
        for (unsigned j = 0; j < 4 && i + j < n && k < 16; j++) out[k++] = (e >> (8 * j)) & 0xFF;
    }
    return k;
}

/* Поиск в глубину от вывода до ЦАП (не глубже 4 узлов). */
static int find_dac(unsigned nid, unsigned depth) {
    if (depth >= 4) return 0;
    h.path[depth] = nid; h.plen = depth + 1;
    unsigned t = wtype(nid);
    if (t == 0) { h.dac = nid; return 1; }                      /* выходной преобразователь */
    if (depth && t != 2 && t != 3) return 0;                    /* дальше - только смеситель/переключатель */
    unsigned c[16], n = conns(nid, c);
    for (unsigned i = 0; i < n; i++) {
        if (find_dac(c[i], depth + 1)) {
            if (t == 4 || t == 3) verb(nid, 0x70100u | i);       /* выбрать вход */
            if (t == 2) verb(nid, 0x37000u | i << 8 | 0x7F);     /* вход смесителя: без глушения */
            return 1;
        }
    }
    return 0;
}

static void unmute_path(void) {
    for (unsigned i = 0; i < h.plen; i++) {
        unsigned nid = h.path[i];
        verb(nid, 0x70500u);                                    /* питание D0 */
        uint32_t caps = param(nid, 0x12);                       /* выходной усилитель: 0 дБ (offset) */
        unsigned gain = caps ? (caps & 0x7F) : 0x7F;
        verb(nid, 0x3B000u | gain);
    }
    uint32_t pc = param(h.pin, 0x0C);
    verb(h.pin, 0x70700u | 0x40 | ((pc & (1u << 3)) ? 0x80 : 0));   /* выход вкл.; усилитель наушников, если есть */
    if (pc & (1u << 16)) verb(h.pin, 0x70C02u);                  /* EAPD - внешний усилитель */
}

static int codec_init(void) {
    uint32_t root = param(0, 0x04);
    unsigned first = (root >> 16) & 0xFF, n = root & 0xFF, afg = 0;
    h.vendor = param(0, 0x00);
    for (unsigned g = first; g < first + n && !afg; g++) if ((param(g, 0x05) & 0xFF) == 1) afg = g;
    if (!afg) return -1;
    verb(afg, 0x70500u);                                         /* группа - в D0 */
    uint32_t sub = param(afg, 0x04);
    unsigned w0 = (sub >> 16) & 0xFF, wn = sub & 0xFF;
    int best = -1; unsigned best_pin = 0;
    for (unsigned w = w0; w < w0 + wn; w++) {
        if (wtype(w) != 4 || !(param(w, 0x0C) & (1u << 4))) continue;   /* вывод с возможностью выхода */
        uint32_t cfg = verb(w, 0xF1C00u);
        unsigned conn = cfg >> 30, dev = (cfg >> 20) & 15;
        if (conn == 1) continue;                                 /* ничего не подключено */
        int score = dev == 1 ? 3 : dev == 2 ? 2 : dev == 0 ? 1 : 0;   /* динамик, наушники, линейный выход */
        if (score > best) { best = score; best_pin = w; }
    }
    if (!best_pin) return -2;
    h.pin = best_pin;
    if (!find_dac(h.pin, 0)) return -3;
    unmute_path();
    verb(h.dac, 0x70610u);                                       /* поток 1, канал 0 */
    verb(h.dac, 0x20000u | 0x0011);                              /* 48 кГц, 16 бит, 2 канала */
    return 0;
}

int hda_probe(void) {
    const pci_dev_t* p = 0;
    for (int i = 0; i < pci_count() && !p; i++) { const pci_dev_t* d = pci_get(i); if (d->class_code == 0x04 && d->subclass == 0x03) p = d; }
    if (!p) return -1;
    if (p->bar[0].io || !p->bar[0].base) return -2;
    pci_enable(p, PCI_CMD_MEM | PCI_CMD_MASTER | PCI_CMD_INTX_OFF);
    h.r = (volatile uint8_t*)pci_map_bar(p, 0, 0, 0x2000 <= p->bar[0].size ? 0x2000 : p->bar[0].size);
    if (!h.r) return -3;
    w32(GCTL, r32(GCTL) & ~1u);                                  /* сброс контроллера */
    if (wait_bit(GCTL, 1, 0, 4)) return -4;
    w32(GCTL, r32(GCTL) | 1u);
    if (wait_bit(GCTL, 1, 1, 4)) return -4;
    for (int i = 0; i < 1000 && !r16(STATESTS); i++) sched_sleep_ticks(1);   /* кодеки отзываются за ~0,5 мс */
    uint16_t st = r16(STATESTS);
    if (!st) { uart_print("[KIRA HDA] controller found, no codec on the link.\n"); return -5; }
    while (!(st & (1u << h.cad))) h.cad++;
    w32(INTCTL, 0);
    h.corb = (uint32_t*)virtio_dma_alloc(1024, &h.corb_p);
    h.rirb = (uint64_t*)virtio_dma_alloc(2048, &h.rirb_p);
    h.buf = (uint8_t*)virtio_dma_alloc(BUFSZ, &h.buf_p);
    h.bdl = (uint32_t*)virtio_dma_alloc(NBDL * 16, &h.bdl_p);
    if (!h.corb || !h.rirb || !h.buf || !h.bdl) return -6;
    w8(CORBCTL, 0); w8(RIRBCTL, 0);
    w32(CORBLBASE, (uint32_t)h.corb_p); w32(CORBUBASE, (uint32_t)(h.corb_p >> 32));
    w8(CORBSIZE, 2);                                             /* 256 записей */
    w16(CORBRP, 0x8000); wait_bit(CORBRP, 0x8000, 0x8000, 2);
    w16(CORBRP, 0);      wait_bit(CORBRP, 0x8000, 0, 2);
    w16(CORBWP, 0);
    w32(RIRBLBASE, (uint32_t)h.rirb_p); w32(RIRBUBASE, (uint32_t)(h.rirb_p >> 32));
    w8(RIRBSIZE, 2);
    w16(RIRBWP, 0x8000); h.rirb_rp = 0;
    w16(RINTCNT, 1);
    /* RIRBCTL: DMA ответов и флаг «ответ получен» (RINTCTL). Без флага контроллер (QEMU - так точно) после RINTCNT
       ответов ждёт его снятия и не берёт следующие команды. Прерывания процессору выключены (INTCTL = 0). */
    w8(CORBCTL, 2); w8(RIRBCTL, 3);
    int rc = codec_init();
    if (rc) { uart_print("[KIRA HDA] codec "); uart_print_hex(h.vendor); uart_print(" has no usable output path (step ");
              uart_print_dec((uint64_t)-rc); uart_print(").\n"); return -7; }
    unsigned iss = (r16(GCAP) >> 8) & 15;
    h.sd = h.r + 0x80 + 0x20 * iss;
    h.up = 1;
    uart_print("[KIRA HDA] Sound: codec "); uart_print_hex(h.vendor >> 16); uart_print(":"); uart_print_hex(h.vendor & 0xFFFF);
    uart_print(", output path pin "); uart_print_dec(h.pin); uart_print(" <- DAC "); uart_print_dec(h.dac);
    uart_print(", 48 kHz 16-bit stereo (commands: sound, beep).\n");
    audio_start();
    return 0;
}


/* Вечный поток вывода: кольцо из 4 частей по 0,5 с, карта идёт по нему по кругу; что в нём - пишет микшер (audio.c). */
int hda_ring_start(void) {
    if (!h.up) return -1;
    kmutex_lock(&h.mx);
    for (uint32_t i = 0; i < BUFSZ; i++) h.buf[i] = 0;
    for (unsigned i = 0; i < NBDL; i++) {
        uint64_t a = h.buf_p + (uint64_t)i * (BUFSZ / NBDL);
        h.bdl[4 * i] = (uint32_t)a; h.bdl[4 * i + 1] = (uint32_t)(a >> 32); h.bdl[4 * i + 2] = BUFSZ / NBDL; h.bdl[4 * i + 3] = 0;
    }
    cpu_mb();
    volatile uint8_t* sd = h.sd;
    MMIO_W8(sd + SD_CTL, 1);                                     /* сброс потока */
    for (int i = 0; i < 100000 && !(MMIO_R8(sd + SD_CTL) & 1); i++) cpu_relax();
    MMIO_W8(sd + SD_CTL, 0);
    for (int i = 0; i < 100000 && (MMIO_R8(sd + SD_CTL) & 1); i++) cpu_relax();
    MMIO_W32(sd + SD_BDPL, (uint32_t)h.bdl_p); MMIO_W32(sd + SD_BDPU, (uint32_t)(h.bdl_p >> 32));
    MMIO_W32(sd + SD_CBL, BUFSZ);
    MMIO_W16(sd + SD_LVI, NBDL - 1);
    MMIO_W16(sd + SD_FMT, 0x0011);
    MMIO_W8(sd + SD_CTL + 2, 1u << 4);                           /* номер потока 1 */
    MMIO_W8(sd + SD_STS, 0x1C);
    MMIO_W8(sd + SD_CTL, 2);                                     /* пуск: дальше - по кругу */
    kmutex_unlock(&h.mx);
    return 0;
}
uint32_t hda_ring_pos(void) { return h.up ? MMIO_R32(h.sd + SD_LPIB) % BUFSZ : 0; }
int16_t* hda_ring_buf(void) { return (int16_t*)h.buf; }
uint32_t hda_ring_bytes(void) { return BUFSZ; }

/* Тон через микшер; dma_bytes - сколько байт карта забрала за это время. */
int hda_beep(unsigned hz, unsigned ms, uint32_t* dma_bytes) {
    if (!h.up) return -1;
    if (hz < 20 || hz > 20000 || !ms) return -2;
    if (ms > 2000) ms = 2000;
    uint64_t hw0 = audio_hw_frames();
    if (audio_tone(hz, ms) != 0) return -1;
    sched_sleep_ticks((uint64_t)ms * TIMER_HZ / 1000 + 1);
    if (dma_bytes) *dma_bytes = (uint32_t)((audio_hw_frames() - hw0) * 4);
    return 0;
}

void hdacmd_sound(const char* a) {
    (void)a;
    if (!h.up) { uart_print("звуковой карты нет (поддерживается Intel HDA: в Limbo - Sound: hda)\n"); return; }
    uart_print("Intel HDA: кодек "); uart_print_hex(h.vendor); uart_print(" (адрес "); uart_print_dec(h.cad);
    uart_print("), выход: вывод "); uart_print_dec(h.pin); uart_print(" <- ЦАП "); uart_print_dec(h.dac);
    uart_print(", 48 кГц, 16 бит, стерео\n");
    audio_print();
    uart_print("проверка: beep [Гц] [мс]\n");
}

void hdacmd_beep(const char* a) {
    unsigned hz = 440, ms = 500;
    while (a && *a == ' ') a++;
    if (a && *a >= '0' && *a <= '9') { hz = 0; while (*a >= '0' && *a <= '9') hz = hz * 10 + (unsigned)(*a++ - '0'); }
    while (a && *a == ' ') a++;
    if (a && *a >= '0' && *a <= '9') { ms = 0; while (*a >= '0' && *a <= '9') ms = ms * 10 + (unsigned)(*a++ - '0'); }
    uint32_t pos = 0;
    int r = hda_beep(hz, ms, &pos);
    if (r == -1) { uart_print("звуковой карты нет\n"); return; }
    if (r == -2) { uart_print("beep [Гц 20..20000] [мс]\n"); return; }
    uart_print("звук "); uart_print_dec(hz); uart_print(" Гц, "); uart_print_dec(ms > 2000 ? 2000 : ms);
    uart_print(" мс: DMA прошёл "); uart_print_dec(pos); uart_print(pos ? " байт - карта играет\n" : " байт - карта не забирает звук\n");
}
#endif
