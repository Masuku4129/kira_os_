#ifndef KOBJ_H
#define KOBJ_H

#include <stdint.h>
#include <stddef.h>

// Объекты ядра для программ: порты (именованные очереди сообщений) и разделяемая память. Программа работает с ними
// через дескрипторы (номера в таблице процесса); объект живёт, пока на него есть ссылки (дескрипторы, отображения,
// идущие вызовы), и освобождается последним отпустившим. Имена - общее пространство: [a-z0-9._-], до 31 знака.

#define KNAME_MAX    31
// Пределы определяет машина: таблица дескрипторов и реестр объектов растут по мере надобности, размер разделяемой
// памяти ограничен свободной ОЗУ (не больше половины), сообщение - 64 КБ (копируется ядром целиком).
#define KH_LIMIT     (1u << 20)  // дескрипторов на процесс (защита от утечки в программе, а не размер таблицы)
#define PORT_QUEUE   64          // сообщений в очереди порта
#define PORT_MSG_MAX 65536       // байт в сообщении

// Ошибки (отрицательные значения в rax системного вызова)
#define KE_ARG     (-10)
#define KE_NOENT   (-2)
#define KE_EXIST   (-3)
#define KE_FULL    (-4)
#define KE_TIMEOUT (-5)
#define KE_AGAIN   (-6)
#define KE_NOMEM   (-7)
#define KE_BADH    (-8)
#define KE_LIMIT   (-9)
#define KE_PERM    (-12)         // у процесса нет права (caps.h)
#define KE_INTR    (-11)         // процесс завершается

#define KOPEN_CREATE 1           // создать, если нет
#define KOPEN_EXCL   2           // с CREATE: ошибка, если уже есть

enum { KOBJ_PORT = 1, KOBJ_SHM = 2 };

typedef struct kobj {
    uint32_t type;
    volatile uint32_t refs;
    char name[KNAME_MAX + 1];
} kobj_t;

// Таблица дескрипторов процесса (защищает вызывающий: мьютекс процесса). Дескриптор = индекс + 1. Растёт вдвое.
typedef struct { kobj_t** obj; uint32_t cap; } htab_t;
int     htab_add(htab_t* t, kobj_t* o);                  // дескриптор, KE_NOMEM или KE_LIMIT
void    htab_free(htab_t* t);                             // массив (объекты отпускает вызывающий)
uint32_t htab_cap(const htab_t* t);
kobj_t* htab_get(htab_t* t, int h, uint32_t type);       // 0 - нет такого или не того типа
kobj_t* htab_take(htab_t* t, int h);                      // убрать из таблицы (ссылку отпускает вызывающий)

int  kname_ok(const char* s);
void kobj_retain(kobj_t* o);
void kobj_release(kobj_t* o);                             // последняя ссылка уничтожает объект

// Порты
int     port_open(const char* name, int flags, kobj_t** out);                 // +1 ссылка
int     port_send(kobj_t* port, uint32_t from_pid, const void* data, uint32_t len);   // 0, KE_FULL, KE_ARG, KE_NOMEM
// Приём: длина сообщения (копируется не больше cap) и отправитель. timeout_ticks: 0 - не ждать, ~0 - без срока.
// stop() != 0 - ожидание прерывается (процесс завершается). Сообщение копируется в kbuf (ядерный буфер cap байт).
int64_t port_recv(kobj_t* port, void* kbuf, uint32_t cap, uint64_t timeout_ticks, uint32_t* from_pid, int (*stop)(void));
unsigned port_queued(kobj_t* port);

// Разделяемая память (страницы обнулены при создании)
int      shm_open(const char* name, uint32_t pages, int flags, kobj_t** out);  // pages - при создании; +1 ссылка
uint32_t shm_pages(kobj_t* shm);
uint64_t shm_frame(kobj_t* shm, uint32_t i);

unsigned kobj_count(void);
uint64_t kobj_messages_total(void);                      /* сообщений, отправленных через порты (для Разума) */                                // живых именованных объектов (для status)

#endif
