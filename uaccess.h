#ifndef UACCESS_H
#define UACCESS_H
#include <stdint.h>

/* Копирование между ядром и памятью программы. Адрес и права проверяются по таблицам страниц текущего процесса,
   копирование переживает исчезновение страницы (другой поток снял отображение) - вернётся ошибка, а не авария ядра.
   При SMAP прямое обращение ядра к памяти программ запрещено процессором: только через эти функции. */
int copy_from_user(void* dst, uint64_t usrc, uint64_t n);      /* 0 - скопировано */
int copy_to_user(uint64_t udst, const void* src, uint64_t n);
int user_read32(uint64_t uaddr, uint32_t* out);                 /* выровненное слово (futex) */
/* Диапазон доступен программе (write - для записи); часть стека, которой программа ещё не касалась, добавляется. */
int user_range(uint64_t va, uint64_t len, int write);

/* Обработчик #PF: ошибка внутри копирования - продолжить с возвратом -1. 1 - обработано. */
int uaccess_fixup(uint64_t* frame, uint64_t rip);
int uaccess_enable_smap(void);                                  /* 1 - SMAP включён на этом процессоре */

#endif
