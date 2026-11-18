#ifndef KIRA_AML_H
#define KIRA_AML_H
/* Интерпретатор AML - языка таблиц ACPI (DSDT/SSDT). Уровень 1 (платформа): чистый код без зависимостей от ядра.
   Всё железо (память, порты, PCI, EC) он трогает только через узкую таблицу aml_ops_t; вся память - из одной арены,
   выданной при создании (kmalloc/malloc здесь не зовётся). Границы проверяются на каждом чтении; глубина вложенности,
   число шагов и время сна ограничены, поэтому битая или враждебная таблица может дать только ошибку, но не зависание и не выход за границы.
   Однопоточный: один вызов за раз (повторный вход возвращает AML_E_BUSY). */
#include <stdint.h>
#include <stddef.h>

enum {
    AML_OK = 0, AML_E_PARSE = -1, AML_E_NOTFOUND = -2, AML_E_NOMEM = -3, AML_E_DEPTH = -4, AML_E_STEPS = -5, AML_E_TYPE = -6,
    AML_E_UNSUP = -7, AML_E_BOUNDS = -8, AML_E_HW = -9, AML_E_FATAL = -10, AML_E_ARGS = -11, AML_E_TABLE = -12, AML_E_TIME = -13,
    AML_E_BUSY = -14
};

/* Пространства операционных областей (OperationRegion). */
enum { AML_SP_MEM = 0, AML_SP_IO = 1, AML_SP_PCI = 2, AML_SP_EC = 3, AML_SP_SMBUS = 4, AML_SP_CMOS = 5, AML_SP_PCIBAR = 6 };

typedef struct aml_ctx aml_ctx_t;
typedef struct aml_node aml_node_t;

typedef struct {
    /* Одно обращение к области. write: 0 - чтение в *val, 1 - запись *val. bits: 8/16/32/64. addr: адрес в пространстве
       (для PCI - смещение регистра). pci: seg<<16 | bus<<8 | dev<<3 | fn (только для AML_SP_PCI, иначе 0). Возврат 0 - успех. */
    int  (*rw)(void* user, int write, unsigned space, uint64_t addr, unsigned bits, uint64_t* val, uint32_t pci);
    void (*sleep_ms)(void* user, uint32_t ms);
    void (*stall_us)(void* user, uint32_t us);
    uint64_t (*timer_100ns)(void* user);          /* может быть 0 */
    void (*notify)(void* user, aml_node_t* n, uint32_t value);   /* Notify(объект, значение); может быть 0 */
    void (*log)(void* user, const char* s);       /* запись в Debug и Fatal; может быть 0 */
    void* user;
} aml_ops_t;

typedef enum { AML_V_NONE = 0, AML_V_INT, AML_V_STR, AML_V_BUF, AML_V_PKG, AML_V_REF } aml_vtype_t;
typedef struct aml_val aml_val_t;
struct aml_val {
    uint8_t  t;          /* aml_vtype_t */
    uint8_t  rk;         /* вид ссылки (внутри) */
    uint32_t len;        /* STR/BUF - байт (у строки без нуля в конце), PKG - число элементов */
    union { uint64_t i; uint8_t* b; aml_val_t* p; aml_node_t* n; } u;
    uint32_t idx;
};

typedef enum { AML_N_SCOPE = 1, AML_N_DEVICE, AML_N_PROC, AML_N_THERMAL, AML_N_POWERRES, AML_N_METHOD, AML_N_NAME, AML_N_ALIAS,
               AML_N_REGION, AML_N_FIELD, AML_N_BUFFIELD, AML_N_MUTEX, AML_N_EVENT } aml_ntype_t;

#define AML_DEF_MAX_STEPS   3000000u     /* шагов (термов) на один вызов */
#define AML_DEF_MAX_TDEPTH  128u         /* вложенность термов */
#define AML_DEF_MAX_MDEPTH  24u          /* вложенность вызовов методов */
#define AML_DEF_MAX_SLEEP   4000u        /* мс сна и ожиданий на один вызов */

/* Арена: mem/size - единственная память интерпретатора (выравнивание 16). Возврат 0 - арена слишком мала. */
aml_ctx_t* aml_create(void* mem, size_t size, const aml_ops_t* ops);
void aml_set_limits(aml_ctx_t* c, uint32_t max_steps, uint32_t max_tdepth, uint32_t max_mdepth, uint32_t max_sleep_ms);
/* Нижняя граница стека, на котором идёт разбор: ниже неё интерпретатор возвращает AML_E_DEPTH, не рискуя переполнением. 0 - без проверки. */
void aml_set_stack_floor(aml_ctx_t* c, const void* floor);
void aml_set_os(aml_ctx_t* c, const char* os_name, uint32_t rev);    /* значение \_OS_ и \_REV (строка должна жить дольше) */
/* Загрузить таблицу целиком (36-байтовый заголовок ACPI + AML): DSDT или SSDT. Байты должны оставаться на месте и не меняться. */
int  aml_load(aml_ctx_t* c, const uint8_t* table, uint32_t len);
void aml_load_done(aml_ctx_t* c);      /* после загрузки всех таблиц: дальше новые постоянные данные ограничены малым бюджетом */

aml_node_t* aml_lookup(aml_ctx_t* c, aml_node_t* scope, const char* path);   /* "\\_SB.PCI0", "_STA" от scope; scope 0 - корень */
aml_node_t* aml_root(aml_ctx_t* c);
aml_node_t* aml_first_child(const aml_node_t* n);
aml_node_t* aml_next_sibling(const aml_node_t* n);
aml_node_t* aml_parent(const aml_node_t* n);
aml_ntype_t aml_node_type(const aml_node_t* n);
void        aml_node_name(const aml_node_t* n, char out[5]);
int         aml_node_path(const aml_node_t* n, char* buf, unsigned cap);     /* "\\_SB_.PCI0", длина или <0 */
unsigned    aml_method_argc(const aml_node_t* n);

/* Вычислить объект: метод (с аргументами) или данные. Результат живёт до следующего вызова aml_call*. */
int aml_call(aml_ctx_t* c, aml_node_t* n, const aml_val_t* args, unsigned nargs, aml_val_t* out);
int aml_call_path(aml_ctx_t* c, aml_node_t* scope, const char* path, const aml_val_t* args, unsigned nargs, aml_val_t* out);
int aml_call_int(aml_ctx_t* c, aml_node_t* scope, const char* path, uint64_t* out);   /* метод/данные, ожидается число */

/* Помощники значений. */
aml_val_t aml_int(uint64_t v);
int aml_val_int(const aml_val_t* v, uint64_t* out);                 /* INT или короткий буфер -> число; ссылки не разворачивает */
const aml_val_t* aml_pkg_at(const aml_val_t* v, unsigned i);       /* элемент пакета или 0 */
int aml_val_node(const aml_val_t* v, aml_node_t** n);               /* ссылка на объект пространства (элемент пакета с именем) */
void aml_eisaid(uint32_t id, char out[8]);                          /* сжатый EISAID -> "PNP0C0A" */
int aml_hid_match(aml_ctx_t* c, aml_node_t* dev, const char* id);   /* _HID или любой из _CID равен id (строка или EISAID) */

typedef struct { uint32_t nodes, arena_used, arena_size, persist_free, steps_last, dup_names, load_errors, loaded_tables; } aml_stats_t;
void aml_stats(const aml_ctx_t* c, aml_stats_t* s);
/* Обход дерева в глубину; fn возвращает не 0 - остановить. */
int aml_walk(aml_ctx_t* c, int (*fn)(aml_node_t* n, int depth, void* u), void* u);
#endif
