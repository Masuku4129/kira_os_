/* kas: ассемблер x86-64 (подмножество синтаксиса nasm) -> ELF64 ET_REL.
 * Чистый C99. Ядро не вызывает malloc: вся память берётся из арены, которую даёт вызывающий.
 * Один и тот же код собирается на хосте и в Кире (ввод-вывод только в kas_main.c). */
#ifndef KAS_H
#define KAS_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *base; size_t cap, used; } KasArena;

/* mem должна быть выровнена по 16 байт. Постоянная часть (текст после раскрытия макросов, операторы, символы,
 * перемещения) около 2 МиБ плюс данные секций и выходной образ; KAS_ARENA_MIN с запасом покрывает файлы ядра. */
#define KAS_ARENA_MIN ((size_t)4 << 20)
void kas_arena_init(KasArena *a, void *mem, size_t cap);

/* Собирает src (len байт, nasm-синтаксис) в ELF64 ET_REL.
 * file - имя исходника: идёт в символ STT_FILE и в сообщения об ошибках.
 * Успех: возвращает 0, out и outlen указывают на образ внутри арены.
 * Ошибка: возвращает 1, в err (errcap байт) текст "файл:строка: ошибка: сообщение". */
int kas_assemble(KasArena *a, const char *file, const char *src, size_t len,
                 const uint8_t **out, size_t *outlen, char *err, size_t errcap);
#endif
