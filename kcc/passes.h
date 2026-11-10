/* kcc: флаги отключения отдельных проходов оптимизации (-fno-xxx; для сверки с отключённым проходом и для поиска ошибок).
 * Определены в opt.c; разбор командной строки - kcc_flag. */
#ifndef KCC_PASSES_H
#define KCC_PASSES_H
int kcc_flag(const char *arg, KOptions *options);                /* 1, если arg - известный флаг этого списка (и он применён) */
#endif
