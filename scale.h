#ifndef SCALE_H
#define SCALE_H

#include <stdint.h>

// Политика масштабирования: размеры таблиц и лимиты выводятся из найденного железа (число ядер, ОЗУ), а не
// заданы константой под одну тестовую машину. Чистые функции, проверяются на хосте для 1..64 ядер.
#define SCALE_MAX_CPUS 256         // = CPU_MAX: таблица ядер и маски сродства (cpumask.h)

unsigned scale_cpus(unsigned detected);                       // сколько ядер система использует (1..SCALE_MAX_CPUS)
unsigned scale_threads(unsigned cpus, uint64_t ram_kb);       // ёмкость таблицы потоков
unsigned scale_procs(unsigned cpus, uint64_t ram_kb);         // сколько программ Ring 3 одновременно

#endif
