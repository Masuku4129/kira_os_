#include <stdio.h>
#include <stdint.h>
#include "arch/aarch64/range.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("arm_range: FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void) {
    uint64_t end = 0;
    CHECK(arm_range_end(0x1000, 0x2000, &end) && end == 0x3000);
    CHECK(!arm_range_end(0x1000, 0, &end));
    CHECK(!arm_range_end(UINT64_MAX - 7, 8, &end));
    CHECK(arm_range_contains(0x1000, 0x2000, 0x1000, 0x2000));
    CHECK(arm_range_contains(0x1000, 0x2000, 0x1800, 0x100));
    CHECK(!arm_range_contains(0x1000, 0x2000, 0x2FFF, 2));
    CHECK(!arm_range_contains(0x1000, 0x2000, 0x1000, 0));
    CHECK(!arm_range_contains(0x1000, 0x2000, UINT64_MAX - 3, 8));
    CHECK(arm_range_overlaps(0x1000, 0x1000, 0x1800, 0x100));
    CHECK(arm_range_overlaps(0x1000, 0x1000, 0x0FFF, 2));
    CHECK(!arm_range_overlaps(0x1000, 0x1000, 0x2000, 0x100));
    CHECK(!arm_range_overlaps(0x1000, 0x1000, 0x1000, 0));
    CHECK(!arm_range_overlaps(UINT64_MAX - 7, 8, 0x1000, 1));
    puts(failures ? "arm_range: FAIL" : "arm_range: overflow-safe half-open containment and overlap PASS");
    return failures != 0;
}
