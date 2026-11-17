/* Хранилище по содержимому (CAS) и дерево Меркла: объект адресуется SHA-256 своего содержимого, одинаковые данные хранятся один раз,
   целостность проверяется при каждом чтении, большие файлы - манифест из хэшей кусков (Merkle DAG) с корнем по RFC 6962.
   Без выделения памяти: таблицу записей и арену данных даёт вызывающий. Основа для версионирования, подписанных обновлений и
   дедупликации; подключение к KiraFS - отдельный шаг (docs/architecture/REQUIRED_TECH_PLAN.md). */
#ifndef KIRA_CAS_H
#define KIRA_CAS_H
#include <stdint.h>
#include <stddef.h>

#define CAS_HASH 32
enum { CAS_OK = 0, CAS_ERR_FULL = -1, CAS_ERR_NOTFOUND = -2, CAS_ERR_CORRUPT = -3, CAS_ERR_ARG = -4, CAS_ERR_SPACE = -5 };

/* --- дерево Меркла, RFC 6962 (хэш листа = H(0x00||данные), узла = H(0x01||левый||правый)) --- */
void merkle_leaf(const void* data, size_t len, uint8_t out[CAS_HASH]);
void merkle_root(const uint8_t (*leaves)[CAS_HASH], size_t n, uint8_t out[CAS_HASH]);        /* n = 0: хэш пустой строки */
/* Путь включения для листа index: возвращает число хэшей (0..64) в path или отрицательную ошибку. */
int merkle_path(const uint8_t (*leaves)[CAS_HASH], size_t n, size_t index, uint8_t (*path)[CAS_HASH], size_t path_cap);
/* Проверка: лист с номером index из n входит в дерево с корнем root. 0 - да. */
int merkle_verify(const uint8_t leaf[CAS_HASH], size_t index, size_t n, const uint8_t (*path)[CAS_HASH], size_t plen, const uint8_t root[CAS_HASH]);

/* --- хранилище --- */
typedef struct { uint8_t hash[CAS_HASH]; uint32_t off, len, refs, used; } cas_entry;
typedef struct { cas_entry* e; size_t max; uint8_t* arena; size_t cap, top; size_t count; } cas_store;
void cas_init(cas_store* s, cas_entry* entries, size_t max_entries, uint8_t* arena, size_t arena_cap);
int cas_put(cas_store* s, const void* data, size_t len, uint8_t hash[CAS_HASH]);            /* повтор: refs++, данные не дублируются */
int cas_get(const cas_store* s, const uint8_t hash[CAS_HASH], const uint8_t** data, size_t* len);   /* с проверкой хэша */
int cas_has(const cas_store* s, const uint8_t hash[CAS_HASH]);
int cas_release(cas_store* s, const uint8_t hash[CAS_HASH]);                                 /* refs--; освобождается в cas_gc */
size_t cas_gc(cas_store* s);                                                                 /* убирает объекты без ссылок, уплотняет арену; возвращает освобождённые байты */
/* Файл кусками: объекты-куски + манифест. Возвращает хэш манифеста и корень Меркла по хэшам кусков (RFC 6962).
   scratch - рабочая память вызывающего размером не меньше cas_file_scratch(len, chunk). */
size_t cas_file_scratch(size_t len, size_t chunk);
int cas_put_file(cas_store* s, const void* data, size_t len, size_t chunk, uint8_t* scratch, size_t scratch_cap,
                 uint8_t manifest[CAS_HASH], uint8_t root[CAS_HASH]);
int cas_get_file(const cas_store* s, const uint8_t manifest[CAS_HASH], void* out, size_t cap, size_t* len);   /* каждый кусок проверяется */
#endif
