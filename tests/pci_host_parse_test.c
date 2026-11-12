#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pci_host_parse.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v);
}

int main(void) {
    uint8_t ranges[56] = {0};
    pci_window_t m32, m64;

    /* PCI child cells: flags + 64-bit PCI address; parent and size: two cells. */
    put32(ranges + 0, 0x02000000); put64(ranges + 4, 0x40000000);
    put64(ranges + 12, 0x40000000); put64(ranges + 20, 0x01000000);
    put32(ranges + 28, 0x03000000); put64(ranges + 32, 0x100000000ULL);
    put64(ranges + 40, 0x100000000ULL); put64(ranges + 48, 0x00200000);

    CHECK(arm_pci_parse_ranges(ranges, sizeof ranges, 3, 2, 2, &m32, &m64) == 0);
    CHECK(m32.pci_base == 0x40000000 && m32.size == 0x01000000);
    CHECK(m32.next == m32.pci_base && m32.cpu_off == 0);
    CHECK(m64.pci_base == 0x100000000ULL && m64.size == 0x00200000);

    /* Negative CPU-to-PCI offset is represented without unsigned wraparound. */
    memset(ranges, 0, sizeof ranges);
    put32(ranges, 0x02000000); put64(ranges + 4, 0x80000000);
    put64(ranges + 12, 0x70000000); put64(ranges + 20, 0x01000000);
    CHECK(arm_pci_parse_ranges(ranges, 28, 3, 2, 2, &m32, &m64) == 0);
    CHECK(m32.cpu_off == -(int64_t)0x10000000);

    CHECK(arm_pci_parse_ranges(ranges, 27, 3, 2, 2, &m32, &m64) != 0);
    CHECK(arm_pci_parse_ranges(ranges, 28, 2, 2, 2, &m32, &m64) != 0);
    CHECK(arm_pci_parse_ranges(ranges, 28, 3, 3, 2, &m32, &m64) != 0);
    CHECK(arm_pci_parse_ranges(ranges, 28, 3, 2, 3, &m32, &m64) != 0);
    CHECK(arm_pci_parse_ranges(0, 4, 3, 2, 2, &m32, &m64) != 0);

    /* PCI address + window size must not wrap. */
    memset(ranges, 0, sizeof ranges);
    put32(ranges, 0x02000000); put64(ranges + 4, UINT64_MAX - 0xFFF);
    put64(ranges + 12, 0x10000000); put64(ranges + 20, 0x2000);
    CHECK(arm_pci_parse_ranges(ranges, 28, 3, 2, 2, &m32, &m64) != 0);

    /* A CPU offset outside int64_t cannot be represented by pci_window_t. */
    memset(ranges, 0, sizeof ranges);
    put32(ranges, 0x02000000); put64(ranges + 4, 0);
    put64(ranges + 12, 0x8000000000000000ULL); put64(ranges + 20, 0x1000);
    CHECK(arm_pci_parse_ranges(ranges, 28, 3, 2, 2, &m32, &m64) != 0);

    /* Empty ranges is valid and leaves both windows unavailable. */
    CHECK(arm_pci_parse_ranges(0, 0, 3, 2, 2, &m32, &m64) == 0);
    CHECK(m32.size == 0 && m64.size == 0);

    printf(failures ? "PCI host ranges tests FAILED\n" :
           "PCI host ranges: cell layout, signed offsets, bounds and malformed input passed\n");
    return failures != 0;
}
