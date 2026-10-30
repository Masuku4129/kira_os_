#ifndef INPUT_H
#define INPUT_H
#include <stdint.h>

/* Единый слой ввода. Драйверы (PS/2 сейчас, USB/virtio-input позже) кладут
   события сюда, потребители (консоль, GUI, игры) читают события, а не сканкоды. */
enum { INPUT_KEY_DOWN = 1, INPUT_KEY_UP = 2 };

enum {
    KEY_NONE = 0,
    KEY_ESC = 0x100, KEY_BACKSPACE, KEY_TAB, KEY_ENTER,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT, KEY_CAPS,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_F11, KEY_F12,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE
};
/* Печатные клавиши имеют код = их ASCII без учёта Shift (строчные буквы, цифры, знаки). */

typedef struct {
    uint16_t type;     /* INPUT_KEY_DOWN / INPUT_KEY_UP */
    uint16_t key;      /* логический код клавиши */
    uint16_t ch;       /* ASCII с учётом Shift/Caps, 0 если символа нет */
    uint16_t mods;     /* INPUT_MOD_* */
} input_event_t;

#define INPUT_MOD_SHIFT 1
#define INPUT_MOD_CTRL  2
#define INPUT_MOD_ALT   4
#define INPUT_MOD_CAPS  8

/* Конечный автомат декодера сканкодов набора 1 (включая префикс E0). Чистая логика. */
typedef struct { uint8_t e0; uint16_t mods; uint8_t shift_l, shift_r, ctrl, alt; } ps2_decoder_t;
void ps2_decoder_reset(ps2_decoder_t* d);
/* Возвращает 1, если байт завершил событие и оно записано в *ev. */
int ps2_decoder_feed(ps2_decoder_t* d, uint8_t byte, input_event_t* ev);

/* Мышь PS/2: пакеты по 3 байта. dy > 0 - вниз (экранные координаты). */
/* buttons: бит0 левая, бит1 правая, бит2 средняя. abs = 1 - абсолютное положение (USB-планшет, сенсорный экран
   в эмуляторе): ax, ay в 0..32767 на весь экран, dx/dy не используются. */
typedef struct { int16_t dx, dy; uint8_t buttons; uint8_t abs; uint16_t ax, ay; } mouse_event_t;
typedef struct { uint8_t idx; uint8_t b[3]; } ps2_mouse_decoder_t;
void ps2_mouse_reset(ps2_mouse_decoder_t* d);
int  ps2_mouse_feed(ps2_mouse_decoder_t* d, uint8_t byte, mouse_event_t* ev);   /* 1 - пакет собран */
void input_push_mouse_byte(uint8_t byte);       /* из IRQ */
int  input_pop_mouse(mouse_event_t* ev);
/* Источники кроме PS/2 (USB): у каждого свой декодер (префиксы E0 и Shift разных клавиатур не смешиваются).
   Можно звать из потока: очередь защищена замком. */
enum { INPUT_SRC_PS2 = 0, INPUT_SRC_USB = 1, INPUT_SRC_N = 2 };
void input_push_scancode_from(int src, uint8_t byte);
void input_push_mouse(const mouse_event_t* ev);

/* Кольцевая очередь событий: один писатель (IRQ), один читатель. Без блокировок. */
void input_init(void);
void input_push_scancode(uint8_t byte);        /* из IRQ */
unsigned input_raw_take(uint8_t* out, unsigned max);   /* сырые байты клавиатуры с прошлого вызова (диагностика) */
int  input_pop(input_event_t* ev);             /* 1 - событие получено */
uint32_t input_dropped(void);
/* Горячие клавиши оболочки (F10, F11) не попадают в очередь событий: их забирает поток дисплея,
   поэтому они работают, даже если приложение не читает клавиатуру. Возвращает KEY_F10/KEY_F11 или 0. */
int input_take_hotkey(void);                  /* сколько событий потеряно при переполнении */

#endif
