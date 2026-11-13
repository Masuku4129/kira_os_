#include "spinlock.h"
#include "input.h"

#define RING 128 /* степень двойки */

static ps2_decoder_t decs[INPUT_SRC_N];
#define dec decs[INPUT_SRC_PS2]
static spinlock_t prod_lock = SPINLOCK_INIT;   /* производителей несколько: прерывание PS/2 и поток USB */
static input_event_t ring[RING];
static volatile uint32_t head, tail; /* head пишет IRQ, tail читает поток */
static volatile uint32_t dropped;

static const uint8_t map_plain[0x3A] = {
    0,0,'1','2','3','4','5','6','7','8','9','0','-','=',0,0,
    'q','w','e','r','t','y','u','i','o','p','[',']',0,0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' '
};
static const uint8_t map_shift[0x3A] = {
    0,0,'!','@','#','$','%','^','&','*','(',')','_','+',0,0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}',0,0,'A','S',
    'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
    'B','N','M','<','>','?',0,'*',0,' '
};

void ps2_decoder_reset(ps2_decoder_t* d) {
    d->e0 = 0; d->mods = 0; d->shift_l = d->shift_r = d->ctrl = d->alt = 0;
}

static void recompute(ps2_decoder_t* d) {
    d->mods = (d->mods & INPUT_MOD_CAPS)
            | ((d->shift_l || d->shift_r) ? INPUT_MOD_SHIFT : 0)
            | (d->ctrl ? INPUT_MOD_CTRL : 0)
            | (d->alt ? INPUT_MOD_ALT : 0);
}

int ps2_decoder_feed(ps2_decoder_t* d, uint8_t b, input_event_t* ev) {
    if (b == 0xE0) { d->e0 = 1; return 0; }
    if (b == 0xE1 || b == 0xFA || b == 0xFE || b == 0x00 || b == 0xFF) { d->e0 = 0; return 0; } /* Pause/ACK/resend/ошибки */
    int up = (b & 0x80) != 0;
    uint8_t sc = b & 0x7F;
    int ext = d->e0;
    d->e0 = 0;

    uint16_t key = KEY_NONE;
    if (!ext) {
        if (sc < 0x3A && map_plain[sc]) key = map_plain[sc];
        else switch (sc) {
            case 0x01: key = KEY_ESC; break;
            case 0x0E: key = KEY_BACKSPACE; break;
            case 0x0F: key = KEY_TAB; break;
            case 0x1C: key = KEY_ENTER; break;
            case 0x1D: key = KEY_LCTRL; break;
            case 0x2A: key = KEY_LSHIFT; break;
            case 0x36: key = KEY_RSHIFT; break;
            case 0x38: key = KEY_LALT; break;
            case 0x3A: key = KEY_CAPS; break;
            case 0x57: key = KEY_F11; break;
            case 0x58: key = KEY_F12; break;
            default:
                if (sc >= 0x3B && sc <= 0x44) key = KEY_F1 + (sc - 0x3B);
                break;
        }
    } else {
        switch (sc) {
            case 0x1C: key = KEY_ENTER; break;
            case 0x1D: key = KEY_RCTRL; break;
            case 0x38: key = KEY_RALT; break;
            case 0x47: key = KEY_HOME; break;
            case 0x48: key = KEY_UP; break;
            case 0x49: key = KEY_PGUP; break;
            case 0x4B: key = KEY_LEFT; break;
            case 0x4D: key = KEY_RIGHT; break;
            case 0x4F: key = KEY_END; break;
            case 0x50: key = KEY_DOWN; break;
            case 0x51: key = KEY_PGDN; break;
            case 0x52: key = KEY_INSERT; break;
            case 0x53: key = KEY_DELETE; break;
            default: break;
        }
    }
    if (key == KEY_NONE) return 0;

    switch (key) {
        case KEY_LSHIFT: d->shift_l = !up; break;
        case KEY_RSHIFT: d->shift_r = !up; break;
        case KEY_LCTRL: case KEY_RCTRL: d->ctrl = !up; break;
        case KEY_LALT: case KEY_RALT: d->alt = !up; break;
        case KEY_CAPS: if (!up) d->mods ^= INPUT_MOD_CAPS; break;
        default: break;
    }
    recompute(d);

    uint16_t ch = 0;
    if (!ext && sc < 0x3A && map_plain[sc] && !up) {
        int shifted = (d->mods & INPUT_MOD_SHIFT) != 0;
        uint8_t base = map_plain[sc];
        if (base >= 'a' && base <= 'z' && (d->mods & INPUT_MOD_CAPS)) shifted = !shifted;
        ch = shifted ? map_shift[sc] : base;
        if ((d->mods & INPUT_MOD_CTRL) && base >= 'a' && base <= 'z') ch = base - 'a' + 1;
    } else if (key == KEY_ENTER && !up) ch = '\n';
    else if (key == KEY_BACKSPACE && !up) ch = '\b';
    else if (key == KEY_TAB && !up) ch = '\t';

    ev->type = up ? INPUT_KEY_UP : INPUT_KEY_DOWN;
    ev->key = key; ev->ch = ch; ev->mods = d->mods;
    return 1;
}

