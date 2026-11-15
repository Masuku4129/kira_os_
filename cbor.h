/* CBOR (RFC 8949): компактная двоичная сериализация конфигураций, метаданных и сообщений. Без выделения памяти и без
   вещественной арифметики (ядро собрано без SSE): числа с плавающей точкой читаются как «сырые» биты и пропускаются.
   Писатель пишет в буфер вызывающего и при нехватке места запоминает ошибку (cbor_w.err), читатель проверяет каждую границу.
   Кодирование детерминированное (RFC 8949, 4.2.1: наименьшая длина числа, определённые длины); читатель в строгом режиме
   отвергает недетерминированное кодирование и неопределённые длины - формат метаданных однозначен, его можно подписывать. */
#ifndef KIRA_CBOR_H
#define KIRA_CBOR_H
#include <stdint.h>
#include <stddef.h>

enum { CBOR_NOT_FOUND = 1, CBOR_OK = 0, CBOR_ERR_SPACE = -1, CBOR_ERR_TRUNC = -2, CBOR_ERR_FORMAT = -3, CBOR_ERR_DEPTH = -4, CBOR_ERR_STRICT = -5, CBOR_ERR_TYPE = -6 };
enum { CBOR_UINT, CBOR_NINT, CBOR_BYTES, CBOR_TEXT, CBOR_ARRAY, CBOR_MAP, CBOR_TAG, CBOR_FALSE, CBOR_TRUE, CBOR_NULL, CBOR_UNDEF, CBOR_SIMPLE, CBOR_FLOAT };
#define CBOR_MAX_DEPTH 16

typedef struct { uint8_t* p; size_t cap, n; int err; } cbor_w;
void cbor_w_init(cbor_w* w, uint8_t* buf, size_t cap);
void cbor_put_uint(cbor_w* w, uint64_t v);
void cbor_put_int(cbor_w* w, int64_t v);
void cbor_put_bytes(cbor_w* w, const void* d, size_t n);
void cbor_put_text(cbor_w* w, const char* s, size_t n);          /* UTF-8, проверяется */
void cbor_put_cstr(cbor_w* w, const char* s);
void cbor_put_array(cbor_w* w, uint64_t count);                    /* затем ровно count элементов */
void cbor_put_map(cbor_w* w, uint64_t pairs);                      /* затем ровно pairs пар «ключ, значение» */
void cbor_put_tag(cbor_w* w, uint64_t tag);
void cbor_put_bool(cbor_w* w, int v);
void cbor_put_null(cbor_w* w);

typedef struct { const uint8_t* p; size_t n, i; int strict; } cbor_r;
typedef struct {
    int type;
    uint64_t val;               /* UINT: значение; NINT: -1 - val; BYTES/TEXT: длина; ARRAY/MAP: число элементов/пар; TAG: номер; SIMPLE/FLOAT: сырые биты */
    const uint8_t* data;        /* BYTES/TEXT: указатель на содержимое внутри входного буфера */
} cbor_item;
void cbor_r_init(cbor_r* r, const void* buf, size_t n, int strict);
int cbor_next(cbor_r* r, cbor_item* it);                           /* читает один заголовок (для массивов и карт - только заголовок) */
int cbor_skip(cbor_r* r);                                          /* пропускает один элемент целиком, вложенные тоже */
int cbor_check(const void* buf, size_t n, int strict);             /* ровно один корректный элемент на весь буфер */
/* Карта по адресу buf: ищет текстовый ключ, кладёт значение в *out (r переходит к значению). 0 - найдено, CBOR_NOT_FOUND - ключа нет, меньше 0 - ошибка. */
int cbor_map_find(const void* buf, size_t n, const char* key, cbor_item* out, cbor_r* at);
#endif
