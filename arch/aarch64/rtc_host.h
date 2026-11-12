#ifndef KIRA_AARCH64_RTC_HOST_H
#define KIRA_AARCH64_RTC_HOST_H

typedef void (*arm_rtc_log_fn)(const char* format, ...);

/* Discover the FDT-described PL031 RTC and publish its UTC epoch. */
void arm_rtc_init(const void* fdt, arm_rtc_log_fn log);

#endif
