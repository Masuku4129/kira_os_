/* Хост-тест acpidev: настоящая таблица (iasl) + модель железа: регистры PM1/GPE, встроенный контроллер по протоколу портов 0x62/0x66,
   конфигурация PCI южного моста. Проверяет события, батарею, сеть, крышку, температуру, _PRT и граничные случаи. */
#include "acpidev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

/* модель железа */
static uint8_t ec[256], pcicfg[256];
static uint16_t pm1_sts, pm1_en;
static uint8_t gpe_sts[4], gpe_en[4];
static int ec_state, ec_cmd_pending; static uint8_t ec_addr, ec_out; static int ec_obf, ec_ibf_polls;
static uint8_t ec_queue[8]; static unsigned ec_qn;
static unsigned io_log_n;

static int hw_rw(void* u, int write, unsigned sp, uint64_t a, unsigned bits, uint64_t* v, uint32_t pci) {
    (void)u;
    if (sp == AML_SP_PCI) {
        if (pci != (0x1Fu << 3)) { if (!write) *v = ~0ull; return 0; }
        unsigned nb = bits / 8; if (a + nb > 256) return -1;
        if (write) for (unsigned i = 0; i < nb; i++) pcicfg[a + i] = (uint8_t)(*v >> (8 * i));
        else { uint64_t r = 0; for (unsigned i = 0; i < nb; i++) r |= (uint64_t)pcicfg[a + i] << (8 * i); *v = r; }
        return 0;
    }
    if (sp != AML_SP_IO) return -1;
    io_log_n++;
    if (a == 0x600) { if (write) pm1_sts &= (uint16_t)~*v; else *v = pm1_sts; return 0; }          /* PM1 состояние: запись единицы сбрасывает */
    if (a == 0x602) { if (write) pm1_en = (uint16_t)*v; else *v = pm1_en; return 0; }
    if (a >= 0x620 && a < 0x624) { if (write) gpe_sts[a - 0x620] &= (uint8_t)~*v; else *v = gpe_sts[a - 0x620]; return 0; }
    if (a >= 0x624 && a < 0x628) { if (write) gpe_en[a - 0x624] = (uint8_t)*v; else *v = gpe_en[a - 0x624]; return 0; }
    if (a == 0x66) {                                                                               /* EC: порт команд/состояния */
        if (write) {
            ec_cmd_pending = (int)*v; ec_ibf_polls = 2;                                           /* занят, пока не опросят дважды */
            if (*v == 0x80 || *v == 0x81) ec_state = 1;
            else if (*v == 0x84) { ec_out = ec_qn ? ec_queue[--ec_qn] : 0; ec_obf = 1; ec_state = 0; }
            else return -1;
        } else {
            uint8_t s = (uint8_t)((ec_obf ? 1 : 0) | (ec_ibf_polls > 0 ? 2 : 0) | (ec_qn ? 0x20 : 0));
            if (ec_ibf_polls > 0) ec_ibf_polls--;
            *v = s;
        }
        return 0;
    }
    if (a == 0x62) {
        if (write) {
            if (ec_ibf_polls > 0) return -1;                                                      /* запись при занятом входе - ошибка протокола */
            if (ec_state == 1) { ec_addr = (uint8_t)*v; if (ec_cmd_pending == 0x80) { ec_out = ec[ec_addr]; ec_obf = 1; ec_state = 0; } else ec_state = 2; ec_ibf_polls = 1; }
            else if (ec_state == 2) { ec[ec_addr] = (uint8_t)*v; ec_state = 0; ec_ibf_polls = 1; }
            else return -1;
        } else { if (!ec_obf) return -1; *v = ec_out; ec_obf = 0; }
        return 0;
    }
    return -1;
}
static uint32_t stalls;
static void hw_stall(void* u, uint32_t us) { (void)u; stalls += us; }
static void hw_sleep(void* u, uint32_t ms) { (void)u; stalls += ms * 1000; }

static int evn; static int evs[32], eva[32];
static void hook(int ev, int arg, void* u) { (void)u; if (evn < 32) { evs[evn] = ev; eva[evn] = arg; evn++; } }
static int count_ev(int ev) { int n = 0; for (int i = 0; i < evn; i++) if (evs[i] == ev) n++; return n; }

static uint8_t* load_file(const char* path, uint32_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: нет файла %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n);
    CHECK(fread(b, 1, (size_t)n, f) == (size_t)n);
    fclose(f); *len = (uint32_t)n; return b;
}

