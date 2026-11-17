#ifndef KIRA_ACPIDEV_H
#define KIRA_ACPIDEV_H
/* ACPI в работе: поверх интерпретатора AML (aml.h) - события SCI/GPE, встроенный контроллер (EC), кнопки, батарея, сеть питания,
   крышка, температура и таблица прерываний PCI (_PRT). Уровень 1 (платформа): чистая логика; всё железо (порты, память, PCI, сон)
   приходит через aml_ops_t, поэтому весь модуль проверяется на хосте на настоящих таблицах и модели железа (tests/acpidev_test.c).

   Где живёт AML (решение по слоям, подробности - docs/architecture/TECH_DECISIONS.md): интерпретатор - платформенный код ядра, а не служба
   Ring 3. Причина: ему нужны порты любых адресов (PM1, GPE, EC, SMI), конфигурация любого устройства PCI и память физических адресов
   таблиц, а у драйверов Ring 3 есть только BAR своего устройства (KDRV_SYS_IO/MMIO_MAP); дать службе такие права значило бы
   добавить в ядро самые широкие системные вызовы, и граница стала бы слабее, а не крепче. Вместо этого сам интерпретатор ограничен:
   своя арена, проверка границ, предел глубины, шагов и сна; он выполняется на отдельном стеке и не вызывает ничего, кроме узкого aml_ops_t. */
#include "aml.h"
#include <stddef.h>

typedef struct { const uint8_t* p; uint32_t len; } acpidev_tab_t;
typedef struct {               /* сведения из FADT, нужные для событий */
    uint16_t sci_int;
    uint32_t pm1a_evt, pm1b_evt; uint8_t pm1_evt_len;
    uint32_t gpe0_blk, gpe1_blk; uint8_t gpe0_len, gpe1_len, gpe1_base;
    uint32_t fadt_flags;
} acpidev_hw_t;

/* Размер арены: таблицы копируются внутрь неё (читать их можно только через окно без кэша, оно медленное), остальное - память интерпретатора. */
#define ACPIDEV_ARENA_BASE (3u << 19)       /* 1,5 МБ: пространство имён и временные значения самых больших таблиц ноутбуков */
size_t acpidev_arena_size(const acpidev_tab_t* dsdt, const acpidev_tab_t* ssdt, unsigned nssdt);

/* Загрузить таблицы, выполнить _INI, найти устройства (EC, батареи, сеть питания, крышки, термозоны, корневые мосты PCI).
   0 - успех; иначе код aml (таблицы не загружены или DSDT не принята). ops.rw получает чтение/запись портов, памяти и PCI; EC модуль берёт на себя. */
int  acpidev_init(void* mem, size_t size, const aml_ops_t* ops, const acpidev_hw_t* hw, const acpidev_tab_t* dsdt, const acpidev_tab_t* ssdt, unsigned nssdt);
int  acpidev_ready(void);
/* Все вызовы AML выполняются через этот запуск (ядро подставляет переход на отдельный стек); по умолчанию - прямой вызов.
   stack_floor - нижняя граница того стека (с запасом на мелкие функции): ниже интерпретатор отказывается углубляться (0 - без проверки). Ставить до acpidev_init. */
void acpidev_set_runner(void (*run)(void (*fn)(void*), void* arg), const void* stack_floor);

/* Режим контроллера прерываний для прошивки: \_PIC(0) - PIC, 1 - APIC (после настройки IOAPIC). */
int  acpidev_pic_mode(int apic);
/* Разрешить события: сначала \_SB._REG, потом - фиксированная кнопка питания и GPE, у которых есть методы _Lxx/_Exx (и GPE встроенного контроллера). */
int  acpidev_enable_events(void);
/* \_PTS(state) и \_GTS(state) перед сном/выключением (state 5 - выключение). 0 - выполнены или методов нет. */
int  acpidev_prepare_sleep(int state);
/* \_S5: значения SLP_TYP, вычисленные интерпретатором (0 - получены). */
int  acpidev_s5(uint8_t* a, uint8_t* b);
/* \_Sx (x = 0..5): значения SLP_TYPa/b пакета, вычисленные интерпретатором (0 - получены); для сна S3 - state 3. */
int  acpidev_sx(int state, uint8_t* a, uint8_t* b);
/* После пробуждения: \_BFS(state), затем \_WAK(state); состояние питания обновится при следующем опросе. */
int  acpidev_wake(int state);
/* Перед сном: \_PRW устройств-будильников (крышка, кнопки, встроенный контроллер) - их GPE очищаются и включаются; *count - сколько включено.
   0 - готово; источники, которым нужна более глубокая спячка, чем state, не включаются. */
int  acpidev_arm_wake(int state, unsigned* count);

/* События для ядра. */
enum { ACPIDEV_EV_POWER_BUTTON = 1, ACPIDEV_EV_SLEEP_BUTTON = 2, ACPIDEV_EV_LID = 3, ACPIDEV_EV_POWER = 4 };
void acpidev_set_hook(void (*fn)(int ev, int arg, void* user), void* user);   /* arg: для LID - 1 открыта, 0 закрыта */
void acpidev_sci(void);               /* из обработчика прерывания: только отметка, никакого AML */
int  acpidev_sci_take(void);          /* 1 - прерывание SCI было с прошлого вызова (ядро после обработки снимает маску линии) */
/* Основной цикл ядра: разбор событий и периодическое обновление питания. now_ms - монотонные миллисекунды. */
void acpidev_poll(uint64_t now_ms);

typedef struct {
    int      valid;               /* данные получены хотя бы раз */
    int      batteries;           /* число установленных батарей; 0 - показывать нечего */
    int      percent;             /* заряд 0..100 или -1 (неизвестен) */
    int      charging, discharging, critical;
    int      ac;                  /* 1 сеть есть, 0 нет, -1 адаптера в таблицах нет */
    int      lid;                 /* 1 открыта, 0 закрыта, -1 крышки нет */
    int      temp_valid, temp_c;  /* самая горячая термозона, °C */
    int      minutes_left;        /* оценка при разрядке, -1 - неизвестно */
    uint32_t remaining, last_full, rate;   /* единицы батареи (мВт·ч/мА·ч и мВт/мА) */
    uint32_t version;             /* растёт при каждом изменении */
} acpidev_power_t;
const acpidev_power_t* acpidev_power(void);
void acpidev_power_refresh(void);     /* прочитать батареи/сеть/крышку/температуру сейчас */

typedef struct { int found; uint32_t gsi; int level; int active_low; int via_link; char link[8]; } acpidev_irq_t;
/* Линия прерывания PCI-устройства по _PRT корневого моста шины bus: pin 1..4 = INTA..INTD. 1 - найдена. Шины за мостами PCI-PCI пока не разбираются. */
int  acpidev_pci_irq(unsigned bus, unsigned dev, unsigned fn, unsigned pin, acpidev_irq_t* out);

typedef struct { uint32_t pm1_events, gpe_events, ec_queries, notifies, gpe_unhandled, ec_errors, ini_run, ini_fail, reg_run; } acpidev_stats_t;
void acpidev_stats(acpidev_stats_t* s);
/* Печать: состояние, устройства, питание, таблица _PRT; eval - значение объекта по пути. sink получает по строке без перевода строки в конце. */
void acpidev_dump(void (*sink)(const char*));
int  acpidev_eval(const char* path, void (*sink)(const char*));
aml_ctx_t* acpidev_ctx(void);

#endif
