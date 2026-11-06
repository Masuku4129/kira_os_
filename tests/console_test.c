// Командная строка консоли: ввод накапливается и выполняется по Enter, Backspace, история, clear.
// Все внешние службы ядра заменены заглушками; UART и клавиатура - сценарий символов.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "console.h"
#include "hwprofile.h"
#include "input.h"
#include "proc.h"
#include "progs.h"
#include "life.h"
#include "mind_sys.h"
#include "heap.h"
#include "immune.h"
#include "sched.h"
#include "fscmd.h"
#include "pci.h"
#include "blk.h"
void smp_describe(void (*out)(const char*)) { out("smp stub\n"); }
int smp_selftest_start(void) { return -1; }
char kernel_start[4096];
unsigned smp_online(void) { return 4; }
unsigned proc_limit(void) { return 8; }
int smp_crash_test(unsigned mode, unsigned cpu) { (void)mode; (void)cpu; return -1; }
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static char out[65536]; static size_t outn;
static char serial[4096]; static size_t sern;
static const char* script; static size_t spos;       // символы "из UART"
static int spawned = -1, saved, tele;
static void emit(const char* s) { size_t n = strlen(s); if (outn + n < sizeof out) { memcpy(out + outn, s, n); outn += n; out[outn] = 0; } }

void pci_print(void) {}
void usb_print(void) { emit("[USB]\n"); }
void usb_hid_allow_all(void) { emit("[ALLOW]\n"); }
blk_dev_t* blk_primary(void) { return 0; }
void blk_print(void) { emit("[DISKS]\n"); }
static char fsarg[96]; static const char* fscall;
#define fsclr() (fsarg[0] = 0)
#define FSSTUB(n) void n(const char* a) { fscall = #n; fsarg[0] = 0; strncpy(fsarg, a, sizeof fsarg - 1); }
FSSTUB(fscmd_ls) FSSTUB(fscmd_cat) FSSTUB(fscmd_write) FSSTUB(fscmd_rm) FSSTUB(fscmd_mkdir) FSSTUB(fscmd_df) FSSTUB(fscmd_format) FSSTUB(fscmd_cp) FSSTUB(fscmd_umount)
void uart_print(const char* s) { emit(s); }
void uart_putchar(char c) { char b[2] = { c, 0 }; emit(b); }
void uart_print_serial(const char* s) { size_t n = strlen(s); memcpy(serial + sern, s, n); sern += n; }
void uart_print_dec(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); emit(b); }
static long flood;   /* >0: «порт» без устройства отдаёт 0xFF без конца */
int uart_getchar(void) { if (flood) { flood++; return 0xFF; } return script && script[spos] ? (unsigned char)script[spos++] : -1; }
static input_event_t evq[16]; static int evh, evt;
int input_pop(input_event_t* ev) { if (evt == evh) return 0; *ev = evq[evt++ % 16]; return 1; }
unsigned input_raw_take(uint8_t* out, unsigned max) { (void)out; (void)max; return 0; }
static void key(int k, int ch) { evq[evh++ % 16] = (input_event_t){ INPUT_KEY_DOWN, (uint16_t)k, (uint16_t)ch, 0 }; }
int gfx_focus_pid(void) { return 0; }
const char* spawned_name;
/* на диске есть только cube3d (и программы, запускаемые явными командами); остальные имена - нет файла */
int prog_run(const char* n) {
    spawned_name = n; spawned = 1;
    const char* known[] = { "plasma", "paint", "burn", "cube3d", "fpu" };
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++) if (!strcmp(n, known[i])) return 7;
    return -1;
}
static char cc_args[64], local_file[64];
int prog_run_args(const char* n, const char* a) {
    if (!strcmp(n, "kcc")) { snprintf(cc_args, sizeof cc_args, "%s", a); return 5; }
    return prog_run(n);
}
int prog_run_local(const char* f) { snprintf(local_file, sizeof local_file, "%s", f); return 6; }
void kdrv_cmd(const char* a) { (void)a; }
void prog_list(void) {}
void platform_status(void (*out)(const char*)) { out("IRQ stub\n"); }
void platform_hw(void (*out)(const char*)) { hw_describe(hw_get(), out); }
int platform_console_cmd(const char* c, const char* a) { (void)c; (void)a; return 0; }
void platform_poweroff(void) {}
void platform_reboot(void) {}
int rng_get(void* o, unsigned long n) { for (unsigned long i = 0; i < n; i++) ((unsigned char*)o)[i] = (unsigned char)i; return 0; }
const char* rng_sources(void) { return "test"; }
int audio_master(int p) { return p < 0 ? 100 : p; }
void audiocmd_record(const char* a) { (void)a; }
void audiocmd_sound(const char* a) { (void)a; }
void audiocmd_beep(const char* a) { (void)a; }
int blk_flush_all(void) { return 0; }
int vfs_flush(int n) { (void)n; return 0; }
void prog_request_file(const char* n, const char* f) { (void)n; (void)f; }
void life_print(void) {}
void display_print_info(void) { emit("[FB] info\n"); }
static char rf_l[160], rf_r[160]; static int rf_n;
void display_request_files(const char* l, const char* r) { snprintf(rf_l, sizeof rf_l, "%s", l); snprintf(rf_r, sizeof rf_r, "%s", r); rf_n++; }
static int kbd_owner;                                   /* 1 - клавиатуру читает Проводник */
int display_kbd_owner(void) { return kbd_owner; }
int experience_save_now(void) { saved++; return 0; }
void experience_print(void) { emit("[EXP]\n"); }
void sched_dump(void) { emit("[PS]\n"); }
void mind_sys_set_telemetry(int on) { tele = on; }
int mind_sys_telemetry(void) { return tele; }
static char mind_req[32];
void mind_sys_print(const char* w) { snprintf(mind_req, sizeof mind_req, "print:%s", w); emit("[MIND]\n"); }
void mind_sys_request_mode(int m) { snprintf(mind_req, sizeof mind_req, "mode:%d", m); }
void mind_sys_request_cap(int p) { snprintf(mind_req, sizeof mind_req, "cap:%d", p); }
unsigned mind_sys_core_cap(void) { return 50; }
void mind_sys_request_fx(int f) { snprintf(mind_req, sizeof mind_req, "fx:%d", f); }
void mind_sys_view(mind_view_t* v) { memset(v, 0, sizeof *v); v->health = 91; strcpy(v->mood, "спокойна"); v->mode = 1; v->quantum_ms = 30; }
uint64_t timer_get_ticks(void) { return 0; }
uint64_t clock_ms(void) { return 0; }
int clock_ready(void) { return 0; }
int apic_is_enabled(void) { return 0; }
uint32_t apic_get_id(void) { return 0; }
void heap_get_stats(heap_stats_t* h) { memset(h, 0, sizeof *h); }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
uint32_t pmm_get_free_memory_kb(void) { return 0; }
uint32_t pmm_get_total_memory_kb(void) { return 0; }
uint64_t pmm_alloc_page(void) { return 0; }
int pmm_free_page(uint64_t p) { (void)p; return -1; }
int vmm_translate(uint64_t v, uint64_t* p) { (void)v; (void)p; return -1; }
int immune_verify_self(void) { return 0; }
uint32_t immune_antibody_count(void) { return 0; }
uint32_t immune_quarantine_count(void) { return 0; }
immune_verdict_t immune_classify(const void* b, size_t n) { (void)b; (void)n; return IMMUNE_UNKNOWN; }
void immune_phagocytose(void* b, size_t n) { (void)b; (void)n; }
int immune_register_self(const uint8_t d[32]) { (void)d; return 0; }
void sha256(const void* d, size_t n, uint8_t o[32]) { (void)d; (void)n; (void)o; }
thread_t* sched_create(const char* n, void (*e)(void*), void* a, thread_prio_t p) { (void)n; (void)e; (void)a; (void)p; return 0; }
const thread_t* sched_current(void) { return 0; }

