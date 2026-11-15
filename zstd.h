/* Zstandard (RFC 8878): распаковщик кадров. Только разжатие, без словарей, без выделения памяти: рабочая область (zstd_work) и выходной буфер
   даёт вызывающий. Все границы проверяются, любой повреждённый или враждебный вход даёт ошибку, а не запись вне буфера.
   Контрольная сумма содержимого кадра (XXH64) проверяется, если она есть. Составные потоки (несколько кадров подряд) и пропускаемые кадры
   поддерживаются. Сжатие (компрессор) не входит: образы и пакеты сжимаются на хосте, а Кира их читает. */
#ifndef KIRA_ZSTD_H
#define KIRA_ZSTD_H
#include <stdint.h>
#include <stddef.h>

enum { ZSTD_OK = 0, ZSTD_ERR_FORMAT = -1, ZSTD_ERR_TRUNC = -2, ZSTD_ERR_DST_SMALL = -3, ZSTD_ERR_UNSUPPORTED = -4, ZSTD_ERR_CHECKSUM = -5, ZSTD_ERR_CORRUPT = -6 };
#define ZSTD_BLOCK_MAX (128 * 1024)

typedef struct zstd_work zstd_work;
size_t zstd_work_size(void);                                   /* сколько байт нужно под zstd_work */
/* Размер содержимого первого кадра, если он записан в заголовке; иначе -1 (нужно знать верхнюю границу заранее). */
int64_t zstd_frame_content_size(const void* src, size_t n);
/* Разжимает все кадры из src в dst. *out - число записанных байт. work - память не меньше zstd_work_size(), выровненная на 8. */
int zstd_decompress(const void* src, size_t n, void* dst, size_t cap, size_t* out, zstd_work* work);
#endif
