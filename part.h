#ifndef PART_H
#define PART_H
#include <stdint.h>
#include "blk.h"

/* Разделы съёмного диска: MBR (основные записи), GPT (защитная MBR + заголовок с CRC + записи) или «суперфлоппи»
   (файловая система с нулевого сектора, без таблицы). Находит разделы, на которых может быть FAT. */
#define PART_MAX 128     /* столько записей у GPT по стандарту */
typedef struct { uint64_t start, count; uint8_t mbr_type; int gpt; } part_t;

/* Число найденных разделов (не больше max). Только читает. sector0 - уже прочитанный нулевой сектор (512 байт). */
int part_scan(blk_dev_t* dev, const uint8_t* sector0, part_t* out, int max);

#endif
