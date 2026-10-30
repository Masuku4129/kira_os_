#ifndef USBHID_H
#define USBHID_H
#include <stdint.h>
#include "input.h"

/* Чистая часть USB (без железа, проверяется на хосте):
   - разбор дескриптора конфигурации: интерфейсы HID и их прерывающие конечные точки IN;
   - клавиатура (загрузочный протокол, 8 байт): разница с прошлым отчётом -> сканкоды набора 1 (тот же декодер, что
     у PS/2: Shift, Caps, горячие клавиши работают одинаково) и автоповтор, которого у USB нет в самом устройстве;
   - мышь (загрузочный протокол) и указатели по дескриптору отчёта (планшет с абсолютными X/Y, мыши с колесом). */

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} usb_setup_t;

enum { USB_REQ_GET_STATUS = 0, USB_REQ_SET_ADDRESS = 5, USB_REQ_GET_DESCRIPTOR = 6, USB_REQ_SET_CONFIGURATION = 9,
       HID_REQ_SET_IDLE = 0x0A, HID_REQ_SET_PROTOCOL = 0x0B };
enum { USB_DT_DEVICE = 1, USB_DT_CONFIG = 2, USB_DT_INTERFACE = 4, USB_DT_ENDPOINT = 5, USB_DT_HID = 0x21, USB_DT_REPORT = 0x22 };
enum { HID_PROTO_NONE = 0, HID_PROTO_KEYBOARD = 1, HID_PROTO_MOUSE = 2 };

typedef struct {
    uint8_t  iface, subclass, protocol;      /* класс 3 (HID); subclass 1 - загрузочный */
    uint8_t  ep;                             /* адрес конечной точки IN (с битом 0x80) */
    uint16_t maxp;
    uint8_t  interval;                       /* bInterval как в дескрипторе */
    uint16_t report_len;                     /* длина дескриптора отчёта (из дескриптора HID) */
} usb_hid_if_t;

/* Все интерфейсы (альтернатива 0) с конечными точками - для хабов и накопителей. */
typedef struct {
    uint8_t  num, cls, sub, proto, nep;
    uint16_t report_len;
    struct { uint8_t addr, attr, interval; uint16_t maxp; } ep[4];
} usb_iface_t;
int usb_parse_ifaces(const uint8_t* buf, unsigned len, uint8_t* cfg, usb_iface_t* out, int max);

/* Возвращает число найденных HID-интерфейсов с конечной точкой IN (не больше max); *cfg - bConfigurationValue.
   Повреждённые длины не выводят за пределы буфера: разбор просто останавливается. */
int usb_parse_config(const uint8_t* buf, unsigned len, uint8_t* cfg, usb_hid_if_t* out, int max);

/* ---- клавиатура ---- */
typedef struct { uint8_t prev[8]; uint8_t rep_usage; uint32_t rep_at; } hid_kbd_t;
#define HID_REPEAT_DELAY_MS 500
#define HID_REPEAT_RATE_MS  33
void     hid_kbd_reset(hid_kbd_t* k);
/* Новый отчёт: байты набора 1 в out (сколько записано). Отчёт «ошибка переполнения» (01 01 01...) пропускается. */
unsigned hid_kbd_report(hid_kbd_t* k, const uint8_t* r, unsigned len, uint32_t now_ms, uint8_t* out, unsigned cap);
/* Автоповтор удерживаемой клавиши: вызывать часто (каждые несколько мс). */
unsigned hid_kbd_tick(hid_kbd_t* k, uint32_t now_ms, uint8_t* out, unsigned cap);
uint16_t hid_usage_to_set1(uint8_t usage);   /* 0 - нет; 0x100 | код - с префиксом E0 */

/* ---- указатель ---- */
typedef struct { uint16_t off, bits; int32_t lmin, lmax; uint8_t present, sign; } hid_field_t;
typedef struct {
    uint8_t report_id;          /* 0 - отчёты без номера */
    uint8_t abs;                /* X/Y абсолютные (планшет) */
    hid_field_t x, y, wheel;
    uint16_t btn_off; uint8_t btn_count;
} hid_pointer_t;
/* 0 - в дескрипторе отчёта есть X и Y (Generic Desktop) - это указатель. */
int hid_parse_pointer(const uint8_t* rd, unsigned len, hid_pointer_t* p);
/* Загрузочная мышь: buttons, dx, dy (int8). */
void hid_boot_mouse_layout(hid_pointer_t* p);
/* 1 - отчёт разобран в *ev. */
int hid_pointer_decode(const hid_pointer_t* p, const uint8_t* rep, unsigned len, mouse_event_t* ev);

#endif
