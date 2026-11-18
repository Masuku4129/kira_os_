#ifndef KASLR_ARCH_H
#define KASLR_ARCH_H
// KASLR x86-64: состояние, которое оставляет раннее перемещение образа (arch/x86_64/kaslr_arch.c), для журнала и команды kaslr.
#include <stdint.h>

enum { KASLR_SRC_RDSEED = 1, KASLR_SRC_RDRAND = 2, KASLR_SRC_TSC_PIT = 4 };

typedef struct {
    uint64_t link_base;        // где образ слинкован и где его загрузил загрузчик (1 МБ)
    uint64_t base;             // где он работает сейчас
    uint64_t slide;            // base - link_base; 0 - образ не перемещался
    uint64_t slots;            // из скольких позиций (2 МБ, свободная память первого ГБ) выбрана эта
    uint32_t entropy_src;      // KASLR_SRC_*: что вошло в энтропию
    int32_t  reason;           // KASLR_OK или причина, по которой сдвига нет (kaslr.h)
    uint64_t relocs;           // сколько слов исправлено
    uint64_t keep_lo, keep_hi; // Multiboot2-структура GRUB может лежать в зазоре внутри старого образа (между rodata и text): её не стираем
} kaslr_state_t;

const kaslr_state_t* kaslr_info(void);
// Строка о KASLR в журнал (адрес базы - по политике kptr.h: без режима отладки не печатается).
void kaslr_log(void);

// Команды консоли (arch/x86_64/kaslrcmd.c).
void kaslr_cmd(const char* arg);   // kaslr: состояние случайного адреса ядра
void kptr_cmd(const char* arg);    // kptr [debug on|off]: политика печати адресов ядра
void wx_cmd(const char* arg);      // wx: W^X образа ядра (ранняя карта, обход таблиц, пробные записи в .text/.rodata)

#endif
