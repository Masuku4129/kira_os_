#include "tpmtis.h"
#if defined(__x86_64__)
#include "vmm.h"
#include "kmutex.h"
#include "cpu.h"

/* Регистры TIS 1.3 (интерфейс FIFO), базовый адрес локальности 0. */
#define TIS_BASE      0xFED40000ull
#define TIS_ACCESS    0x00u      /* 8 бит */
#define TIS_STS       0x18u      /* 32 бита: статус и burstCount (биты 8..23) */
#define TIS_FIFO      0x24u      /* 8 бит */
#define TIS_DID_VID   0xF00u
#define ACC_VALID     0x80u
#define ACC_ACTIVE    0x20u
#define ACC_REQUEST   0x02u
#define STS_VALID     0x80u
#define STS_READY     0x40u
#define STS_GO        0x20u
#define STS_DATA      0x10u
#define STS_EXPECT    0x08u

static volatile uint8_t* regs;
static int probed, present;
static kmutex_t tis_mx = KMUTEX_INIT;

static uint8_t  r8(uint32_t o) { return *(volatile uint8_t*)(regs + o); }
static uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(regs + o); }
static void     w8(uint32_t o, uint8_t v) { *(volatile uint8_t*)(regs + o) = v; }
static void     w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(regs + o) = v; }

/* Ждать условия не дольше ms миллисекунд. Время - по счётчику TSC, а не по тикам планировщика: монтирование тома (и сверка якоря) идёт ДО запуска
   таймера, а выключение - когда прерывания могут быть уже закрыты. Частота TSC неизвестна заранее; берётся нижняя оценка 1 ГГц, поэтому предел
   не короче заявленного на процессорах до 1 ГГц и на быстрых (3 ГГц) втрое короче заявленного - с запасом: TPM отвечает за миллисекунды. */
#define TSC_PER_MS 1000000ull
#define WAIT(cond, ms) ({ uint64_t dl_ = rdtsc() + (uint64_t)(ms) * TSC_PER_MS; int ok_ = 0; \
                          for (;;) { if (cond) { ok_ = 1; break; } if (rdtsc() > dl_) break; cpu_relax(); } ok_; })

int tpmtis_probe(void) {
    if (probed) return present ? 0 : -1;
    probed = 1;
    regs = (volatile uint8_t*)vmm_map_mmio(TIS_BASE, 0x1000, VMM_MMIO_UC);
    if (!regs) return -1;
    uint8_t acc = r8(TIS_ACCESS);
    uint32_t id = r32(TIS_DID_VID);
    /* Нет устройства - шина отдаёт все единицы; у настоящего TIS бит достоверности регистров установлен, а идентификатор не нулевой и не 0xFFFFFFFF. */
    if (acc == 0xFF || !(acc & ACC_VALID) || id == 0xFFFFFFFFu || id == 0) return -1;
    present = 1;
    return 0;
}

static uint32_t burst(void) { return (r32(TIS_STS) >> 8) & 0xFFFFu; }

/* Выполнить обмен, локальность 0 уже занята. 0 - успех. */
static int exchange(const uint8_t* cmd, size_t n, uint8_t* rsp, size_t* rl) {
    if (!(r32(TIS_STS) & STS_READY)) {
        w32(TIS_STS, STS_READY);
        if (!WAIT(r32(TIS_STS) & STS_READY, 2000)) return -1;
    }
    size_t sent = 0;
    while (sent < n) {
        uint32_t b = 0;
        if (!WAIT((b = burst()) != 0, 2000)) return -1;
        while (b-- && sent < n - 1) w8(TIS_FIFO, cmd[sent++]);
        if (sent < n - 1) continue;
        /* осталось ровно 1 байт: после него бит expect обязан сброситься */
        if (!WAIT(r32(TIS_STS) & STS_EXPECT, 2000)) return -1;
        w8(TIS_FIFO, cmd[sent++]);
        if (!WAIT(!(r32(TIS_STS) & STS_EXPECT), 2000)) return -1;
    }
    w32(TIS_STS, STS_GO);
    if (!WAIT((r32(TIS_STS) & (STS_VALID | STS_DATA)) == (STS_VALID | STS_DATA), 20000)) return -1;
    size_t got = 0, total = 10;
    while (got < total) {
        uint32_t b = 0;
        if (!WAIT((r32(TIS_STS) & (STS_VALID | STS_DATA)) == (STS_VALID | STS_DATA), 2000)) return -1;
        b = burst();
        if (!b) { cpu_relax(); continue; }
        while (b-- && got < total) {
            uint8_t v = r8(TIS_FIFO);
            if (got < *rl) rsp[got] = v;
            got++;
            if (got == 6) total = (size_t)rsp[2] << 24 | (size_t)rsp[3] << 16 | (size_t)rsp[4] << 8 | rsp[5];   /* общий размер - из заголовка ответа */
            if (got == 6 && (total < 10 || total > *rl)) return -1;
        }
    }
    *rl = got;
    return 0;
}

int tpmtis_submit(const uint8_t* cmd, size_t n, uint8_t* rsp, size_t* rl) {
    if (!cmd || !n || !rsp || !rl || *rl < 10 || tpmtis_probe() != 0) return -1;
    kmutex_lock(&tis_mx);
    int rc = -1;
    w8(TIS_ACCESS, ACC_REQUEST);                                   /* занять локальность 0 */
    if (WAIT(r8(TIS_ACCESS) & ACC_ACTIVE, 1000)) {
        rc = exchange(cmd, n, rsp, rl);
        w32(TIS_STS, STS_READY);                                   /* TPM в состояние покоя */
        w8(TIS_ACCESS, ACC_ACTIVE);                                /* отдать локальность */
    }
    kmutex_unlock(&tis_mx);
    return rc;
}
#else
int tpmtis_probe(void) { return -1; }
int tpmtis_submit(const uint8_t* cmd, size_t n, uint8_t* rsp, size_t* rl) { (void)cmd; (void)n; (void)rsp; (void)rl; return -1; }
#endif