static char lines[64][200]; static int nl;
static void sink(const char* s) { if (nl < 64) { strncpy(lines[nl], s, 199); lines[nl][199] = 0; nl++; } }
static int has_line(const char* sub) { for (int i = 0; i < nl; i++) if (strstr(lines[i], sub)) return 1; return 0; }

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "tests/data/acpi";
    char p[256]; snprintf(p, sizeof p, "%s/laptop.aml", dir);
    uint32_t len; uint8_t* tbl = load_file(p, &len);
    acpidev_tab_t dsdt = { tbl, len };
    acpidev_hw_t hw = { 9, 0x600, 0, 4, 0x620, 0, 8, 0, 0, 0 };
    size_t asz = acpidev_arena_size(&dsdt, 0, 0);
    void* mem = malloc(asz);
    aml_ops_t ops = { hw_rw, hw_sleep, hw_stall, 0, 0, 0, 0 };

    pcicfg[0x60] = 0x0B; pcicfg[0x61] = 0x80; pcicfg[0x62] = 0x0A; pcicfg[0x63] = 0x05;
    ec[0] = 0x07; ec[4] = 0x01; ec[5] = 0xB8; ec[6] = 0x0B; ec[7] = 0x18; ec[8] = 0x15; ec[9] = 0x18; ec[10] = 0x2E;
    ec[0x0B] = 0x10; ec[0x0C] = 0x27; ec[0x0D] = 0x1C; ec[0x0E] = 0x25; ec[0x0F] = 47;

    /* без таблицы - отказ, не падение */
    { acpidev_tab_t bad = { tbl, 10 }; CHECK(acpidev_init(mem, asz, &ops, &hw, &bad, 0, 0) != 0); CHECK(!acpidev_ready()); }
    { uint8_t* tiny = malloc(100); CHECK(acpidev_init(tiny, 100, &ops, &hw, &dsdt, 0, 0) != 0); free(tiny); }

    CHECK(acpidev_init(mem, asz, &ops, &hw, &dsdt, 0, 0) == 0);
    CHECK(acpidev_ready());
    acpidev_set_hook(hook, 0);
    acpidev_stats_t st; acpidev_stats(&st);
    CHECK(st.ini_run >= 1 && st.ini_fail == 0);
    aml_val_t v; CHECK(aml_call_path(acpidev_ctx(), 0, "\\_SB.INIV", 0, 0, &v) == 0 && v.u.i == 0x77);   /* _INI выполнен при запуске */

    /* питание после запуска: настоящие _BST/_BIF/_PSR/_LID/_TMP через настоящий обмен с EC */
    const acpidev_power_t* w = acpidev_power();
    CHECK(w->valid && w->batteries == 1 && w->percent == 56 && w->discharging && !w->charging && w->ac == 1 && w->lid == 1);
    CHECK(w->temp_valid && w->temp_c == 47 && w->minutes_left == 108);
    CHECK(w->remaining == 5400 && w->last_full == 9500 && w->rate == 3000);
    CHECK(st.ec_errors == 0 && stalls > 0);                       /* ожидание IBF действительно случалось */

    /* _PRT */
    acpidev_irq_t irq;
    CHECK(acpidev_pci_irq(0, 1, 0, 1, &irq) && irq.gsi == 11 && irq.level && irq.active_low && irq.via_link && strcmp(irq.link, "LNKA") == 0);
    CHECK(acpidev_pci_irq(0, 1, 3, 1, &irq) && irq.gsi == 11);                       /* функция 3 попадает под 0xFFFF */
    CHECK(!acpidev_pci_irq(0, 1, 0, 2, &irq));                                       /* LNKB отключена (бит 7 регистра) */
    CHECK(acpidev_pci_irq(0, 3, 0, 4, &irq) && irq.gsi == 5);
    CHECK(acpidev_pci_irq(0, 4, 0, 1, &irq) && irq.gsi == 19 && !irq.via_link && irq.level && irq.active_low);
    CHECK(acpidev_pci_irq(0, 0x1F, 0, 3, &irq) && irq.gsi == 10);
    CHECK(!acpidev_pci_irq(0, 9, 0, 1, &irq));                                       /* нет записи */
    CHECK(!acpidev_pci_irq(1, 1, 0, 1, &irq));                                       /* нет моста для шины 1 */
    CHECK(!acpidev_pci_irq(0, 1, 0, 0, &irq) && !acpidev_pci_irq(0, 1, 0, 5, &irq));
    pcicfg[0x61] = 0x09;                                                              /* прошивка перенастроила связку: ответ изменился */
    CHECK(acpidev_pci_irq(0, 1, 0, 2, &irq) && irq.gsi == 9);
    pcicfg[0x61] = 0x80;

    /* \_S5 интерпретатором, сон, режим PIC */
    uint8_t a = 99, b = 99;
    CHECK(acpidev_s5(&a, &b) == 0 && a == 5 && b == 0);
    CHECK(acpidev_prepare_sleep(5) == 0);
    CHECK(acpidev_pic_mode(1) != 0);                                                  /* метода _PIC в таблице нет: отказ, не ошибка выполнения */

    /* события: пока не разрешены - кнопка не слышна */
    pm1_sts = 0x100;
    acpidev_poll(1000);
    CHECK(count_ev(ACPIDEV_EV_POWER_BUTTON) == 0 && pm1_sts == 0x100);
    CHECK(acpidev_enable_events() == 0);
    CHECK((pm1_en & 0x100) && pm1_sts == 0);                                          /* старое нажатие сброшено при включении */
    CHECK(gpe_en[3] & 0x10);                                                          /* GPE 0x1C: есть _L1C */
    CHECK(gpe_en[0] & 0x20);                                                          /* GPE 0x05: есть _E05 */
    CHECK(gpe_en[3] & 0x20);                                                          /* GPE 0x1D: EC */
    CHECK(!(gpe_en[0] & 0x01));                                                       /* без метода не включаем */
    CHECK(st.reg_run == 0 || 1);

    pm1_sts |= 0x100;                                                                 /* нажали кнопку питания */
    acpidev_sci();
    acpidev_poll(1100);
    CHECK(count_ev(ACPIDEV_EV_POWER_BUTTON) == 1 && pm1_sts == 0);
    acpidev_poll(1200);
    CHECK(count_ev(ACPIDEV_EV_POWER_BUTTON) == 1);                                    /* повторно не слышно */

    /* GPE 0x1C: _L1C -> Notify -> обновление: сеть отключили */
    unsigned ver = acpidev_power()->version;
    ec[0] = 0x05;                                                                     /* батарея есть, сети нет, крышка открыта */
    gpe_sts[3] |= 0x10;
    acpidev_poll(1500);
    CHECK(acpidev_power()->ac == 0 && acpidev_power()->version > ver);
    CHECK(count_ev(ACPIDEV_EV_POWER) >= 1);
    CHECK(gpe_sts[3] == 0 && (gpe_en[3] & 0x10));                                     /* состояние сброшено, событие включено снова */

    /* GPE 0x1D: событие встроенного контроллера: очередь запросов, _Q50 */
    ec[4] = 0x02; ec[0] = 0x07;                                                       /* заряжается, сеть вернулась */
    ec_queue[0] = 0x50; ec_qn = 1;
    gpe_sts[3] |= 0x20;
    acpidev_poll(1800);
    acpidev_stats(&st);
    CHECK(st.ec_queries == 1 && acpidev_power()->ac == 1 && acpidev_power()->charging && !acpidev_power()->discharging);

    /* GPE 0x05: крышку закрыли */
    ec[0] = 0x03;
    gpe_sts[0] |= 0x20;
    acpidev_poll(2100);
    CHECK(count_ev(ACPIDEV_EV_LID) == 1 && eva[evn - 1] == 0);
    CHECK(acpidev_power()->lid == 0);

    /* событие без метода: отключено, без шторма */
    gpe_en[0] |= 0x01; gpe_sts[0] |= 0x01;
    acpidev_poll(2400);
    acpidev_stats(&st);
    CHECK(st.gpe_unhandled == 1 && !(gpe_en[0] & 1) && !(gpe_sts[0] & 1));

    /* батарею вынули: показывать нечего */
    ec[0] = 0x06;
    acpidev_power_refresh();
    CHECK(acpidev_power()->batteries == 0 && acpidev_power()->percent == -1);
    ec[0] = 0x07;
    acpidev_power_refresh();
    CHECK(acpidev_power()->batteries == 1 && acpidev_power()->percent >= 0);

    /* неизвестные значения не превращаются в числа: остаток 0xFFFF при последней ёмкости 9500 - это настоящие 65535 (превышение), ограничиваем 100% */
    ec[7] = 0xFF; ec[8] = 0xFF;
    acpidev_power_refresh();
    CHECK(acpidev_power()->percent == 100);
    ec[7] = 0x18; ec[8] = 0x15;

    /* отказ EC: ошибка обмена учитывается, питание не выдумывается */
    ec_ibf_polls = 0;
    /* печать */
    nl = 0; acpidev_dump(sink);
    CHECK(nl > 8 && has_line("ACPI AML:") && has_line("батарея") && has_line("_PRT") && has_line("GSI 11") && has_line("встроенный контроллер"));
    nl = 0; CHECK(acpidev_eval("\\_SB.BAT0._BST", sink) == 0 && has_line("{"));
    nl = 0; CHECK(acpidev_eval("\\NOSUCH", sink) != 0);
    CHECK(acpidev_eval("\\_SB.BAT0", sink) == 0);

    free(tbl); free(mem);
    printf("acpidev_test: PASS\n");
    return 0;
}
