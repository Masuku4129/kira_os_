#ifndef USB_H
#define USB_H
#include <stdint.h>
#include "usbhid.h"

/* USB: общее ядро стека и интерфейс контроллеров.
   Контроллеры: UHCI (USB 1.1; его даёт QEMU/Limbo с -usb) и xHCI (USB 3.x, все современные ПК; в QEMU - qemu-xhci).
   Классы: HID (клавиатура, мышь, планшет), хабы USB 2.0 (в том числе внутренние хабы ноутбуков), накопители
   (флешки, карты, внешние диски: Bulk-Only + SCSI) - как блочные устройства с томами FAT в /usbN.
   Вся работа с контроллерами - в одном потоке ядра «usb»: опрос прерывающих точек (~4 мс), портов (100 мс),
   запросы других потоков (чтение и запись накопителей) выполняются в нём же через usb_call. Поэтому драйверам
   контроллеров не нужны замки, а прерывания контроллеров не нужны вовсе (одинаково на эмуляторах и железе). */

enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };
#define USB_ERR       (-1)
#define USB_ERR_STALL (-2)

typedef struct usb_hc usb_hc_t;
typedef struct usb_pipe usb_pipe_t;              /* конечная точка (прерывающая или массовая), устроена контроллером */

typedef struct usb_dev {
    usb_hc_t* hc;
    struct usb_dev* parent;                      /* хаб, к которому подключено (0 - корневой порт контроллера) */
    uint8_t   used, addr, speed, port;           /* port - номер порта у родителя (у корня - корневой порт), с 1 */
    uint8_t   root_port, depth;                  /* корневой порт ветки; число хабов между корнем и устройством */
    uint32_t  route;                             /* xHCI: строка маршрута (по 4 бита на уровень хабов) */
    uint8_t   tt_slot, tt_port;                  /* xHCI: переводчик транзакций для LS/FS за хабом HS */
    uint8_t   is_hub, hub_nports, hub_ttt, hub_mtt;
    uint16_t  ep0_maxp;
    uint16_t  vendor, product;
    uint8_t   dev_class, dev_proto;
    uint32_t  hc_slot;                           /* xHCI: номер слота */
} usb_dev_t;

/* Операции контроллера.
   control: данные в/из data (len = setup.wLength); число переданных байт или < 0.
   enumerate: новое устройство на порту (корневом, parent = 0, или хаба) после сброса порта: адрес, размер пакета EP0;
     возвращает устройство, готовое к usb_attach, или 0.
   intr_open/bulk_open: открыть конечную точку (адрес с битом 0x80 - IN). intr_poll: > 0 - пришёл отчёт, 0 - нет,
     < 0 - точка сломалась. bulk_xfer: передать len байт (направление - по адресу точки); число байт (IN может прийти
     меньше), USB_ERR_STALL - устройство остановило точку (после CLEAR_FEATURE вызвать bulk_reset), USB_ERR - сбой.
   ports: подключения/отключения корневых портов. release: освободить адрес/слот отключённого устройства. */
struct usb_hc {
    const char* name;
    void* priv;
    int  (*control)(usb_hc_t* hc, usb_dev_t* d, const usb_setup_t* s, void* data);
    usb_dev_t* (*enumerate)(usb_hc_t* hc, usb_dev_t* parent, uint8_t port, uint8_t speed);
    usb_pipe_t* (*intr_open)(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp, uint8_t interval);
    int  (*intr_poll)(usb_hc_t* hc, usb_pipe_t* p, uint8_t* buf, unsigned cap);
    usb_pipe_t* (*bulk_open)(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp);
    int  (*bulk_xfer)(usb_hc_t* hc, usb_pipe_t* p, void* buf, unsigned len);
    void (*bulk_reset)(usb_hc_t* hc, usb_pipe_t* p);
    void (*pipe_close)(usb_hc_t* hc, usb_pipe_t* p);
    void (*ports)(usb_hc_t* hc);
    void (*release)(usb_hc_t* hc, usb_dev_t* d);
};

#define USB_MAX_DEV 255     /* адресов на шине - 127; контроллеров несколько */
#define USB_MAX_HID 64
#define USB_MAX_HUB 64
#define USB_MAX_MSC 64

/* ---- для контроллеров ---- */
usb_dev_t* usb_dev_new(usb_hc_t* hc, usb_dev_t* parent, uint8_t port, uint8_t speed);   /* поля ветки заполнены */
int  usb_attach(usb_dev_t* d);                         /* дескрипторы, конфигурация, драйвер класса */
void usb_detach(usb_dev_t* d);                         /* вместе со всем, что за ним (если это хаб) */
void usb_detach_port(usb_hc_t* hc, uint8_t port);      /* корневой порт опустел */
usb_dev_t* usb_dev_on_port(usb_hc_t* hc, uint8_t port);   /* устройство на корневом порту */
void usb_dev_free(usb_dev_t* d);                       /* отказ при адресации: устройство не состоялось */

/* ---- для драйверов классов (usb.c, usbhub.c, usbmsc.c) ---- */
int  usb_ctl(usb_dev_t* d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void* data);
int  usb_clear_halt(usb_dev_t* d, usb_pipe_t* p, uint8_t ep);
int  hub_attach(usb_dev_t* d, const usb_iface_t* f);
void hub_poll(void);
void hub_detach(usb_dev_t* d);
void hub_forget(usb_dev_t* d);
int  hub_count(void);
int  msc_attach(usb_dev_t* d, const usb_iface_t* f);
void msc_detach(usb_dev_t* d);
void msc_poll(void);                                   /* отложенное отключение томов */
void msc_print(void (*out)(const char*));
int  msc_count(void);

/* Иммунитет ввода. Клавиатура, которая в первые секунды или в любой момент печатает быстрее человека (больше
   USB_HID_BURST нажатий за 200 мс), рефлекторно задерживается: её нажатия копятся, а не уходят в систему, и
   Разум получает сигнал (devhealth). Его решение: usb_hid_verdict(1) - отключить, (0) - отпустить и отдать накопленное.
   usb_hid_allow_all - команда «usb allow»: вернуть отключённые и больше не задерживать их. */
#define USB_HID_BURST 24
void usb_hid_verdict(int block);
void usb_hid_allow_all(void);

/* Шаг опроса HID: отчёты -> очередь ввода, автоповтор клавиатуры. now_ms - монотонные часы. */
void usb_hid_poll(uint32_t now_ms);
unsigned usb_hid_count(void);

/* Выполнить fn(arg) в потоке usb и дождаться (из него самого - сразу). Результат fn. */
int  usb_call(int (*fn)(void*), void* arg);

/* Общие помощники (в ядре; в хост-тесте - заглушки теста). */
void*    usb_dma_alloc(uint32_t bytes, uint64_t* phys, int below_4g);   /* обнулённые страницы, адрес = физический */
void     usb_sleep_ms(uint32_t ms);
uint32_t usb_now_ms(void);
void     usb_log(const char* s);
void     usb_log_dec(uint64_t v);
void     usb_log_hex(uint64_t v);

/* Ядро: найти контроллеры на PCI и запустить поток. */
void usb_init(void);
void usb_print(void);            /* команда usb */

/* Контроллеры (uhci.c, xhci.c): 0 - запущен, hc заполнен. */
int uhci_start(usb_hc_t* hc, const void* pci_dev);
int xhci_start(usb_hc_t* hc, const void* pci_dev);

#endif
