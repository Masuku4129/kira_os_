#include "syscall_services.h"
#include "syscall_numbers.h"
#include "rtc.h"

int syscall_service_clock(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t* result) {
    (void)a1; (void)a2; (void)a3;
    if (!result || nr != SYS_TIME) return 0;
    rtc_time_t tm;
    *result = rtc_now(0, &tm) == 0
        ? (uint64_t)rtc_to_epoch(&tm) : (uint64_t)-1;
    return 1;
}