void ps2_mouse_reset(ps2_mouse_decoder_t* d) { d->idx = 0; }

int ps2_mouse_feed(ps2_mouse_decoder_t* d, uint8_t byte, mouse_event_t* ev) {
    if (d->idx == 0 && !(byte & 8)) return 0;       /* бит 3 первого байта всегда 1: иначе потеряна синхронизация */
    d->b[d->idx++] = byte;
    if (d->idx < 3) return 0;
    d->idx = 0;
    uint8_t b0 = d->b[0];
    if (b0 & 0xC0) return 0;                        /* переполнение счётчиков: пакет недостоверен */
    int dx = (int)d->b[1] - (int)((b0 << 4) & 0x100);
    int dy = (int)d->b[2] - (int)((b0 << 3) & 0x100);
    ev->dx = (int16_t)dx;
    ev->dy = (int16_t)-dy;
    ev->buttons = b0 & 7; ev->abs = 0; ev->ax = ev->ay = 0;
    return 1;
}

#define MRING 64
static ps2_mouse_decoder_t mdec;
static mouse_event_t mring[MRING];
static volatile uint32_t mhead, mtail;

static void mpush(const mouse_event_t* ev) {
    uint32_t h = mhead;
    if (h - mtail >= MRING) return;                 /* очередь полна: потребитель отстал, старое важнее */
    mring[h & (MRING - 1)] = *ev;
    __atomic_store_n(&mhead, h + 1, __ATOMIC_RELEASE);
}

void input_push_mouse_byte(uint8_t byte) {
    mouse_event_t ev;
    uint64_t f = spin_lock_irqsave(&prod_lock);
    if (ps2_mouse_feed(&mdec, byte, &ev)) mpush(&ev);
    spin_unlock_irqrestore(&prod_lock, f);
}

void input_push_mouse(const mouse_event_t* ev) {
    uint64_t f = spin_lock_irqsave(&prod_lock);
    mpush(ev);
    spin_unlock_irqrestore(&prod_lock, f);
}

static spinlock_t min_lock = SPINLOCK_INIT;

int input_pop_mouse(mouse_event_t* ev) {
    uint64_t f = spin_lock_irqsave(&min_lock);
    uint32_t t = mtail;
    if (t == __atomic_load_n(&mhead, __ATOMIC_ACQUIRE)) { spin_unlock_irqrestore(&min_lock, f); return 0; }
    *ev = mring[t & (MRING - 1)];
    __atomic_store_n(&mtail, t + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&min_lock, f);
    return 1;
}

void input_init(void) {
    for (int i = 0; i < INPUT_SRC_N; i++) ps2_decoder_reset(&decs[i]);
    ps2_mouse_reset(&mdec);
    head = tail = dropped = 0;
    mhead = mtail = 0;
    __atomic_store_n(&hotkey_pending, 0, __ATOMIC_RELEASE);
}

static volatile int hotkey_pending;

int input_take_hotkey(void) { return __atomic_exchange_n(&hotkey_pending, 0, __ATOMIC_ACQ_REL); }

/* Сырые байты клавиатуры для диагностики (test keys): последние RAW_N байт, производитель - прерывание. */
#define RAW_N 64
static uint8_t raw[RAW_N];
static volatile uint32_t raw_head, raw_seen;

unsigned input_raw_take(uint8_t* out, unsigned max) {
    uint32_t h = __atomic_load_n(&raw_head, __ATOMIC_ACQUIRE);
    uint32_t from = raw_seen;
    if (h - from > RAW_N) from = h - RAW_N;
    unsigned n = 0;
    while (from != h && n < max) out[n++] = raw[from++ % RAW_N];
    raw_seen = h;
    return n;
}

