#include "rtc.h"

/* Дни от 1970-01-01 по григорианскому календарю (алгоритм Х. Хиннанта, целочисленный). */
int64_t rtc_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rtc_civil_from_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

int rtc_weekday(int64_t days) {             /* 1970-01-01 - четверг (3, если понедельник 0) */
    int64_t w = (days + 3) % 7;
    return (int)(w < 0 ? w + 7 : w);
}

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

int rtc_decode(const uint8_t r[6], uint8_t status_b, int century, rtc_time_t* t) {
    int binary = (status_b & 0x04) != 0, h24 = (status_b & 0x02) != 0;
    uint8_t hr = r[2];
    int pm = !h24 && (hr & 0x80);
    hr &= 0x7F;
    t->sec = binary ? r[0] : bcd(r[0]);
    t->min = binary ? r[1] : bcd(r[1]);
    t->hour = binary ? hr : bcd(hr);
    t->day = binary ? r[3] : bcd(r[3]);
    t->month = binary ? r[4] : bcd(r[4]);
    int y = binary ? r[5] : bcd(r[5]);
    if (!h24) { if (t->hour == 12) t->hour = 0; if (pm) t->hour += 12; }
    t->year = century > 0 ? century * 100 + y : (y < 70 ? 2000 + y : 1900 + y);
    if (t->sec > 59 || t->min > 59 || t->hour > 23 || t->day < 1 || t->day > 31 || t->month < 1 || t->month > 12) return -1;
    return 0;
}

int64_t rtc_to_epoch(const rtc_time_t* t) {
    return rtc_days_from_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
}

void rtc_from_epoch(int64_t s, rtc_time_t* t) {
    int64_t days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
    int64_t rem = s - days * 86400;
    rtc_civil_from_days(days, &t->year, &t->month, &t->day);
    t->hour = (int)(rem / 3600); t->min = (int)(rem / 60 % 60); t->sec = (int)(rem % 60);
}

static const char* const wd[7] = { "ПОНЕДЕЛЬНИК", "ВТОРНИК", "СРЕДА", "ЧЕТВЕРГ", "ПЯТНИЦА", "СУББОТА", "ВОСКРЕСЕНЬЕ" };
static const char* const mo[12] = { "ЯНВАРЯ", "ФЕВРАЛЯ", "МАРТА", "АПРЕЛЯ", "МАЯ", "ИЮНЯ", "ИЮЛЯ", "АВГУСТА", "СЕНТЯБРЯ", "ОКТЯБРЯ", "НОЯБРЯ", "ДЕКАБРЯ" };

static unsigned put_s(char* o, unsigned n, unsigned cap, const char* s) { while (*s && n + 1 < cap) o[n++] = *s++; o[n] = 0; return n; }
static unsigned put_u(char* o, unsigned n, unsigned cap, unsigned v, int width) {
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k < width) t[k++] = '0';
    while (k && n + 1 < cap) o[n++] = t[--k];
    o[n] = 0;
    return n;
}

void rtc_format_date(const rtc_time_t* t, char* out, unsigned cap) {
    unsigned n = 0;
    n = put_s(out, n, cap, wd[rtc_weekday(rtc_days_from_civil(t->year, t->month, t->day))]);
    n = put_s(out, n, cap, ", ");
    n = put_u(out, n, cap, (unsigned)t->day, 1);
    n = put_s(out, n, cap, " ");
    n = put_s(out, n, cap, mo[(t->month - 1) % 12]);
    n = put_s(out, n, cap, " ");
    put_u(out, n, cap, (unsigned)t->year, 4);
}

void rtc_format_hm(const rtc_time_t* t, char* out, unsigned cap) {
    unsigned n = put_u(out, 0, cap, (unsigned)t->hour, 2);
    n = put_s(out, n, cap, ":");
    put_u(out, n, cap, (unsigned)t->min, 2);
}

#ifndef KIRA_HOST_TEST
#include "clock.h"
#include "cpu.h"
#include "uart.h"

static inline void outb_(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb_(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static uint8_t cmos(uint8_t reg) { outb_(0x70, (uint8_t)(0x80 | reg)); return inb_(0x71); }   /* бит 7: NMI не трогаем */

static int64_t boot_epoch;
static uint64_t boot_ns;
static int have;

int rtc_init(void) {
    static const uint8_t regs[6] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };
    uint8_t a[6], b[6];
    int ok = 0;
    for (int attempt = 0; attempt < 5 && !ok; attempt++) {
        uint32_t guard = 0;
        while ((cmos(0x0A) & 0x80) && ++guard < 1000000) { }           /* идёт обновление: ждём */
        for (int i = 0; i < 6; i++) a[i] = cmos(regs[i]);
        guard = 0;
        while ((cmos(0x0A) & 0x80) && ++guard < 1000000) { }
        for (int i = 0; i < 6; i++) b[i] = cmos(regs[i]);
        ok = 1;
        for (int i = 0; i < 6; i++) if (a[i] != b[i]) ok = 0;             /* два одинаковых чтения подряд */
    }
    rtc_time_t t;
    uint8_t century_raw = cmos(0x32);
    uint8_t sb = cmos(0x0B);
    int century = (sb & 0x04) ? century_raw : (century_raw >> 4) * 10 + (century_raw & 15);
    if (century < 19 || century > 22) century = 0;                       /* регистр века есть не везде */
    if (!ok || rtc_decode(b, sb, century, &t) != 0 || t.year < 2000 || t.year > 2199) {
        uart_print("[KIRA RTC] Real-time clock unreadable: date and time are not shown.\n");
        return -1;
    }
    boot_epoch = rtc_to_epoch(&t);
    boot_ns = clock_ns();
    have = 1;
    char d[48], hm[8];
    rtc_format_date(&t, d, sizeof d); rtc_format_hm(&t, hm, sizeof hm);
    uart_print("[KIRA RTC] Real-time clock (CMOS, read as UTC): "); uart_print(d); uart_print(" "); uart_print(hm); uart_print("\n");
    return 0;
}

int rtc_now(int32_t tz_min, rtc_time_t* out) {
    if (!have) return -1;
    int64_t s = boot_epoch + (int64_t)((clock_ns() - boot_ns) / 1000000000ULL) + (int64_t)tz_min * 60;
    rtc_from_epoch(s, out);
    return 0;
}
#endif
