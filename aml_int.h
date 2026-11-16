#ifndef KIRA_AML_INT_H
#define KIRA_AML_INT_H
/* Внутренние типы интерпретатора AML: общие для aml.c (арена, пространство имён, поля) и aml_exec.c (выполнение термов). */
#include "aml.h"

#define AML_MAX_ARGS   7
#define AML_MAX_LOCALS 8

/* Коды управления потоком (положительные): идут вверх по возвратам, как ошибки, но не ошибки. */
#define C_BREAK 1
#define C_CONT  2
#define C_RET   3

/* Виды ссылок (aml_val_t.rk для AML_V_REF). */
enum { RK_NONE = 0, RK_NODE, RK_SLOT, RK_BYTE };

typedef struct { const uint8_t *p, *end; } cur_t;                       /* курсор по байт-коду: чтение только внутри [p, end) */
typedef struct { const uint8_t* seg; uint8_t nseg, up, root; } nm_t;    /* разобранное имя: сегменты по 4 байта, число '^', признак '\' */

struct aml_node {
    char name[4]; uint8_t type; uint8_t flags; uint8_t pad[2];
    aml_node_t *parent, *child, *last, *next;
    union {
        struct { const uint8_t *body, *end; uint8_t argc, serial, native, pad; } m;             /* метод: тело в таблице; native - встроенный */
        struct { aml_val_t v; const uint8_t *lazy, *lend; uint8_t state; } d;                  /* Name: state 0 - значение готово, 1 - не вычислено, 2 - вычисляется, 3 - не удалось */
        aml_node_t* alias;
        struct { const uint8_t *off_pc, *len_pc, *end; uint64_t off, len; uint8_t space, ready, pci_ready; uint32_t pci; } r;   /* операционная область */
        struct { aml_node_t *region, *idx, *data, *bank; const uint8_t *bankpc, *bankend; uint64_t bankval; uint32_t bitoff, bitlen; uint8_t access, update, kind, bankready; } f;
        struct { uint8_t* data; uint32_t datalen, bitoff, bitlen; } bf;                         /* поле буфера */
    } u;
};
enum { FK_REGION = 0, FK_INDEX = 1, FK_BANK = 2 };

typedef struct frame {
    aml_node_t* scope;        /* текущая область имён (для метода - сам метод) */
    aml_node_t* method;       /* вызываемый метод, 0 - загрузка таблицы */
    aml_val_t*  args;
    aml_val_t*  loc;
    aml_node_t* dyn;          /* объекты, созданные внутри метода (живут до его конца) */
    aml_val_t   ret;
    uint8_t     loading;      /* 1 - загрузка таблицы: новые объекты постоянные */
    uint8_t     nargs;
} frame_t;

struct aml_ctx {
    uint8_t *base, *lo, *hi, *hi0, *end;       /* арена: постоянное растёт вверх от lo, временное - вниз от hi */
    size_t persist_cap;                        /* сколько постоянной памяти можно взять после загрузки */
    uint8_t* lo_loaded;
    int loaded, busy;
    aml_ops_t ops;
    aml_node_t* root;
    uint64_t intmask; uint8_t intbits;
    uint32_t max_steps, max_tdepth, max_mdepth, max_sleep;
    uint32_t steps, tdepth, mdepth, slept, fdepth;
    const char* os; uint32_t rev;
    uint32_t nodes, dups, load_errors, tables, steps_last;
};

/* aml.c */
void*       a_alloc(aml_ctx_t* c, size_t n, int persistent);
int         a_is_persistent(const aml_ctx_t* c, const void* p);
int         a_copy(aml_ctx_t* c, const aml_val_t* src, aml_val_t* dst, int persistent, int depth);
aml_node_t* a_child(const aml_node_t* par, const uint8_t* seg);
int         a_lookup(aml_ctx_t* c, frame_t* f, aml_node_t* scope, const nm_t* nm, int search, aml_node_t** out);
int         a_parse_name(cur_t* cu, nm_t* nm);
int         a_new_node(aml_ctx_t* c, frame_t* f, aml_node_t* parent, const uint8_t* seg, int type, aml_node_t** out);
int         a_node_read(aml_ctx_t* c, aml_node_t* n, aml_val_t* out);
int         a_node_write(aml_ctx_t* c, aml_node_t* n, const aml_val_t* v);
int         a_name_force(aml_ctx_t* c, aml_node_t* n);
int         a_to_int(aml_ctx_t* c, const aml_val_t* v, uint64_t* out);
int         a_deref(aml_ctx_t* c, aml_val_t* v);
int         a_bytes_of(const aml_val_t* v, const uint8_t** p, uint32_t* n);
int         a_call_node(aml_ctx_t* c, aml_node_t* n, const aml_val_t* args, unsigned argc, aml_val_t* out);
int         a_pkg_len(cur_t* cu, const uint8_t** pkg_end);
int         a_field_raw_len(cur_t* cu, uint32_t* bits);
int         a_native_call(aml_ctx_t* c, aml_node_t* n, const aml_val_t* args, unsigned argc, aml_val_t* out);

/* aml_exec.c */
int a_term(aml_ctx_t* c, frame_t* f, cur_t* cu, aml_val_t* out);
int a_exec_block(aml_ctx_t* c, frame_t* f, cur_t* cu);
int a_define_field(aml_ctx_t* c, frame_t* f, cur_t* cu, int kind, aml_node_t* region, aml_node_t* idx, aml_node_t* data, aml_node_t* bank, const uint8_t* bankpc, const uint8_t* bankend, uint8_t flags);

#ifdef AML_TRACE   /* отладка на хосте: где именно оборвался разбор */
#include <stdio.h>
#define AML_FAIL(code) (fprintf(stderr, "aml: ошибка %d в %s:%d\n", (code), __FILE__, __LINE__), (code))
#else
#define AML_FAIL(code) (code)
#endif
#define NEED(cu, n) do { if ((size_t)((cu)->end - (cu)->p) < (size_t)(n)) return AML_FAIL(AML_E_PARSE); } while (0)
#define TRY(x) do { int _r = (x); if (_r) return _r; } while (0)

#endif
