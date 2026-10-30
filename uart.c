#include "uart.h"
#include "cpu.h"
#include "klog.h"
#include "earlycon.h"
#include "spinlock.h"
#include "linebuf.h"
#include "percpu.h"

#define PORT 0x3F8

static spinlock_t uart_lock = SPINLOCK_INIT;   // строка выводится целиком; порядок замков: uart -> klog, uart -> earlycon

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Порт есть не всегда: в Limbo, на ноутбуках и современных платах COM1 часто нет, тогда чтение даёт 0xFF -
   «данные готовы» навсегда, и командная строка читала бы байт 0xFF бесконечно. Порт проверяется петлёй
   (байт, посланный самому себе), без него вывод идёт только в журнал и на экран. */
static int present;
static unsigned fifo_depth = 1;   /* сколько байт передатчик примет, когда он свободен */

int uart_present(void) { return present; }

void uart_init(void) {
    present = 0;
    if (inb(PORT + 5) == 0xFF) return;                  // никто не отвечает на шине
    outb(PORT + 1, 0x00);    // Disable interrupts
    outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(PORT + 0, 0x01);    // Divisor 1 = 115200 baud
    outb(PORT + 1, 0x00);    // High byte
    outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(PORT + 2, 0xC7);    // Enable FIFO, clear with 14-byte threshold
    outb(PORT + 4, 0x1E);    // петля: передатчик замкнут на приёмник
    outb(PORT + 0, 0xAE);
    for (int i = 0; i < 100000 && !(inb(PORT + 5) & 0x01); i++) { }
    if (!(inb(PORT + 5) & 0x01) || inb(PORT) != 0xAE) { outb(PORT + 4, 0x00); return; }
    while (inb(PORT + 5) & 0x01) (void)inb(PORT);        // остатки FIFO
    outb(PORT + 4, 0x0B);    // обычный режим: RTS/DSR set
    fifo_depth = (inb(PORT + 2) & 0xC0) == 0xC0 ? 16 : 1;   // 16550A: FIFO передатчика 16 байт; 8250/16450 - один
    present = 1;
}

int uart_getchar(void) {
    if (!present) return -1;
    uint8_t lsr = inb(PORT + 5);
    if (lsr == 0xFF) { present = 0; return -1; }        // порт пропал (горячее отключение, переходник)
    if ((lsr & 0x01) == 0) return -1;
    return inb(PORT);
}

/* ---- Передача в порт без ожидания ----
   Медленный или никем не читаемый порт (115200 бод - 11 КБ/с; эмулятор, чей хозяин не забирает вывод) не должен
   держать ядро: раньше каждый байт ждал готовности передатчика с выключенными прерываниями, и длинная строка
   останавливала ядро на сотни миллисекунд. Теперь строки ложатся в кольцо, а в порт уходит столько, сколько он
   готов принять прямо сейчас (FIFO 16 байт), - при выводе и на каждом тике. При аварии кольцо выдаётся с ожиданием:
   система всё равно останавливается, и последние слова должны дойти. */
#define TX_RING (64 * 1024)
static char tx_ring[TX_RING];
static volatile uint32_t tx_head, tx_tail;          /* голова - под uart_lock, хвост - под tx_busy */
static volatile uint64_t tx_dropped;                /* байт, не поместившихся в кольцо (порт не успевает) */
static volatile int tx_busy;                         /* порт обслуживает какое-то ядро */
extern int smp_halting(void);

static int panic_mode(void) { return spin_bust || smp_halting(); }

static void tx_put(char c) {                        /* под uart_lock */
    uint32_t h = tx_head;
    if (h - tx_tail >= TX_RING) { tx_dropped++; return; }
    tx_ring[h % TX_RING] = c;
    __atomic_store_n(&tx_head, h + 1, __ATOMIC_RELEASE);
}

