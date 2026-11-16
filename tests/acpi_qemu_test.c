/* Хост-тест на НАСТОЯЩИХ таблицах DSDT из QEMU: плата pc (PIIX3) и q35 (ICH9). Таблицы сняты командой ОС "acpi dump dsdt" в запущенной
   под QEMU системе (tools/acpi_dump_to_bin.py), лежат в tests/data/acpi/pc_dsdt.aml и q35_dsdt.aml. Проверяется: загрузка без ошибок, _INI,
   _PRT и связки (LNKx / GSIx) -> номера GSI, \_S5 интерпретатора = поиск байтов в acpi.c, устойчивость к обрезке и порче этих таблиц.
   Значения в ожиданиях - те, что ОС напечатала в QEMU (а не придуманные): плата pc -> маршрутизация PIIX3 (регистры 0x60..0x63 у 00:01.0 = 10,10,11,11). */
#include "acpidev.h"
#include "acpi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

void uart_print(const char* s) { (void)s; }          /* acpi.c печатает при разборе таблиц; тесту вывод не нужен */
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
static uint8_t piix_cfg[256];
static int hw_rw(void* u, int write, unsigned sp, uint64_t a, unsigned bits, uint64_t* v, uint32_t pci) {
    (void)u;
    unsigned nb = bits / 8;
    if (sp == AML_SP_PCI) {
        if (pci == (1u << 3) && a + nb <= 256) {                 /* 00:01.0 - мост ISA PIIX3, регистры маршрутизации PIRQ */
            if (write) for (unsigned i = 0; i < nb; i++) piix_cfg[a + i] = (uint8_t)(*v >> (8 * i));
            else { uint64_t r = 0; for (unsigned i = 0; i < nb; i++) r |= (uint64_t)piix_cfg[a + i] << (8 * i); *v = r; }
            return 0;
        }
        if (!write) *v = ~0ull;
        return 0;
    }
    if (sp == AML_SP_IO || sp == AML_SP_MEM) { if (!write) *v = 0; return 0; }
    return -1;
}
static void hw_sleep(void* u, uint32_t ms) { (void)u; (void)ms; }
static void hw_stall(void* u, uint32_t us) { (void)u; (void)us; }

static uint8_t* load_file(const char* path, uint32_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: нет файла %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    CHECK(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f); *len = (uint32_t)n;
    return b;
}

static void fix_sum(uint8_t* t, uint32_t len) {
    t[4] = (uint8_t)len; t[5] = (uint8_t)(len >> 8); t[6] = (uint8_t)(len >> 16); t[7] = (uint8_t)(len >> 24);
    t[9] = 0; uint8_t s = 0; for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + t[i]);
    t[9] = (uint8_t)(0 - s);
}

static void* G_mem; static size_t G_sz;
static int start(const uint8_t* tbl, uint32_t len, uint16_t sci) {
    acpidev_tab_t dsdt = { tbl, len };
    acpidev_hw_t hw; memset(&hw, 0, sizeof hw);
    hw.sci_int = sci; hw.pm1a_evt = 0x600; hw.pm1_evt_len = 4; hw.gpe0_blk = 0x620; hw.gpe0_len = 8;
    size_t asz = acpidev_arena_size(&dsdt, 0, 0);
    if (asz > G_sz) { free(G_mem); G_mem = malloc(asz); G_sz = asz; }
    aml_ops_t ops = { hw_rw, hw_sleep, hw_stall, 0, 0, 0, 0 };
    return acpidev_init(G_mem, G_sz, &ops, &hw, &dsdt, 0, 0);
}

