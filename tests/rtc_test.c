// Хост-тест часов: календарь (дни, дни недели, високосные годы), разбор регистров CMOS (BCD/двоичный, 12/24 ч),
// часовой пояс через смену суток и года, русские подписи.
#include <stdio.h>
#include <string.h>
#include "rtc.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    CHECK(rtc_days_from_civil(1970, 1, 1) == 0);
    CHECK(rtc_days_from_civil(2000, 3, 1) == 11017);
    CHECK(rtc_weekday(rtc_days_from_civil(2026, 10, 30)) == 4);          /* пятница */
    CHECK(rtc_weekday(rtc_days_from_civil(2042, 10, 14)) == 1);          /* вторник, как на образце */
    for (int64_t z = -800000; z < 800000; z += 997) { int y, m, d; rtc_civil_from_days(z, &y, &m, &d); CHECK(rtc_days_from_civil(y, m, d) == z); }
    int y, m, d; rtc_civil_from_days(rtc_days_from_civil(2024, 2, 29), &y, &m, &d); CHECK(y == 2024 && m == 2 && d == 29);
    CHECK(rtc_days_from_civil(2100, 3, 1) - rtc_days_from_civil(2100, 2, 28) == 1);   /* 2100 не високосный */

    rtc_time_t t;
    uint8_t bcd_regs[6] = { 0x59, 0x51, 0x13, 0x30, 0x10, 0x26 };
    CHECK(rtc_decode(bcd_regs, 0x02, 20, &t) == 0 && t.year == 2026 && t.month == 10 && t.day == 30 && t.hour == 13 && t.min == 51 && t.sec == 59);
    uint8_t bin[6] = { 5, 7, 23, 31, 12, 99 };
    CHECK(rtc_decode(bin, 0x06, 0, &t) == 0 && t.year == 1999 && t.hour == 23 && t.day == 31);
    uint8_t pm[6] = { 0, 0, 0x81, 1, 1, 0x26 };                           /* 12-часовой режим: 1 PM */
    CHECK(rtc_decode(pm, 0x00, 20, &t) == 0 && t.hour == 13);
    uint8_t am12[6] = { 0, 0, 0x12, 1, 1, 0x26 };                         /* 12 AM = 0 часов */
    CHECK(rtc_decode(am12, 0x00, 20, &t) == 0 && t.hour == 0);
    uint8_t bad[6] = { 0x60, 0, 0, 1, 1, 0x26 };
    CHECK(rtc_decode(bad, 0x02, 20, &t) != 0);

    rtc_time_t u = { 2026, 12, 31, 22, 30, 0 };
    rtc_from_epoch(rtc_to_epoch(&u) + 180 * 60, &t);                       /* UTC+3: новый год */
    CHECK(t.year == 2027 && t.month == 1 && t.day == 1 && t.hour == 1 && t.min == 30);
    rtc_from_epoch(-1, &t); CHECK(t.year == 1969 && t.month == 12 && t.day == 31 && t.hour == 23 && t.sec == 59);

    char buf[64];
    rtc_time_t f = { 2026, 10, 30, 13, 5, 0 };
    rtc_format_date(&f, buf, sizeof buf); CHECK(!strcmp(buf, "ПЯТНИЦА, 30 ОКТЯБРЯ 2026"));
    rtc_format_hm(&f, buf, sizeof buf); CHECK(!strcmp(buf, "13:05"));
    rtc_format_date(&f, buf, 8); CHECK(strlen(buf) < 8);                    /* не выходит за буфер */
    printf("%s\n", fails ? "rtc: FAILED" : "rtc: calendar, weekdays, leap years, CMOS BCD/binary/12h decode, time zone rollover, Russian date passed");
    return fails != 0;
}
