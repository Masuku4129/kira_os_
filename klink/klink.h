/* klink: компоновщик x86-64 (ET_REL -> ET_EXEC) для Kira OS. Чистый C99, без malloc: вся память - арена, выданная вызывающим.
 * Один код для хоста (klink/main.c) и для Киры (программа Ring 3 может вызвать klink_link с буферами в памяти).
 * Что поддержано и что нет - в комментарии к тестам tests/klink_test.sh и в сообщениях об ошибках klink_link. */
#ifndef KLINK_H
#define KLINK_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;                /* имя для сообщений и для шаблонов файлов в скрипте */
    const unsigned char *data;       /* образ ET_REL целиком */
    size_t len;
} KLInput;

typedef struct {
    /* вход */
    const KLInput *objs;
    unsigned nobjs;
    const char *script;              /* текст скрипта компоновки (подмножество GNU ld), обязателен */
    size_t script_len;
    const char *entry;               /* -e: переопределяет ENTRY() скрипта; NULL - из скрипта */
    int gc_sections;                 /* --gc-sections */
    uint64_t page_size;              /* -z max-page-size (степень двойки), 0 = 4096 */
    unsigned char *arena;            /* рабочая память (не освобождается; объём - порядка 8x суммарного входа) */
    size_t arena_cap;
    /* результат */
    const unsigned char *out;        /* образ ET_EXEC внутри арены */
    size_t out_len;
    unsigned sections_in;            /* входных секций с SHF_ALLOC, подошедших под правила скрипта */
    unsigned sections_gc;            /* из них удалено --gc-sections */
    char err[512];                   /* сообщение об ошибке при коде -1 */
} KLink;

/* 0 - успех (k->out, k->out_len), -1 - ошибка (k->err). Не завершает процесс и не пишет в stdout/stderr. */
int klink_link(KLink *k);

#endif
