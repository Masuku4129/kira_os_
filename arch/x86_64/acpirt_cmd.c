/* x86-64: команда консоли acpi (сводка, список таблиц, выгрузка таблиц, вычисление объекта AML). Выгрузка идёт в журнал в виде
   строк "ACPIDUMP BEGIN имя длина" ... "ACPIDUMP END имя": так настоящие DSDT/SSDT из QEMU попадают в тесты на хосте
   (tests/data/acpi, tools/acpi_dump_to_bin.py). */
#include "acpirt.h"
#include "../../acpi.h"
#include "../../acpidev.h"
#include "../../uart.h"

static int word_is(const char* s, const char* w) { while (*w) if (*s++ != *w++) return 0; return *s == 0 || *s == ' '; }
static const char* skip_word(const char* s) { while (*s && *s != ' ') s++; while (*s == ' ') s++; return s; }
static void sink_print(const char* s) { uart_print(s); uart_print("\n"); }

static void hex_line(const uint8_t* p, unsigned n) {
    static const char H[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) { uart_putchar(H[p[i] >> 4]); uart_putchar(H[p[i] & 15]); }
    uart_print("\n");
}
static void dump_table(const char* name, uint64_t phys, uint32_t len) {
    const uint8_t* p = acpi_table_view(phys, len);
    if (!p) { uart_print("таблица "); uart_print(name); uart_print(" недоступна\n"); return; }
    uart_print("ACPIDUMP BEGIN "); uart_print(name); uart_print(" "); uart_print_dec(len); uart_print("\n");
    for (uint32_t o = 0; o < len; o += 32) hex_line(p + o, len - o < 32 ? len - o : 32);
    uart_print("ACPIDUMP END "); uart_print(name); uart_print("\n");
}

void acpirt_cmd(const char* a) {
    while (a && *a == ' ') a++;
    const acpi_info_t* ai = acpi_get();
    if (!a || !*a) {
        if (!ai || !ai->valid) uart_print("ACPI: таблиц нет\n");
        acpirt_dump();
        uart_print("acpi tables | acpi dump dsdt|ssdtN | acpi eval \\_SB.ПУТЬ\n");
        return;
    }
    if (word_is(a, "tables")) {
        if (!ai) return;
        uart_print("DSDT: ");
        if (ai->dsdt_phys) { uart_print_hex(ai->dsdt_phys); uart_print(", "); uart_print_dec(ai->dsdt_len); uart_print(" байт\n"); } else uart_print("нет\n");
        for (unsigned i = 0; i < ai->ssdt_count && i < ACPI_MAX_SSDT; i++) {
            uart_print("SSDT"); uart_print_dec(i); uart_print(": "); uart_print_hex(ai->ssdt_phys[i]); uart_print(", "); uart_print_dec(ai->ssdt_len[i]); uart_print(" байт\n");
        }
        uart_print("SCI IRQ "); uart_print_dec(ai->sci_int); uart_print(", PM1a_EVT "); uart_print_hex(ai->pm1a_evt); uart_print(", PM1a_CNT "); uart_print_hex(ai->pm1a_cnt);
        uart_print(", GPE0 "); uart_print_hex(ai->gpe0_blk); uart_print(", флаги FADT "); uart_print_hex(ai->fadt_flags); uart_print("\n");
        return;
    }
    if (word_is(a, "dump")) {
        const char* w = skip_word(a);
        if (!ai) return;
        if (word_is(w, "dsdt")) { if (ai->dsdt_phys) dump_table("DSDT", ai->dsdt_phys, ai->dsdt_len); else uart_print("DSDT нет\n"); return; }
        if (w[0] == 's' && w[1] == 's' && w[2] == 'd' && w[3] == 't') {
            unsigned n = 0; const char* d = w + 4;
            while (*d >= '0' && *d <= '9' && n < 1000) n = n * 10 + (unsigned)(*d++ - '0');
            if (n < ai->ssdt_count && n < ACPI_MAX_SSDT) {
                char nm[12] = "SSDT"; unsigned k = 4;
                if (n >= 10) nm[k++] = (char)('0' + n / 10);
                nm[k++] = (char)('0' + n % 10); nm[k] = 0;
                dump_table(nm, ai->ssdt_phys[n], ai->ssdt_len[n]);
            } else uart_print("такой SSDT нет\n");
            return;
        }
        uart_print("acpi dump dsdt | acpi dump ssdtN\n");
        return;
    }
    if (word_is(a, "eval")) {
        const char* p = skip_word(a);
        if (!*p) { uart_print("acpi eval ПУТЬ (например \\_SB.PCI0._PRT)\n"); return; }
        if (!acpirt_up()) { uart_print("ACPI: интерпретатор AML не запущен\n"); return; }
        int rc = acpidev_eval(p, sink_print);
        if (rc != 0) { uart_print("ошибка вычисления, код "); uart_print_dec((uint64_t)(rc < 0 ? -rc : rc)); uart_print("\n"); }
        return;
    }
    uart_print("acpi: неизвестная подкоманда (tables, dump, eval)\n");
}