uint16_t input_cyr(uint16_t key, uint16_t ch, uint16_t mods) {
    static const char lat[] = "qwertyuiop[]asdfghjkl;'zxcvbnm,.`/";
    static const uint16_t cyr[] = { 0x439, 0x446, 0x443, 0x43A, 0x435, 0x43D, 0x433, 0x448, 0x449, 0x437, 0x445, 0x44A,
                                    0x444, 0x44B, 0x432, 0x430, 0x43F, 0x440, 0x43E, 0x43B, 0x434, 0x436, 0x44D,
                                    0x44F, 0x447, 0x441, 0x43C, 0x438, 0x442, 0x44C, 0x431, 0x44E, 0x451, '.' };
    if (!ch || (mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT))) return ch;
    for (unsigned i = 0; lat[i]; i++) {
        if (key != (uint8_t)lat[i]) continue;
        int upper = !!(mods & INPUT_MOD_SHIFT) ^ !!(mods & INPUT_MOD_CAPS);
        uint16_t c = cyr[i];
        if (c == '.') return upper ? ',' : '.';
        if (!upper) return c;
        return c == 0x451 ? 0x401 : (uint16_t)(c - 0x20);
    }
    return ch;
}

static volatile int layout_ru;
int input_layout(void) { return layout_ru; }

void input_push_scancode_from(int src, uint8_t byte) {
    if (src < 0 || src >= INPUT_SRC_N) return;
    uint64_t f = spin_lock_irqsave(&prod_lock);
    if (src == INPUT_SRC_PS2) {
        raw[raw_head % RAW_N] = byte;
        __atomic_store_n(&raw_head, raw_head + 1, __ATOMIC_RELEASE);
    }
    input_event_t ev;
    if (ps2_decoder_feed(&decs[src], byte, &ev)) {
        if (ev.type == INPUT_KEY_DOWN &&                     /* Alt+Shift - другая раскладка */
            (((ev.key == KEY_LSHIFT || ev.key == KEY_RSHIFT) && (ev.mods & INPUT_MOD_ALT)) ||
             ((ev.key == KEY_LALT || ev.key == KEY_RALT) && (ev.mods & INPUT_MOD_SHIFT))))
            layout_ru = !layout_ru;
        if (layout_ru) ev.ch = input_cyr(ev.key, ev.ch, ev.mods);
        if (ev.key == KEY_F10 || ev.key == KEY_F11 ||
            ((ev.mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT)) == (INPUT_MOD_CTRL | INPUT_MOD_ALT) &&
             (ev.key == KEY_LEFT || ev.key == KEY_RIGHT))) {
            if (ev.type == INPUT_KEY_DOWN) {
                int hk = ev.key;
                if (ev.key == KEY_LEFT) hk = KEY_WORKSPACE_PREV;
                else if (ev.key == KEY_RIGHT) hk = KEY_WORKSPACE_NEXT;
                __atomic_store_n(&hotkey_pending, hk, __ATOMIC_RELEASE);
            }
        } else {
            uint32_t h = head;
            if (h - tail >= RING) dropped++;
            else { ring[h & (RING - 1)] = ev; __atomic_store_n(&head, h + 1, __ATOMIC_RELEASE); }
        }
    }
    spin_unlock_irqrestore(&prod_lock, f);
}

void input_push_scancode(uint8_t byte) { input_push_scancode_from(INPUT_SRC_PS2, byte); }

/* Производители - под prod_lock, потребителей два: поток дисплея и системный вызов окна в фокусе, они могут
   работать на разных ядрах. Замок делает чтение хвоста и его продвижение неделимыми. */
static spinlock_t in_lock = SPINLOCK_INIT;

int input_pop(input_event_t* ev) {
    uint64_t f = spin_lock_irqsave(&in_lock);
    uint32_t t = tail;
    if (t == __atomic_load_n(&head, __ATOMIC_ACQUIRE)) { spin_unlock_irqrestore(&in_lock, f); return 0; }
    *ev = ring[t & (RING - 1)];
    __atomic_store_n(&tail, t + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&in_lock, f);
    return 1;
}

uint32_t input_dropped(void) { return dropped; }
