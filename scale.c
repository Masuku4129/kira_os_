#include "scale.h"

unsigned scale_cpus(unsigned detected) {
    if (detected < 1) return 1;
    return detected > SCALE_MAX_CPUS ? SCALE_MAX_CPUS : detected;
}

// Потоки: на каждое ядро - холостой поток и запас под службы и программы; стек потока 16 КБ плюс запись таблицы,
// вместе не больше 1/16 ОЗУ. Но не меньше, чем нужно, чтобы поместились холостые потоки всех ядер и службы ядра.
unsigned scale_threads(unsigned cpus, uint64_t ram_kb) {
    cpus = scale_cpus(cpus);
    uint64_t want = 24 + 12ULL * cpus;
    uint64_t by_ram = ram_kb / 272;                      // (ОЗУ / 16) / 17 КБ на поток
    uint64_t cap = want < by_ram ? want : by_ram;
    uint64_t floor = cpus + 12;
    if (cap < floor) cap = floor;
    return cap > 2048 ? 2048 : (unsigned)cap;
}

// Программы Ring 3: по четыре на ядро, но не меньше восьми (если хватает памяти) и не больше одной на 8 МБ ОЗУ.
unsigned scale_procs(unsigned cpus, uint64_t ram_kb) {
    cpus = scale_cpus(cpus);
    uint64_t n = 4ULL * cpus;
    if (n < 8) n = 8;
    uint64_t by_ram = ram_kb / 8192;
    if (n > by_ram) n = by_ram;
    if (n < 2) n = 2;
    return n > 256 ? 256 : (unsigned)n;
}
