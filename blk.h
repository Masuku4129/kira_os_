#ifndef BLK_H
#define BLK_H
#include <stdint.h>

#define BLK_SECTOR 512

/* Блочное устройство: 512-байтовые секторы. Драйверы (virtio-blk, AHCI/SATA, NVMe, ATA PIO, USB-накопители)
   дают только этот интерфейс; файловые системы и хранилище опыта не знают, на чём лежат. */
typedef struct blk_dev {
    char name[48];
    uint64_t sectors;
    int (*read)(struct blk_dev*, uint64_t lba, uint32_t count, void* buf);
    int (*write)(struct blk_dev*, uint64_t lba, uint32_t count, const void* buf);
    int (*flush)(struct blk_dev*);
    void* priv;
} blk_dev_t;

/* Найденные при загрузке диски. У каждого свой мьютекс: потоки с разных ядер обращаются к нему по очереди,
   а разные диски работают параллельно. */
#define BLK_MAX 256
typedef struct {
    blk_dev_t* dev;              /* защищённый мьютексом вход - им и пользоваться */
    char id[12];                 /* короткое имя: vda, sata0, nvme0n1, ata0 */
    char model[41];
    int  kira;                   /* 1 - на диске данные Киры (KiraFS или хранилище опыта) */
    int  foreign;                /* 1 - таблица разделов или чужая файловая система: без явного согласия не трогаем */
} blk_info_t;

void blk_probe(void);                                   /* PCI: virtio-blk, NVMe, AHCI, затем IDE (ATA PIO) */
int  blk_count(void);
const blk_info_t* blk_info(int i);
blk_dev_t* blk_find(const char* id);
/* Системный диск: только тот, на котором уже есть данные Киры. На чужие диски (Windows, Linux, Android) система
   сама ничего не пишет - даже хранилище опыта; сделать диск системным можно только командой format. */
blk_dev_t* blk_primary(void);
int  blk_set_primary(blk_dev_t* d);                    /* после format: 0 - диск из списка */
void blk_print(void);                                  /* команда disk */
int  blk_flush_all(void);                              /* дописать кэши записи всех дисков (перед выключением); ошибок */

/* Для драйверов: добавить диск (id - короткое имя, model - описание). */
void blk_register(blk_dev_t* raw, const char* id, const char* model);

void blk_stats(uint64_t* requests, uint64_t* tsc_total);   /* операций с дисками и их суммарное время в тактах TSC */

/* Ожидание без таймера (диски опрашиваются ещё до запуска часов): примерно us микросекунд (порт 0x80). */
void blk_udelay(uint32_t us);

/* Ожидание ответа устройства со сроком. Первые ~200 мкс - короткое кручение (обычная команда NVMe/SATA
   завершается за десятки мкс), дальше процессор отдаётся другим потокам (если можно уснуть), а срок считается
   по часам TSC. До запуска часов (опрос дисков при загрузке) - по счёту задержек.
   blk_wait_begin(&w, мс); while (!готово) if (!blk_wait_step(&w)) -> срок вышел. */
struct irqwait;
typedef struct { uint64_t start, end; uint8_t real; struct irqwait* iw; uint32_t mark; } blk_wait_t;
/* С прерыванием устройства: звать после blk_wait_begin и ДО запуска команды - дальше ожидание спит до прерывания
   (не дольше тика за раз), а не уступает процессор, оставаясь готовым. */
void blk_wait_irq(blk_wait_t* w, struct irqwait* iw);
void blk_wait_begin(blk_wait_t* w, uint32_t ms);
int  blk_wait_step(blk_wait_t* w);

#endif
