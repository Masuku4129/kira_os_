#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* Часы реального времени (CMOS RTC). Время читается один раз при загрузке (с ожиданием конца обновления
   и повторным чтением до совпадения), дальше идёт по монотонным часам TSC: порты не трогаются каждую секунду. */

typedef struct { int year, month, day, hour, min, sec; } rtc_time_t;

/* Чистые функции (проверяются на хосте). */
int64_t rtc_days_from_civil(int y, int m, int d);                /* дни от 1970-01-01 */
void    rtc_civil_from_days(int64_t z, int* y, int* m, int* d);
int     rtc_weekday(int64_t days);                               /* 0 - понедельник */
/* Разбор регистров CMOS: r[0..5] = секунды, минуты, часы, день, месяц, год (как прочитаны), status_b - регистр B. */
int     rtc_decode(const uint8_t r[6], uint8_t status_b, int century, rtc_time_t* out);   /* 0 - корректно */
int64_t rtc_to_epoch(const rtc_time_t* t);
void    rtc_from_epoch(int64_t s, rtc_time_t* t);
/* «ПЯТНИЦА, 30 ОКТЯБРЯ 2026» и «13:51» */
void    rtc_format_date(const rtc_time_t* t, char* out, unsigned cap);
void    rtc_format_hm(const rtc_time_t* t, char* out, unsigned cap);

/* Ядро */
int     rtc_init(void);                    /* 0 - время прочитано */
int     rtc_now(int32_t tz_min, rtc_time_t* out);   /* 0 - есть время */

#endif