static void feed(const char* s) { script = s; spos = 0; console_poll(); }
static int has(const char* s) { return strstr(out, s) != 0; }
static void reset(void) { outn = 0; out[0] = 0; sern = 0; serial[0] = 0; }

int main(void) {
    feed("");                                           /* первый вызов показывает приглашение */
    CHECK(has("кира> "));
    /* hw: профиль железа печатается командой, без запуска программ */
    hw_init(4, 8ULL * 1024 * 1024);
    spawned = -1; reset(); feed("hw\n");
    CHECK(has("[KIRA HW] CPU") && has("cores (ACPI): 4, RAM 8192 MB") && has("program heap up to 4096 MB") && spawned == -1);
    /* команда без Enter не выполняется: однобуквенный ввод больше не запускает ничего */
    reset(); feed("a");
    CHECK(spawned == -1 && has("a") && !has("[PROC]"));
    /* Enter выполняет: "a" = plasma */
    reset(); feed("\n");
    CHECK(spawned_name && !strcmp(spawned_name, "plasma"));
    /* полное имя и аргументы, пробелы вокруг */
    spawned = -1; spawned_name = 0; reset(); feed("  paint  \n");
    CHECK(spawned_name && !strcmp(spawned_name, "paint"));
    /* files: Проводник на двух путях; без аргументов - пути не меняются */
    reset(); feed("files /fm-a   /fm-b\n");
    CHECK(rf_n == 1 && !strcmp(rf_l, "/fm-a") && !strcmp(rf_r, "/fm-b") && has("[KIRA FILES] explorer: /fm-a | /fm-b"));
    reset(); feed("files\n");
    CHECK(rf_n == 2 && !rf_l[0] && !rf_r[0]);
    /* Backspace стирает символ до Enter: "plasmx" -> "plasm" + "a" */
    spawned = -1; spawned_name = 0; reset(); feed("plasmx\b");
    CHECK(has("\b \b"));
    feed("a\n");
    CHECK(spawned_name && !strcmp(spawned_name, "plasma"));
    /* неизвестная команда */
    /* cc и runlocal: два слова / одно слово, иначе справка и ничего не запускается */
    reset(); feed("cc hello.c hello\n");
    CHECK(!strcmp(cc_args, "hello.c hello"));
    cc_args[0] = 0; reset(); feed("cc hello.c\n");
    CHECK(!cc_args[0] && has("использование: cc исходник результат"));
    reset(); feed("cc a b c\n");
    CHECK(!cc_args[0] && has("использование: cc"));
    reset(); feed("runlocal hello\n");
    CHECK(!strcmp(local_file, "hello"));
    local_file[0] = 0; reset(); feed("runlocal\n");
    CHECK(!local_file[0] && has("использование: runlocal"));
    reset(); feed("runlocal a b\n");
    CHECK(!local_file[0] && has("использование: runlocal"));

    reset(); feed("blah\n");
    CHECK(has("неизвестная команда: blah"));
    /* имя программы с диска запускает её без run (как в обычной оболочке) */
    spawned = -1; spawned_name = 0; reset(); feed("cube3d\n");
    CHECK(spawned_name && !strcmp(spawned_name, "cube3d") && !has("неизвестная команда"));
    /* опасные тесты только полным именем: "d" и "n" - неизвестны, а не падение системы */
    reset(); feed("d\nn\nu\nW\n");
    CHECK(has("неизвестная команда: d") && has("неизвестная команда: n") && has("неизвестная команда: u") && has("неизвестная команда: W"));
    /* справка перечисляет команды и подсказывает про клавиши */
    reset(); feed("help\n");
    CHECK(has("clear") && has("paint (b)") && has("test (t)") && has("Ctrl+L") && !has("panic-df"));
    /* проверки системы - отдельным списком под test; опасные только полным именем */
    reset(); feed("test\n");
    CHECK(has("panic-df") && has("isolation") && has("crash"));
    spawned = -1; spawned_name = 0; reset(); feed("test fpu\n");
    CHECK(spawned_name && !strcmp(spawned_name, "fpu"));
    reset(); feed("test panic\n"); CHECK(has("нет такой проверки: panic"));
    reset(); feed("test fp\n"); CHECK(has("нет такой проверки: fp"));
    /* clear: управляющий символ экрана в журнале + ANSI в serial, после него новое приглашение */
    reset(); feed("clear\n");
    CHECK(strchr(out, '\f') != 0 && strstr(serial, "\x1b[2J") != 0);
    CHECK(strstr(strchr(out, '\f'), "кира> ") != 0);
    /* Ctrl+L очищает, а набранная строка остаётся */
    reset(); feed("pai\x0c");
    CHECK(strchr(out, '\f') != 0);
    CHECK(strstr(strchr(out, '\f'), "кира> pai") != 0);
    spawned = -1; spawned_name = 0; feed("nt\n");
    CHECK(spawned_name && !strcmp(spawned_name, "paint"));
    /* CR LF - одна команда, а не две */
    reset(); feed("save\r\n");
    CHECK(saved == 1);
    size_t prompts = 0; for (const char* p = out; (p = strstr(p, "кира> ")); p += 8) prompts++;
    CHECK(prompts == 1);
    /* telemetry on/off */
    reset(); feed("telemetry on\n"); CHECK(tele == 1 && has("включена"));
    reset(); feed("telemetry off\n"); CHECK(tele == 0 && has("выключена"));
    reset(); feed("telemetry maybe\n"); CHECK(has("использование"));
    /* COM1 нет, а чтение даёт 0xFF без конца: console_poll всё равно возвращается (сторож не сработает) */
    flood = 1; console_poll(); CHECK(flood > 1 && flood < 2000); flood = 0;
    /* Разум */
    reset(); feed("ai why\n"); CHECK(!strcmp(mind_req, "print:why"));
    reset(); feed("ai forecast\n"); CHECK(!strcmp(mind_req, "print:forecast"));
    reset(); feed("ai observe\n"); CHECK(!strcmp(mind_req, "mode:0"));
    reset(); feed("ai act\n"); CHECK(!strcmp(mind_req, "mode:1"));
    reset(); feed("ai fx auto\n"); CHECK(!strcmp(mind_req, "fx:4"));
    reset(); feed("ai fx 2\n"); CHECK(!strcmp(mind_req, "fx:2"));
    reset(); feed("ai fx 7\n"); CHECK(has("использование"));
    reset(); feed("status\n"); CHECK(has("health 91") && has("спокойна"));
    /* команда от оболочки: эхо после приглашения, выполнение, недописанная строка возвращается */
    reset(); feed("pin");
    CHECK(console_request("ai why") == 0);
    reset(); feed("");
    CHECK(!strcmp(mind_req, "print:why") && has("ai why\n") && has("pin"));
    for (int i = 0; i < 4; i++) CHECK(console_request("ai act") == 0);
    CHECK(console_request("ai act") == -1);                                  /* очередь из 4 */
    feed("\x15");
    /* API очереди не должен принимать NULL или молча обрезать длинные команды. */
    CHECK(console_request(0) == -1);
    char oversized[80]; memset(oversized, 'a', sizeof oversized - 1); oversized[sizeof oversized - 1] = 0;
    CHECK(console_request(oversized) == -1);
    CHECK(console_request("ai why") == 0);
    reset(); feed("");
    CHECK(!strcmp(mind_req, "print:why"));
    /* Ctrl+C отменяет строку */
    spawned = -1; spawned_name = 0; reset(); feed("plasma\x03\n");
    CHECK(spawned == -1 && has("^C"));
    /* Ctrl+U стирает всю строку */
    reset(); feed("plasma\x15paint\n");
    CHECK(spawned_name && !strcmp(spawned_name, "paint"));
    /* длинная строка не переполняет буфер */
    char longl[400]; memset(longl, 'x', sizeof longl - 2); longl[sizeof longl - 2] = '\n'; longl[sizeof longl - 1] = 0;
    reset(); feed(longl);
    CHECK(has("неизвестная команда: xxx"));
    /* история: стрелка вверх возвращает прошлые команды, вниз - обратно, Enter выполняет */
    feed("plasma\n"); feed("paint\n");
    spawned = -1; spawned_name = 0; reset();
    key(KEY_UP, 0); feed("");
    CHECK(has("paint"));
    key(KEY_UP, 0); feed("");
    CHECK(has("plasma"));
    spawned = -1; spawned_name = 0; key(KEY_ENTER, '\n'); feed("");
    CHECK(spawned_name && !strcmp(spawned_name, "plasma"));
    key(KEY_UP, 0); key(KEY_UP, 0); key(KEY_UP, 0); key(KEY_DOWN, 0); feed("");   /* plasma, paint, plasma ... и вниз: paint */
    spawned = -1; spawned_name = 0; key(KEY_ENTER, '\r'); feed("");
    CHECK(spawned_name && !strcmp(spawned_name, "paint"));
    key(KEY_UP, 0); key(KEY_DOWN, 0); feed("");                      /* вниз за пределом истории - пустая строка */
    spawned = -1; spawned_name = 0; key(KEY_ENTER, '\n'); feed("");
    CHECK(spawned == -1 && !spawned_name);
    reset(); fscall = 0; feed("write /a.txt hello world\r");
    CHECK(fscall && !strcmp(fscall, "fscmd_write") && !strcmp(fsarg, "/a.txt hello world"));
    reset(); fscall = 0; feed("ls\r"); CHECK(fscall && !strcmp(fscall, "fscmd_ls") && !fsarg[0]);
    /* каждая команда и каждый псевдоним из help находятся в таблице и выполняются (опасные тесты, останавливающие систему, пропускаются) */
    {
        static char helptext[16384];
        reset(); feed("help\n");
        strncpy(helptext, out, sizeof helptext - 1);
        int ran = 0, aliases = 0;
        for (char* ln = helptext; ln && *ln; ) {
            char* nl = strchr(ln, '\n'); if (nl) *nl = 0;
            if (ln[0] == ' ' && ln[1] == ' ' && ln[2] && ln[2] != ' ') {
                char name[32]; unsigned n = 0; const char* p = ln + 2;
                while (*p && *p != ' ' && n < sizeof name - 1) name[n++] = *p++;
                name[n] = 0;
                char alias = 0;
                if (p[0] == ' ' && p[1] == '(' && p[3] == ')') alias = p[2];
                if (strncmp(name, "crash-", 6) && strcmp(name, "starve")) {
                    char cmd[48]; snprintf(cmd, sizeof cmd, "%s\n", name);
                    reset(); feed(cmd); CHECK(!has("неизвестная команда")); ran++;
                    if (alias && alias != '?') { snprintf(cmd, sizeof cmd, "%c\n", alias); reset(); feed(cmd); CHECK(!has("неизвестная команда")); aliases++; }
                }
            }
            ln = nl ? nl + 1 : 0;
        }
        CHECK(ran >= 25 && aliases >= 10);
        printf("console: %d commands and %d aliases from help all resolve and run\n", ran, aliases);
    }
    printf("console: Enter-driven command line, backspace, history, clear, Ctrl+L/C/U, telemetry switch, ai commands passed\n");
    return 0;
}
