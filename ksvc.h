#ifndef KIRA_KSVC_H
#define KIRA_KSVC_H
/* Каркас служб ядра на портах: общий порт (открывается из любого домена), сообщения CBOR v1 (userland/kframe.h), привязка ответного порта.
   Отправителя определяет ядро по номеру, который проставляет port_recv; имя, права и домен берутся из таблицы процессов, а не из сообщения.
   Клиент начинает с {"op":"hello"} и вложенным личным портом - туда идут ответы (одна привязка на процесс, до KSVC_BINDS клиентов).
   Дальше запросы; служба получает их через ksvc_next и отвечает ksvc_reply_*. Разбор hello, ошибок формата и мёртвых отправителей - здесь. */
#include <stdint.h>
#include "kobj.h"
#include "userland/kframe.h"

#define KSVC_BINDS 16
#define KSVC_E_FRAME (-200)        /* ошибка разбора сообщения: -200 - KF_E_* (ответ с id 0) */
#define KSVC_E_BUSY  (-103)        /* привязки кончились */
#define KSVC_E_ARG   (-101)

typedef struct {
    kobj_t* port;
    struct { uint32_t pid; kobj_t* rep; } bind[KSVC_BINDS];
    uint8_t* mb;                   /* принятое сообщение */
    uint8_t* rb;                   /* ответ */
    unsigned tick;
} ksvc_t;

typedef struct {
    uint32_t pid;
    char prog[16];
    uint32_t caps, dom;
    kframe_t f;                    /* op, id, args (внутри ksvc_t.mb: действительны до следующего ksvc_next) */
    kobj_t* att;                   /* вложение (ссылка у запроса: отпускает ksvc_done) или 0 */
    kobj_t* rep;                   /* ответный порт клиента (ссылка у службы) */
} ksvc_req_t;

int  ksvc_init(ksvc_t* s, const char* port_name);          /* 0 - готово */
int  ksvc_next(ksvc_t* s, ksvc_req_t* r);                  /* 1 - запрос получен, 0 - очередь пуста */
void ksvc_done(ksvc_req_t* r);                             /* отпустить вложение запроса */
void ksvc_reply_code(ksvc_t* s, const ksvc_req_t* r, int64_t code);
/* Ответ с телом: res пишет body(w, ctx) ровно одним элементом CBOR; code - после него. Не поместилось - ответ KSVC_E_ARG. */
void ksvc_reply_body(ksvc_t* s, const ksvc_req_t* r, int64_t code, void (*body)(cbor_w* w, void* ctx), void* ctx);
void ksvc_housekeeping(ksvc_t* s);                         /* раз в несколько проходов: снять привязки погибших клиентов */
#endif
