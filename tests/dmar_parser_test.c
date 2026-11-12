#include "dmar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static void checksum(uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    p[9] = 0;
    for (size_t i = 0; i < n; ++i) sum = (uint8_t)(sum + p[i]);
    p[9] = (uint8_t)(0u - sum);
}
static void base_table(uint8_t *p, size_t n)
{
    memset(p, 0, n);
    memcpy(p, "DMAR", 4);
    put32(p + 4, (uint32_t)n);
    p[8] = 1;
    p[36] = 47; /* 48-bit host physical address width minus one */
    put16(p + 48, 0); put16(p + 50, 16); /* DRHD */
    p[52] = 1; /* include-all */
    put16(p + 54, 0);
    put64(p + 56, 0xfed90000ull);
    checksum(p, n);
}
static int count_unit(const struct kira_dmar_unit *u, void *ctx)
{
    unsigned *count = ctx;
    assert(u->segment == 0 && u->include_all && u->register_base == 0xfed90000ull);
    ++*count;
    return 0;
}

int main(void)
{
    uint8_t table[96];
    struct kira_dmar_inventory inventory;
    unsigned count = 0;

    base_table(table, sizeof table);
    put16(table + 64, 1); put16(table + 66, 32); /* RMRR */
    put16(table + 68, 0); put16(table + 70, 0);
    put64(table + 72, 0x100000ull);
    put64(table + 80, 0x100fffull);
    table[88] = 1; table[89] = 8; /* endpoint scope */
    table[94] = 2; table[95] = 0;
    checksum(table, sizeof table);

    assert(kira_dmar_parse(table, sizeof table, &inventory) == KIRA_DMAR_OK);
    assert(inventory.host_address_width == 48 && inventory.unit_count == 1);
    assert(inventory.rmrr_count == 1 && inventory.rmrrs[0].base == 0x100000ull);
    assert(inventory.rmrrs[0].scope_count == 1);
    assert(inventory.rmrrs[0].limit == 0x100fffull);
    assert(kira_dmar_for_each_unit(&inventory, count_unit, &count) == KIRA_DMAR_OK);
    assert(count == 1);
    {
        uint8_t optin[sizeof table];
        memcpy(optin, table, sizeof optin); optin[37] = 0x4; checksum(optin, sizeof optin);
        assert(kira_dmar_parse(optin, sizeof optin, &inventory) == KIRA_DMAR_OK);
        assert(inventory.dma_ctrl_platform_opt_in == 1);
    }

    assert(kira_dmar_parse(table, 48, &inventory) == KIRA_DMAR_E_LENGTH);
    assert(kira_dmar_parse(0, sizeof table, &inventory) == KIRA_DMAR_E_ARGUMENT);

    {
        uint8_t bad[sizeof table];
        memcpy(bad, table, sizeof bad); bad[37] = 0x80; checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        memcpy(bad, table, sizeof bad); bad[52] = 0x80; checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        memcpy(bad, table, sizeof bad); bad[53] = 0x10; checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        memcpy(bad, table, sizeof bad); put16(bad + 66, 0); checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        memcpy(bad, table, sizeof bad); put64(bad + 80, 0x100ffeull); checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        memcpy(bad, table, sizeof bad); put64(bad + 56, 0x1000000000000ull); checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        memcpy(bad, table, sizeof bad); put64(bad + 80, 0x1000000000fffull); checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
    }

    /* A future structure type may carry DMA policy: do not silently ignore it. */
    {
        uint8_t bad[68];
        base_table(bad, sizeof bad);
        put16(bad + 64, 7); put16(bad + 66, 4); checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
    }

    /* A known PCI scope must have a supported scope type and zero reserved bytes. */
    {
        uint8_t bad[72];
        base_table(bad, sizeof bad);
        put16(bad + 50, 24);
        bad[64] = 6; bad[65] = 8; /* unknown scope type */
        bad[70] = 1; bad[71] = 0;
        checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
        bad[64] = 1; bad[66] = 1; checksum(bad, sizeof bad);
        assert(kira_dmar_parse(bad, sizeof bad, &inventory) == KIRA_DMAR_E_STRUCTURE);
    }

    puts("DMAR parser: topology, RMRR, reserved fields, scope and unknown-type rejection passed");
    return 0;
}
