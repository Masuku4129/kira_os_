#include "keyboard.h"
#include "spinlock.h"
#include "pic.h"
#include "uart.h"
#include "console.h"
#include "input.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64
#define ST_OUT 0x01
#define ST_IN  0x02
#define ST_AUX 0x20

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static int wait_in_clear(void) {
    for (uint32_t i = 0; i < 200000; i++) if (!(inb(PS2_STATUS) & ST_IN)) return 0;
    return -1;
}
static int wait_out_full(void) {
    for (uint32_t i = 0; i < 200000; i++) if (inb(PS2_STATUS) & ST_OUT) return 0;
    return -1;
}
static int ctl_cmd(uint8_t c) { if (wait_in_clear()) return -1; outb(PS2_CMD, c); return 0; }
static int ctl_data(uint8_t d) { if (wait_in_clear()) return -1; outb(PS2_DATA, d); return 0; }
static void flush_output(void) { for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUT); i++) (void)inb(PS2_DATA); }

/* Общий обработчик IRQ1 (клавиатура) и IRQ12 (мышь): контроллер 8042 отдаёт оба потока
   через один порт данных, различает их бит AUX в регистре состояния. */
/* Чтение состояния и байта данных должно быть неделимым: если бы два ядра одновременно увидели «байт готов»,
   второе прочитало бы тот же байт ещё раз (контроллер отдаёт последний байт повторно) - двойное нажатие.
   Обработчик вызывается из IRQ1, IRQ12 и при переносе прерываний на другое ядро. */
static spinlock_t ps2_lock = SPINLOCK_INIT;

static void ps2_irq(void) {
    uint64_t f = spin_lock_irqsave(&ps2_lock);
    for (int n = 0; n < 32; n++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & ST_OUT)) break;
        uint8_t b = inb(PS2_DATA);
        if (st & ST_AUX) input_push_mouse_byte(b);
        else input_push_scancode(b);
    }
    spin_unlock_irqrestore(&ps2_lock, f);
    console_note_event();
    // EOI отправляет irq_dispatch
}

int keyboard_init(void) {
    input_init();
    if (irq_register_handler(1, keyboard_handler) != 0) {
        return -1;
    }
    pic_unmask_irq(1);
    uart_print("[KIRA KEYBOARD] PS/2 IRQ1 handler registered (decoded by the input layer).\n");
    return 0;
}

void keyboard_handler(void) { ps2_irq(); }

static int mouse_cmd(uint8_t c) {
    if (ctl_cmd(0xD4) || ctl_data(c)) return -1;      /* следующий байт - устройству AUX */
    if (wait_out_full()) return -2;
    return inb(PS2_DATA) == 0xFA ? 0 : -3;            /* ACK */
}

/* Вызывать ДО включения прерываний: ответы устройства читаются опросом. */
int mouse_init(void) {
    flush_output();
    if (ctl_cmd(0xA8)) goto fail;                      /* включить порт AUX */
    if (ctl_cmd(0x20) || wait_out_full()) goto fail;   /* прочитать байт конфигурации */
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                                       /* разрешить IRQ12 */
    cfg &= (uint8_t)~0x20;                             /* включить тактирование AUX */
    if (ctl_cmd(0x60) || ctl_data(cfg)) goto fail;
    if (mouse_cmd(0xF6)) goto fail;                    /* настройки по умолчанию */
    if (mouse_cmd(0xF4)) goto fail;                    /* включить передачу данных */
    flush_output();
    if (irq_register_handler(12, keyboard_handler) != 0) return -1;
    pic_unmask_irq(12);
    uart_print("[KIRA MOUSE] PS/2 mouse enabled (IRQ12, 3-byte packets).\n");
    return 0;
fail:
    uart_print("[KIRA MOUSE] No PS/2 mouse answered: pointer disabled, keyboard unaffected.\n");
    return -1;
}