/* Отдать порту то, что он примет без ожидания. Пропускает ход, если порт уже обслуживает другое ядро. */
static void tx_pump(void) {
    if (!present || tx_tail == __atomic_load_n(&tx_head, __ATOMIC_ACQUIRE)) return;
    uint64_t f = irq_save();
    if (__atomic_exchange_n(&tx_busy, 1, __ATOMIC_ACQUIRE)) { irq_restore(f); return; }   /* занят: не ждать */
    for (int rounds = 0; rounds < 64; rounds++) {
        uint32_t t = tx_tail, h = __atomic_load_n(&tx_head, __ATOMIC_ACQUIRE);
        if (t == h || !(inb(PORT + 5) & 0x20)) break;                  /* пусто или передатчик ещё занят */
        unsigned n = h - t < fifo_depth ? h - t : fifo_depth;
        for (unsigned i = 0; i < n; i++) outb(PORT, (uint8_t)tx_ring[(t + i) % TX_RING]);
        __atomic_store_n(&tx_tail, t + n, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&tx_busy, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

/* Авария: выдать кольцо целиком, ожидая передатчик (не дольше ~мс на байт: зависший порт не держит вечно). */
static void tx_drain_sync(void) {
    if (!present) { tx_tail = tx_head; return; }
    while (tx_tail != tx_head) {
        int ok = 0;
        for (int i = 0; i < 200000; i++) if (inb(PORT + 5) & 0x20) { ok = 1; break; }
        if (!ok) { tx_tail = tx_head; return; }
        outb(PORT, (uint8_t)tx_ring[tx_tail % TX_RING]);
        tx_tail++;
    }
}

uint64_t uart_tx_dropped(void) { return tx_dropped; }

// Строка уходит в журнал, на раннюю консоль и в кольцо порта целиком под замком uart_lock.
static void emit_line(const char* s, unsigned n, void* ctx) {
    (void)ctx;
    uint64_t flags = spin_lock_irqsave(&uart_lock);
    for (unsigned i = 0; i < n; i++) {
        klog_putc(s[i]);        // копия в журнал: её выводит на экран поток дисплея
        earlycon_putc(s[i]);    // пока дисплея нет (или он не поднялся) - сразу в видеопамять
        tx_put(s[i]);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
    if (panic_mode()) tx_drain_sync(); else tx_pump();
}

// У каждого ядра два буфера: для потоков и для обработчиков прерываний (обработчик может прервать печать потока
// на середине строки, их тексты не должны склеиться).
static linebuf_t lines[PERCPU_MAX_CPUS][2];

static linebuf_t* my_line(void) {
    percpu_t* c = this_cpu();
    return &lines[c->cpu_id][c->irq_depth ? 1 : 0];
}

void uart_putchar(char c) {
    uint64_t f = irq_save();
    lb_putc(my_line(), c, emit_line, 0);
    irq_restore(f);
}

// Только в последовательный порт: служебный вывод, который не должен засорять экран.
void uart_print_serial(const char* str) {
    uint64_t flags = spin_lock_irqsave(&uart_lock);
    while (*str) tx_put(*str++);
    spin_unlock_irqrestore(&uart_lock, flags);
    if (panic_mode()) tx_drain_sync(); else tx_pump();
}

void uart_print(const char* str) {
    uint64_t f = irq_save();
    linebuf_t* b = my_line();
    while (*str) lb_putc(b, *str++, emit_line, 0);
    irq_restore(f);
}

// Раз в тик на каждом ядре: хвост строки без перевода строки не должен застрять в буфере.
void uart_tick_flush(void) {
    percpu_t* c = this_cpu();
    lb_tick(&lines[c->cpu_id][0], emit_line, 0);
    lb_tick(&lines[c->cpu_id][1], emit_line, 0);
    tx_pump();                                          // хвост кольца - в порт, сколько он примет
}

// Смена потока на этом ядре (вызывает планировщик, прерывания выключены): незаконченная строка уходившего потока
// выводится сразу, буфер потоков ядра достаётся новому потоку пустым.
void uart_switch_flush(void) {
    lb_flush(&lines[this_cpu()->cpu_id][0], emit_line, 0);
}

// Авария: выдать всё, что накопили все ядра (остальные уже остановлены).
void uart_flush_all(void) {
    for (unsigned i = 0; i < PERCPU_MAX_CPUS; i++) {
        lb_flush(&lines[i][0], emit_line, 0);
        lb_flush(&lines[i][1], emit_line, 0);
    }
    tx_drain_sync();
}

void uart_print_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];
    int pos = 18;
    buf[pos] = '\0';
    do {
        buf[--pos] = digits[value & 0xF];
        value >>= 4;
    } while (value != 0);
    buf[--pos] = 'x';
    buf[--pos] = '0';
    uart_print(&buf[pos]);
}

void uart_print_dec(uint64_t value) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    do {
        buf[--pos] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    uart_print(&buf[pos]);
}