static void one(const char* path, int q35) {
    uint32_t len; uint8_t* tbl = load_file(path, &len);
    memset(piix_cfg, 0, sizeof piix_cfg);
    piix_cfg[0x60] = 10; piix_cfg[0x61] = 10; piix_cfg[0x62] = 11; piix_cfg[0x63] = 11;     /* PIRQA..D, как их оставила прошивка SeaBIOS в QEMU */
    CHECK(start(tbl, len, 9) == 0);
    acpidev_stats_t st; acpidev_stats(&st);
    CHECK(st.ini_fail == 0);
    aml_stats_t as; aml_stats(acpidev_ctx(), &as);
    CHECK(as.load_errors == 0 && as.dup_names == 0 && as.nodes > 200);
    acpidev_irq_t irq;
    /* как ядро: сначала сообщаем прошивке режим контроллера прерываний (на q35 \_PIC(1) переключает _PRT на таблицу GSI) */
    { int pr = acpidev_pic_mode(1); CHECK(pr == 0 || pr == AML_E_NOTFOUND); }          /* у DSDT платы pc метода \_PIC нет - не ошибка */
    if (q35) {
        CHECK(acpidev_pci_irq(0, 0, 0, 1, &irq) && irq.gsi == 20 && irq.via_link && strcmp(irq.link, "GSIE") == 0 && irq.level && !irq.active_low);   /* QEMU: Interrupt(Level, ActiveHigh, Shared) */
        CHECK(acpidev_pci_irq(0, 0, 0, 2, &irq) && irq.gsi == 21);
        CHECK(acpidev_pci_irq(0, 3, 0, 1, &irq) && irq.gsi == 23);
        CHECK(acpidev_pci_irq(0, 25, 0, 1, &irq) && irq.gsi == 16 && strcmp(irq.link, "GSIA") == 0);
        CHECK(acpidev_pci_irq(0, 31, 0, 4, &irq) && irq.gsi == 19);
        CHECK(acpidev_pci_irq(0, 30, 0, 1, &irq) && irq.gsi == 20);
    } else {
        CHECK(acpidev_pci_irq(0, 1, 0, 1, &irq) && irq.gsi == 9 && strcmp(irq.link, "LNKS") == 0);
        CHECK(acpidev_pci_irq(0, 1, 0, 2, &irq) && irq.gsi == 10 && strcmp(irq.link, "LNKB") == 0);
        CHECK(acpidev_pci_irq(0, 3, 0, 1, &irq) && irq.gsi == 11 && strcmp(irq.link, "LNKC") == 0);
        CHECK(acpidev_pci_irq(0, 3, 0, 3, &irq) && irq.gsi == 10 && strcmp(irq.link, "LNKA") == 0);
        CHECK(acpidev_pci_irq(0, 3, 0, 4, &irq) && irq.gsi == 10);
        /* поменяли маршрутизацию в регистрах моста - связка это видит (читает настоящее состояние, а не кэш) */
        piix_cfg[0x62] = 5;
        CHECK(acpidev_pci_irq(0, 3, 0, 1, &irq) && irq.gsi == 5);
        piix_cfg[0x62] = 0x80;                                                        /* бит 7: линия отключена */
        CHECK(!acpidev_pci_irq(0, 3, 0, 1, &irq));
        piix_cfg[0x62] = 11;
    }
    CHECK(!acpidev_pci_irq(0, 0x1E, 0, 0, &irq));                                      /* pin 0 - не существует */
    CHECK(!acpidev_pci_irq(7, 0, 0, 1, &irq));                                         /* шины 7 нет */
    {   /* \_S5 интерпретатора = байтовый поиск ядра */
        uint8_t a1 = 0xFF, b1 = 0xFF, a2 = 0xEE, b2 = 0xEE;
        CHECK(acpidev_s5(&a1, &b1) == 0);
        CHECK(acpi_find_s5(tbl + 36, len - 36, &a2, &b2) == 0);
        CHECK(a1 == a2 && b1 == b2);
        printf("  %s: \\_S5 = %u,%u, объектов %u, _PRT и связки разобраны\n", q35 ? "q35" : "pc", a1, b1, as.nodes);
    }
    CHECK(acpidev_prepare_sleep(5) == 0);                                              /* \_PTS(5) */

    /* порча и обрезка настоящей таблицы: только ошибка, не падение (проверяет ASAN/UBSan) */
    uint8_t* w = malloc(len);
    uint64_t rs = 0x9E3779B97F4A7C15ull ^ (uint64_t)len;
    for (int it = 0; it < 300; it++) {
        memcpy(w, tbl, len);
        uint32_t l2 = len;
        rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
        if (it % 3 == 0) l2 = 37 + (uint32_t)(rs % (len - 37));
        else for (int k = 0; k < 6; k++) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; w[36 + rs % (len - 36)] = (uint8_t)(rs >> 24); }
        fix_sum(w, l2);
        if (start(w, l2, 9) == 0) { (void)acpidev_pci_irq(0, 1, 0, 1, &irq); acpidev_power_refresh(); (void)acpidev_pic_mode(0); }
    }
    free(w); free(tbl);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "tests/data/acpi";
    char p[256];
    snprintf(p, sizeof p, "%s/pc_dsdt.aml", dir); one(p, 0);
    snprintf(p, sizeof p, "%s/q35_dsdt.aml", dir); one(p, 1);
    free(G_mem);
    printf("acpi_qemu_test: PASS\n");
    return 0;
}
