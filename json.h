/* Строгий разбор JSON (RFC 8259) без выделения памяти: проверка документа и выборка значения по пути вида `a.b[2].c`.
   Чистый модуль: без ядра, без malloc, глубина вложенности ограничена (JSON_MAX_DEPTH), на любом мусоре возвращает ошибку.
   Потребитель: команда шелла `json файл путь` (fscmd.c) - проверка и чтение настроек в JSON-файлах KiraFS. */
#ifndef KIRA_JSON_H
#define KIRA_JSON_H
#include <stddef.h>

#define JSON_MAX_DEPTH 16

enum { JSON_OK = 0, JSON_ERR_SYNTAX = -1, JSON_ERR_DEPTH = -2, JSON_ERR_PATH = -3, JSON_ERR_NOTFOUND = -4, JSON_ERR_BUF = -5 };
enum { JSON_NULL = 1, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

typedef struct { int type; const char* p; size_t n; } json_val;   /* p/n - текст значения в документе (для строки - вместе с кавычками) */

/* Проверяет весь документ: ровно одно значение, после него только пробелы. JSON_OK или код ошибки. */
int json_validate(const char* s, size_t n);
/* Находит значение по пути ("" - корень). Документ проверяется целиком до выборки. */
int json_get(const char* s, size_t n, const char* path, json_val* out);
/* Строка без кавычек, escape-последовательности раскрыты, результат UTF-8 и завершён нулём. Возвращает длину или код ошибки. */
long json_string(const json_val* v, char* out, size_t cap);
#endif
