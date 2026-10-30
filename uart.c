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
    present = 1;
}

int uart_getchar(void) {
    if (!present) return -1;
    uint8_t lsr = inb(PORT + 5);
    if (lsr == 0xFF) { present = 0; return -1; }        // порт пропал (горячее отключение, переходник)
    if ((lsr & 0x01) == 0) return -1;
    return inb(PORT);
}

static void serial_out(char c) {
    if (!present) return;
    for (int i = 0; i < 200000; i++)                   // ~ мс: занятый или зависший порт не держит ядро
        if (inb(PORT + 5) & 0x20) { outb(PORT, c); return; }
}

// Строка уходит в журнал, на раннюю консоль и в последовательный порт целиком под замком uart_lock.
static void emit_line(const char* s, unsigned n, void* ctx) {
    (void)ctx;
    uint64_t flags = spin_lock_irqsave(&uart_lock);
    for (unsigned i = 0; i < n; i++) {
        klog_putc(s[i]);        // копия в журнал: её выводит на экран поток дисплея
        earlycon_putc(s[i]);    // пока дисплея нет (или он не поднялся) - сразу в видеопамять
        serial_out(s[i]);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
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
    while (*str) serial_out(*str++);
    spin_unlock_irqrestore(&uart_lock, flags);
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
