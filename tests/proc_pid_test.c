#include "../proc_pid.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    uint32_t next = 1;
    CHECK(proc_pid_take(&next) == 1 && next == 2);
    CHECK(proc_pid_take(&next) == 2 && next == 3);

    next = 0x7ffffffeU;
    CHECK(proc_pid_take(&next) == 0x7ffffffeU && next == 0x7fffffffU);
    CHECK(proc_pid_take(&next) == 0x7fffffffU && next == 0x80000000U);
    CHECK(proc_pid_take(&next) == 0 && next == 0x80000000U);

    next = 0;
    CHECK(proc_pid_take(&next) == 0 && next == 0);
    next = 0xffffffffU;
    CHECK(proc_pid_take(&next) == 0 && next == 0xffffffffU);
    CHECK(proc_pid_take(0) == 0);

    puts("process PID allocator: positive, non-reusing exhaustion PASS");
    return 0;
}
