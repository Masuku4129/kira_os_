/* Драйверы (/bin/drvman): окно управления драйверами Ring 3 (docs/DRIVERS.md). Программа пользовательского уровня; ядро ничего не рисует.
   1) устройства PCI и кто их занимает - ТОЛЬКО ЧТЕНИЕ списка ядра (KDRV_SYS_PCILIST, право CAP_PCILIST): встроенный драйвер ядра /
      драйвер Ring 3 drv-ИМЯ запущен / пакет установлен, но не запущен / драйвера нет;
   2) пакеты из /drv: версия, источник, лицензия, SHA-256, права, итог сверки хешей;
   3) поле «имя или адрес рецепта» и кнопки: «Найти и показать» (HTTPS, лицензия, хеш, права - ничего не ставит), «Установить» (только после
      нажатия «Собрать и установить» в окне подтверждения; сборка kcc на устройстве, запись в /drv), «Удалить», «Запустить», «Остановить».
   Логика скачивания, проверки и установки - тот же userland/drvget.c (собран с -DDRVGET_LIB), поэтому окно и консольная команда drvget
   не расходятся. Долгие шаги (сеть, компиляция) идут в рабочем потоке, окно отвечает. Ошибки показываются как есть: ничего не имитируется.
   Режим для стенда и скриптов: «drvman do КОМАНДА[;КОМАНДА...]» выполняет те же действия теми же функциями без мыши и печатает строки
   состояния (те же, что рисуются в окне): pci | pkgs | list | verify ИМЯ | base URL | find ИМЯ|URL | install | start ИМЯ | stop ИМЯ | remove ИМЯ. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "kira.h"
#include "../font.h"
#include "../kdrv_format.h"
#include "drvget_api.h"

KIRA_ENTRY()

#define W 640
#define H 560
#define CW FONT_W
#define CH FONT_H
#define BG      0x00101820u
#define PANEL   0x00182430u
#define BAR_BG  0x00203040u
#define FG      0x00E0E8F0u
#define DIM     0x008899AAu
#define ACCENT  0x0042A5F5u
#define GREEN   0x0066BB6Au
#define AMBER   0x00FFB300u
#define RED     0x00FF5252u
#define NOBG    0xFFFFFFFFu
#define DEV_ROWS 7                                        /* строк списка устройств на экране */
#define PKG_ROWS 2                                        /* пакетов на экране (по 4 строки) */

enum { KEY_ESC = 0x100, KEY_BACKSPACE, KEY_TAB, KEY_ENTER, KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT,
       KEY_RALT, KEY_CAPS, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11,
       KEY_F12, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE };
#define MOD_CTRL 2

static u32 *px;
static int have_window;

/* ---------- рисование ---------- */
static void fill(int x, int y, int w, int h, u32 c) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; i++) if (i >= 0 && i < W) px[j * W + i] = c;
    }
}
static int utf8(const char *s, u32 *cp) {
    u8 c = (u8)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && ((u8)s[1] & 0xC0) == 0x80) { *cp = (u32)(c & 0x1F) << 6 | ((u8)s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && ((u8)s[1] & 0xC0) == 0x80 && ((u8)s[2] & 0xC0) == 0x80) { *cp = (u32)(c & 0x0F) << 12 | (u32)((u8)s[1] & 0x3F) << 6 | ((u8)s[2] & 0x3F); return 3; }
    *cp = '?';
    return 1;
}
static void glyph(int x, int y, u32 cp, u32 fg, u32 bg) {
    const u8 *g = font_glyph(cp);
    for (int r = 0; r < CH; r++) {
        if (y + r < 0 || y + r >= H) continue;
        for (int c = 0; c < CW; c++) {
            if (x + c < 0 || x + c >= W) continue;
            if (g[r] & (0x80 >> c)) px[(y + r) * W + x + c] = fg;
            else if (bg != NOBG) px[(y + r) * W + x + c] = bg;
        }
    }
}
/* Строка UTF-8 с позиции x; не больше maxcols знаков. Возвращает x за последним знаком. */
static int text(int x, int y, const char *s, u32 fg, int maxcols) {
    for (int n = 0; *s && n < maxcols && x + CW <= W; n++) { u32 cp; s += utf8(s, &cp); glyph(x, y, cp, fg, NOBG); x += CW; }
    return x;
}
static int cols_of(const char *s) { int n = 0; while (*s) { u32 cp; s += utf8(s, &cp); n++; } return n; }

/* Добавить s в b[at..] и дополнить пробелами до width знаков (по знакам, а не байтам). Возвращает новую длину. */
static int addf(char *b, int cap, int at, const char *s, int width) {
    int cols = 0;
    while (*s && at + 4 < cap) { u32 cp; int k = utf8(s, &cp); for (int i = 0; i < k; i++) b[at++] = s[i]; s += k; cols++; }
    while (cols < width && at + 2 < cap) { b[at++] = ' '; cols++; }
    b[at] = 0;
    return at;
}

/* ---------- журнал (панель внизу и последовательный порт) ---------- */
#define LOG_LINES 160
#define LOG_COLS 76
static char loglines[LOG_LINES][LOG_COLS * 3 + 1];
static unsigned log_n, log_cols, log_len;                 /* строк завершено; знаков и байт в текущей */
static int log_scroll;                                     /* на сколько строк прокручено вверх от конца */
static volatile u32 lock;
static volatile int dirty = 1;

static void log_put(const char *s, size_t n) {
    k_mutex_lock(&lock);
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        char *ln = loglines[log_n % LOG_LINES];
        if (c == '\r') continue;
        if (c == '\n' || (log_cols >= LOG_COLS && (c & 0xC0) != 0x80)) {
            ln[log_len] = 0; log_n++; log_cols = 0; log_len = 0; log_scroll = 0;
            ln = loglines[log_n % LOG_LINES];
            if (c == '\n') continue;
        }
        if (log_len + 1 >= sizeof loglines[0]) continue;
        ln[log_len++] = (char)c; ln[log_len] = 0;
        if ((c & 0xC0) != 0x80) log_cols++;
    }
    k_mutex_unlock(&lock);
    dirty = 1;
}
static void sink(const char *s, size_t n) {
    log_put(s, n);
    while (n) { size_t k = n > 2000 ? 2000 : n; k_write(s, k); s += k; n -= k; }       /* и в журнал процесса: стенд читает его */
}
static void lg(const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    sink(b, strlen(b));
}

/* ---------- состояние: устройства PCI и пакеты ---------- */
#define MAXPCI 128
#define MAXPKG 16
static kdrv_pcient_t pci[MAXPCI];
static int npci, pci_err;
static drvget_pkg_t pkg[MAXPKG];
static int npkg = -1;                                     /* -1: каталога /drv нет или он не читается */
static int sel_pkg = -1, sel_dev = -1, focus_pkgs;
static int dev_top, pkg_top;

static const char *err_name(int e) {
    switch (e) {
    case -12: return "нет права (CAP_PCILIST)";
    case -100: return "доступ запрещён";
    case -103: return "очередь менеджера драйверов занята";
    case -13: return "нет такого пакета";
    default: return "ошибка";
    }
}

static void refresh_pci(void) {
    kdrv_pcient_t tmp[MAXPCI];
    int n = 0, err = 0;
    while (n < MAXPCI) {
        int want = MAXPCI - n > KDRV_PCILIST_MAX ? KDRV_PCILIST_MAX : MAXPCI - n;
        i64 r = ksys(KDRV_SYS_PCILIST, (u64)(tmp + n), (u64)want, (u64)n);
        if (r < 0) { err = (int)r; break; }
        n += (int)r;
        if (r < want) break;
    }
    k_mutex_lock(&lock);
    memcpy(pci, tmp, sizeof(kdrv_pcient_t) * (size_t)n);
    npci = err ? 0 : n; pci_err = err;
    if (dev_top >= npci) dev_top = 0;
    k_mutex_unlock(&lock);
    dirty = 1;
}

static void refresh_pkgs(void) {                           /* не вызывать, пока идёт задача: общий буфер drvget */
    drvget_pkg_t tmp[MAXPKG];
    int n = drvget_pkgs(tmp, MAXPKG);
    k_mutex_lock(&lock);
    if (n > 0) memcpy(pkg, tmp, sizeof(drvget_pkg_t) * (size_t)n);
    npkg = n;
    if (sel_pkg >= npkg) sel_pkg = -1;
    if (pkg_top >= npkg) pkg_top = 0;
    k_mutex_unlock(&lock);
    dirty = 1;
}

static const char *class_text(u32 cls) {
    switch (cls >> 16) {
    case 0x00: return "старое";    case 0x01: return "накопитель"; case 0x02: return "сеть";
    case 0x03: return "видео";     case 0x04: return "мультимедиа"; case 0x05: return "память";
    case 0x06: return "мост";      case 0x07: return "связь";      case 0x08: return "системное";
    case 0x09: return "ввод";      case 0x0C: return "шина USB/др."; case 0x0D: return "радио";
    case 0x10: return "шифрование"; case 0x11: return "сигналы";
    default: return "другое";
    }
}

static int pkg_matches(const drvget_pkg_t *k, const kdrv_pcient_t *e) {
    kdrv_header_t th;
    if (k->state == DGP_UNREADABLE || !k->nmatch) return 0;
    memset(&th, 0, sizeof th);
    th.nmatch = k->nmatch;
    for (u32 i = 0; i < k->nmatch; i++) th.match[i] = k->match[i];
    return kdrv_match_dev(&th, e->vendor, e->device, e->cls);
}
static int running(const char *name) {                    /* работает ли драйвер Ring 3 с таким именем (по последнему списку) */
    for (int i = 0; i < npci; i++) if (pci[i].owner == KDRV_OWN_RING3 && !strcmp(pci[i].name, name)) return i + 1;
    return 0;
}
static int pkg_for_dev(const kdrv_pcient_t *e) {              /* подходящий пакет; исправный предпочтительнее испорченного */
    int first = -1;
    for (int i = 0; i < npkg; i++) {
        if (!pkg_matches(&pkg[i], e)) continue;
        if (pkg[i].state == DGP_OK) return i;
        if (first < 0) first = i;
    }
    return first;
}

/* Состояние устройства словами; col - цвет. */
static void dev_state(const kdrv_pcient_t *e, char *b, int cap, u32 *col) {
    *col = FG;
    switch (e->owner) {
    case KDRV_OWN_KERNEL: snprintf(b, (size_t)cap, "встроенный драйвер ядра"); *col = GREEN; return;
    case KDRV_OWN_BRIDGE: snprintf(b, (size_t)cap, "мост или шина, драйвер не нужен"); *col = DIM; return;
    case KDRV_OWN_RING3: snprintf(b, (size_t)cap, "драйвер Ring 3 drv-%s запущен", e->name); *col = GREEN; return;
    default: break;
    }
    int k = pkg_for_dev(e);
    if (k >= 0) { snprintf(b, (size_t)cap, "пакет %s установлен, не запущен", pkg[k].name); *col = AMBER; }
    else { snprintf(b, (size_t)cap, "драйвера нет"); *col = RED; }
}
#define DEV_LEFT_COLS (9 + 11 + 8 + 13)                  /* ширина колонок до состояния */
static void dev_parts(const kdrv_pcient_t *e, char *left, int lcap, char *st, int scap, u32 *col) {
    char t[64];
    int at = 0;
    snprintf(t, sizeof t, "%02x:%02x.%x", e->bus, e->dev, e->fn & 7);
    at = addf(left, lcap, at, t, 9);
    snprintf(t, sizeof t, "%04x:%04x", e->vendor, e->device);
    at = addf(left, lcap, at, t, 11);
    snprintf(t, sizeof t, "%06x", e->cls);
    at = addf(left, lcap, at, t, 8);
    addf(left, lcap, at, class_text(e->cls), 13);
    dev_state(e, st, scap, col);
}
static void dev_line(const kdrv_pcient_t *e, char *b, int cap) {
    char left[160], st[96];
    u32 col;
    dev_parts(e, left, sizeof left, st, sizeof st, &col);
    snprintf(b, (size_t)cap, "%s%s", left, st);
}

static void caps_str(u32 c, char *b, int cap) {
    static const struct { u32 bit; const char *n; } t[] = { { CAP_MMIO, "MMIO" }, { CAP_IRQ, "IRQ" }, { CAP_DMA, "DMA" }, { CAP_PCIDEV, "PCIDEV" }, { CAP_NETDEV, "NETDEV" } };
    int at = 0;
    b[0] = 0;
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (c & t[i].bit) at += snprintf(b + at, (size_t)(cap - at), "%s%s", at ? " " : "", t[i].n);
    if (!c) snprintf(b, (size_t)cap, "-");
}
static const char *pkg_status(const drvget_pkg_t *k) {
    switch (k->state) {
    case DGP_OK: return "SHA-256 сходится";
    case DGP_BAD: return "НЕ СОВПАДАЕТ";
    case DGP_NOREC: return "нет файла .rec";
    case DGP_UNCONF: return "не подтверждён";
    case DGP_TOOBIG: return "слишком велик";
    default: return "не читается";
    }
}
/* Четыре строки пакета: 0 - имя, версия, лицензия, работа и итог сверки; 1 - права; 2 - SHA-256 образа; 3 - источник. */
static void pkg_line(const drvget_pkg_t *k, int n, char *b, int cap) {
    char t[200], cs[80];
    int at = 0;
    if (n == 0) {
        snprintf(t, sizeof t, "%s", k->name);                at = addf(b, cap, at, t, 12);
        snprintf(t, sizeof t, "v%u", (unsigned)k->version);  at = addf(b, cap, at, t, 5);
        snprintf(t, sizeof t, "лицензия: %s", k->license[0] ? k->license : "не записана");
        at = addf(b, cap, at, t, 24);
        at = addf(b, cap, at, running(k->name) ? "ЗАПУЩЕН" : "не запущен", 11);
        snprintf(t, sizeof t, "сверка: %s", pkg_status(k));  addf(b, cap, at, t, 0);
    } else if (n == 1) {
        caps_str(k->caps, cs, sizeof cs);
        snprintf(t, sizeof t, "права запрошены: %s", cs);
        at = addf(b, cap, at, t, 44);
        caps_str(k->caps_ok, cs, sizeof cs);
        snprintf(t, sizeof t, "подтверждены: %s", k->state == DGP_NOREC || k->state == DGP_UNCONF ? "нет" : cs);
        addf(b, cap, at, t, 0);
    } else if (n == 2) {
        char hx[65];
        dg_hex(k->elf_sha, 32, hx);
        snprintf(t, sizeof t, "sha256 образа %s", hx);
        addf(b, cap, at, k->state == DGP_NOREC ? "sha256 образа -" : t, 0);
    } else {
        snprintf(t, sizeof t, "источник: %s", k->source[0] ? k->source : "не записан");
        addf(b, cap, at, t, 0);
    }
}

/* Строка журнала процесса рвётся ядром на куски по 160 байт, поэтому строки состояния короткие: пробелы выравнивания сжимаются. */
static void squeeze(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) if (!(*r == ' ' && w > s && w[-1] == ' ')) *w++ = *r;
    *w = 0;
}

/* Строки состояния для журнала: те же тексты, что рисуются в окне (так стенд проверяет именно показанное). */
static void report_pci(void) {
    char b[200];
    if (pci_err) lg("drvman: список PCI недоступен: %s (код %d)\n", err_name(pci_err), pci_err);
    else lg("drvman: устройств PCI: %d\n", npci);
    for (int i = 0; i < npci; i++) { dev_line(&pci[i], b, sizeof b); squeeze(b); lg("drvman: pci %s\n", b); }
}
static void report_pkgs(void) {
    char b[200];
    if (npkg < 0) lg("drvman: каталога /drv нет или он недоступен\n");
    else lg("drvman: пакетов в /drv: %d\n", npkg);
    for (int i = 0; i < npkg; i++) {
        for (int n = 0; n < 4; n++) { pkg_line(&pkg[i], n, b, sizeof b); squeeze(b); lg("drvman: пакет %s | %s\n", pkg[i].name, b); }
    }
}

/* ---------- действия (выполняются в рабочем потоке или, в режиме do, прямо в главном) ---------- */
static char input[160];
static int input_len;
static char job_arg[200];

static void act_find(const char *arg) {
    if (!*arg) { lg("drvman: введите имя драйвера или адрес https://.../drvget.rec\n"); return; }
    lg("drvman: ищу драйвер «%s» (скачивание по HTTPS, проверка лицензии и хеша)...\n", arg);
    int rc = drvget_find(arg);
    if (rc == 0) lg("drvman: набор готов к установке (F2 или «Установить»: окно подтверждения)\n");
    else lg("drvman: поиск не удался (код %d), ничего не установлено\n", rc);
}
static void act_install(void) {
    const dg_recipe_t *r = drvget_staged();
    if (!r) { lg("drvman: нечего устанавливать: сначала «Найти и показать»\n"); return; }
    char nm[16];
    snprintf(nm, sizeof nm, "%s", r->name);
    int rc = drvget_install_staged();
    if (rc == 0) lg("drvman: драйвер %s установлен в /drv\n", nm);
    else lg("drvman: установка %s не выполнена (код %d)\n", nm, rc);
    refresh_pkgs();
    refresh_pci();
    for (int i = 0; i < npkg; i++) if (!strcmp(pkg[i].name, nm)) { sel_pkg = i; if (i >= pkg_top + PKG_ROWS) pkg_top = i - PKG_ROWS + 1; }   /* новый пакет выбран: сразу можно запускать */
}
static void act_remove(const char *name) {
    if (!*name) { lg("drvman: выберите пакет в списке\n"); return; }
    refresh_pci();
    if (running(name)) { lg("drvman: драйвер %s сейчас работает: сначала «Остановить»\n", name); return; }
    int rc = drvget_remove(name);
    lg(rc == 0 ? "drvman: пакет %s удалён\n" : "drvman: удалить %s не удалось (код %d)\n", name, rc);
    refresh_pkgs();
}
static void act_start(const char *name) {
    if (!*name) { lg("drvman: выберите пакет в списке\n"); return; }
    refresh_pci();
    int w = running(name);
    if (w) { lg("drvman: драйвер %s уже запущен на устройстве %02x:%02x.%x\n", name, pci[w - 1].bus, pci[w - 1].dev, pci[w - 1].fn & 7); return; }
    int rc = drvget_ctl(1, name);
    if (rc < 0) { lg("drvman: запуск %s отклонён: %s (код %d)\n", name, err_name(rc), rc); return; }
    lg("drvman: просьба запустить %s принята менеджером драйверов ядра, жду...\n", name);
    u64 until = k_clock_ns() + 6000000000ull;
    do { k_sleep(10); refresh_pci(); w = running(name); } while (!w && k_clock_ns() < until);
    if (w) lg("drvman: драйвер %s запущен на устройстве %02x:%02x.%x (%04x:%04x)\n", name, pci[w - 1].bus, pci[w - 1].dev, pci[w - 1].fn & 7, pci[w - 1].vendor, pci[w - 1].device);
    else {
        lg("drvman: драйвер %s не запущен: нет свободного подходящего устройства PCI\n", name);
        lg("drvman: либо пакет не прошёл проверку ядра: причина в журнале ядра (команда drv)\n");
    }
}
static void act_stop(const char *name) {
    if (!*name) { lg("drvman: выберите пакет в списке\n"); return; }
    refresh_pci();
    if (!running(name)) { lg("drvman: драйвер %s не работает\n", name); return; }
    int rc = drvget_ctl(0, name);
    if (rc < 0) { lg("drvman: остановка %s отклонена: %s (код %d)\n", name, err_name(rc), rc); return; }
    lg("drvman: просьба остановить %s принята, жду...\n", name);
    u64 until = k_clock_ns() + 6000000000ull;
    int w;
    do { k_sleep(10); refresh_pci(); w = running(name); } while (w && k_clock_ns() < until);
    lg(w ? "drvman: драйвер %s ещё работает (ядро не завершило процесс за 6 с)\n" : "drvman: драйвер %s остановлен, устройство возвращено ядру\n", name);
}

enum { J_NONE = 0, J_FIND, J_INSTALL, J_REMOVE, J_START, J_STOP };
static volatile int busy, job_kind;
static const char *const job_name[] = { "", "поиск и проверка", "сборка и установка", "удаление", "запуск", "остановка" };

static void run_job(int kind, const char *arg) {
    switch (kind) {
    case J_FIND: act_find(arg); break;
    case J_INSTALL: act_install(); break;
    case J_REMOVE: act_remove(arg); break;
    case J_START: act_start(arg); break;
    case J_STOP: act_stop(arg); break;
    default: break;
    }
}
#define STACK_BYTES (512 * 1024)                          /* kcc рекурсивен (вложенность до 600): стеку рабочего потока нужен запас, он не растёт сам, как стек главного */
static u8 *stacks[2];
static int stack_turn;
static void worker(u64 unused) {
    (void)unused;
    run_job(job_kind, job_arg);
    refresh_pkgs();
    refresh_pci();
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    dirty = 1;
}
static void start_job(int kind, const char *arg) {
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) { lg("drvman: подождите, предыдущее действие (%s) ещё идёт\n", job_name[job_kind]); return; }
    snprintf(job_arg, sizeof job_arg, "%s", arg);
    job_kind = kind;
    u8 *st = stacks[stack_turn ^= 1];
    if (!st || k_thread_create(worker, st, STACK_BYTES, 0) < 0) {
        lg("drvman: не удалось создать рабочий поток, выполняю в окне (оно не отвечает)\n");
        run_job(kind, arg);
        refresh_pkgs(); refresh_pci();
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    }
    dirty = 1;
}

/* ---------- интерфейс ---------- */
#define DEV_Y 38
#define PKG_Y 172
#define PKG_H 64
#define IN_Y 338
#define BTN_Y 364
#define HINT_Y 390
#define LOG_Y 410
#define LOG_ROWS ((H - LOG_Y - 4) / CH)
typedef struct { int x, w; const char *label; int id; } btn_t;
enum { B_FIND, B_INSTALL, B_REMOVE, B_START, B_STOP, B_REFRESH };
static const btn_t btns[] = {
    { 8, 152, "Найти и показать", B_FIND }, { 168, 96, "Установить", B_INSTALL }, { 272, 72, "Удалить", B_REMOVE },
    { 352, 88, "Запустить", B_START }, { 448, 96, "Остановить", B_STOP }, { 552, 80, "Обновить", B_REFRESH },
};
static int confirming;                                    /* окно подтверждения установки открыто */
static const btn_t yes_btn = { 120, 208, "Собрать и установить", 100 }, no_btn = { 344, 96, "Отмена", 101 };
#define CONF_X 40
#define CONF_Y 90
#define CONF_W 560
#define CONF_H 330
#define CONF_BY (CONF_Y + CONF_H - 40)

static int btn_enabled(int id) {
    int sp = sel_pkg >= 0 && sel_pkg < npkg;
    switch (id) {
    case B_FIND: return input_len > 0;
    case B_INSTALL: return drvget_staged() != 0;
    case B_REMOVE: return sp && !running(pkg[sel_pkg].name);
    case B_START: return sp && pkg[sel_pkg].state == DGP_OK && !running(pkg[sel_pkg].name);
    case B_STOP: return sp && running(pkg[sel_pkg].name);
    default: return 1;
    }
}
static void draw_btn(const btn_t *b, int y, int enabled) {
    fill(b->x, y, b->w, 24, enabled ? ACCENT : PANEL);
    int tw = cols_of(b->label) * CW;
    text(b->x + (b->w - tw) / 2, y + 4, b->label, enabled ? 0x00081018u : DIM, 40);
}

static void draw_confirm(void) {
    const dg_recipe_t *r = drvget_staged();
    if (!r) return;
    char b[200], cs[80], hx[80];
    fill(CONF_X - 2, CONF_Y - 2, CONF_W + 4, CONF_H + 4, ACCENT);
    fill(CONF_X, CONF_Y, CONF_W, CONF_H, BG);
    int y = CONF_Y + 8, x = CONF_X + 10;
    snprintf(b, sizeof b, "Установить драйвер «%s», версия %u?", r->name, (unsigned)r->version);
    text(x, y, b, FG, 68); y += 22;
    snprintf(b, sizeof b, "источник: %s", drvget_staged_url());
    text(x, y, b, FG, 66); y += CH;
    snprintf(b, sizeof b, "лицензия: %s (проверена по тексту каждого файла)", r->license);
    text(x, y, b, FG, 66); y += CH;
    snprintf(b, sizeof b, "файлов: %u, главный %s", (unsigned)r->nfile, r->main_file);
    text(x, y, b, FG, 66); y += CH;
    snprintf(b, sizeof b, "SHA-256 набора:");
    text(x, y, b, DIM, 66); y += CH;
    snprintf(hx, sizeof hx, "%s", drvget_staged_hash());
    text(x, y, hx, AMBER, 66); y += CH + 6;
    text(x, y, "устройства PCI:", DIM, 66);
    int xx = x + 16 * CW;
    for (u32 i = 0; i < r->nmatch; i++) {
        if (r->match[i].vendor != KDRV_ANY) snprintf(b, sizeof b, "%04x:%04x ", r->match[i].vendor, r->match[i].device);
        else snprintf(b, sizeof b, "класс %06x ", (unsigned)r->match[i].cls);
        xx = text(xx, y, b, FG, 20);
    }
    y += CH;
    caps_str(r->caps, cs, sizeof cs);
    snprintf(b, sizeof b, "запрашиваемые права: %s", cs);
    text(x, y, b, FG, 66); y += CH;
    text(x, y, "права действуют только на устройство этого драйвера.", DIM, 66); y += CH + 6;
    if (r->caps & CAP_DMA) {
        text(x, y, "ВНИМАНИЕ: IOMMU нет. Устройство с DMA способно писать", RED, 66); y += CH;
        text(x, y, "в любую память; программно это не отключить. Ставьте", RED, 66); y += CH;
        text(x, y, "только драйверы, чьим исходникам вы доверяете.", RED, 66); y += CH;
    }
    text(x, y + 6, "Сборка идёт на этом устройстве компилятором kcc.", DIM, 66);
    draw_btn(&yes_btn, CONF_BY, 1);
    draw_btn(&no_btn, CONF_BY, 1);
}

/* Рисует кадр; возвращает число показанных строк списков (для строки «кадр» в журнале стенда). */
static int draw(void) {
    int rows = 0;
    char b[240];
    fill(0, 0, W, H, BG);
    fill(0, 0, W, 20, BAR_BG);
    k_mutex_lock(&lock);
    if (pci_err) snprintf(b, sizeof b, "Драйверы: список PCI недоступен (%s)", err_name(pci_err));
    else snprintf(b, sizeof b, "Драйверы: устройств PCI %d, пакетов в /drv %d", npci, npkg < 0 ? 0 : npkg);
    text(6, 2, b, FG, 62);
    if (busy) {
        snprintf(b, sizeof b, "%s...", job_name[job_kind]);
        text(W - (cols_of(b) + 1) * CW, 2, b, AMBER, 30);
    }
    /* устройства */
    text(6, 22, "Устройства PCI (Tab - список, стрелки - выбор)", focus_pkgs ? DIM : ACCENT, 70);
    fill(0, DEV_Y - 2, W, 1, PANEL);
    for (int r = 0; r < DEV_ROWS && dev_top + r < npci; r++) {
        int i = dev_top + r, y = DEV_Y + r * CH;
        char left[160], st[96];
        u32 col;
        if (i == sel_dev) fill(0, y, W, CH, PANEL);
        dev_parts(&pci[i], left, sizeof left, st, sizeof st, &col);
        int x = text(6, y, left, FG, DEV_LEFT_COLS);
        text(x, y, st, col, 79 - DEV_LEFT_COLS);
        rows++;
    }
    if (pci_err) text(6, DEV_Y + 4, "Нет права CAP_PCILIST или вызов ядра отклонён: список устройств не показывается.", RED, 76);
    else if (!npci) text(6, DEV_Y + 4, "Ядро не вернуло ни одного устройства PCI.", DIM, 76);
    if (npci > DEV_ROWS) { snprintf(b, sizeof b, "%d-%d из %d", dev_top + 1, dev_top + DEV_ROWS > npci ? npci : dev_top + DEV_ROWS, npci); text(W - 14 * CW, 22, b, DIM, 14); }
    /* пакеты */
    text(6, PKG_Y - 18, "Пакеты драйверов в /drv", focus_pkgs ? ACCENT : DIM, 70);
    fill(0, PKG_Y - 2, W, 1, PANEL);
    for (int r = 0; r < PKG_ROWS && pkg_top + r < npkg; r++) {
        int i = pkg_top + r, y = PKG_Y + r * PKG_H;
        if (i == sel_pkg) fill(0, y, W, PKG_H - 2, PANEL);
        pkg_line(&pkg[i], 0, b, sizeof b);
        text(6, y, b, running(pkg[i].name) ? GREEN : FG, 78);
        for (int n = 1; n < 4; n++) { pkg_line(&pkg[i], n, b, sizeof b); text(6, y + n * CH, b, n == 3 ? DIM : FG, 78); }
        rows += 4;
    }
    if (npkg < 0) text(6, PKG_Y + 4, "Каталога /drv нет или он недоступен.", AMBER, 76);
    else if (!npkg) text(6, PKG_Y + 4, "Пакетов нет. Введите имя драйвера ниже и нажмите «Найти и показать».", DIM, 76);
    if (npkg > PKG_ROWS) { snprintf(b, sizeof b, "%d-%d из %d", pkg_top + 1, pkg_top + PKG_ROWS > npkg ? npkg : pkg_top + PKG_ROWS, npkg); text(W - 14 * CW, PKG_Y - 18, b, DIM, 14); }
    k_mutex_unlock(&lock);
    /* поле ввода */
    text(6, IN_Y - 18, "Имя драйвера (рецепт drvget) или адрес https://.../drvget.rec:", DIM, 76);
    fill(6, IN_Y, W - 12, 22, PANEL);
    fill(6, IN_Y + 21, W - 12, 1, ACCENT);
    text(10, IN_Y + 3, input, FG, 76);
    if ((k_ticks() / 50) & 1) fill(10 + cols_of(input) * CW, IN_Y + 3, 2, CH - 2, ACCENT);
    for (unsigned i = 0; i < sizeof btns / sizeof *btns; i++) draw_btn(&btns[i], BTN_Y, btn_enabled(btns[i].id));
    text(6, HINT_Y, "Enter - найти, F2 - установить, F3 - удалить, F4 - запустить, F6 - остановить, F5 - обновить", DIM, 78);
    /* журнал */
    fill(0, LOG_Y - 4, W, H - LOG_Y + 4, 0x000C1218u);
    k_mutex_lock(&lock);
    unsigned total = log_n + 1;                           /* вместе с недописанной */
    int first = (int)total - LOG_ROWS - log_scroll;
    if (first < 0) first = 0;
    if ((int)total - first > LOG_LINES) first = (int)total - LOG_LINES;
    for (int r = 0; r < LOG_ROWS && first + r < (int)total; r++) text(4, LOG_Y + r * CH, loglines[(unsigned)(first + r) % LOG_LINES], FG, LOG_COLS);
    k_mutex_unlock(&lock);
    if (confirming) draw_confirm();
    return rows;
}

static void present(void) {
    if (!have_window) return;
    int rows = draw();
    k_gfx_present();
    dirty = 0;
    (void)rows;
}

static int hit(const btn_t *b, int y, int mx, int my) { return mx >= b->x && mx < b->x + b->w && my >= y && my < y + 24; }

static const char *sel_name(void) { return sel_pkg >= 0 && sel_pkg < npkg ? pkg[sel_pkg].name : ""; }

static void confirm_open(void) {                           /* окно подтверждения: в журнал то же, что на нём написано */
    const dg_recipe_t *r = drvget_staged();
    char cs[80];
    if (!r) return;
    caps_str(r->caps, cs, sizeof cs);
    confirming = 1;
    lg("drvman: подтверждение: установить %s v%u?\n", r->name, (unsigned)r->version);
    lg("drvman: подтверждение: источник %s\n", drvget_staged_url());
    lg("drvman: подтверждение: лицензия %s, права: %s\n", r->license, cs);
    lg("drvman: подтверждение: SHA-256 набора %s\n", drvget_staged_hash());
    if (r->caps & CAP_DMA) lg("drvman: подтверждение: предупреждение: IOMMU нет, DMA-устройство может писать в любую память\n");
}

static void press(int id) {
    if (!btn_enabled(id)) return;
    switch (id) {
    case B_FIND: start_job(J_FIND, input); break;
    case B_INSTALL: confirm_open(); break;
    case B_REMOVE: start_job(J_REMOVE, sel_name()); break;
    case B_START: start_job(J_START, sel_name()); break;
    case B_STOP: start_job(J_STOP, sel_name()); break;
    case B_REFRESH: refresh_pci(); if (!busy) refresh_pkgs(); break;
    default: break;
    }
}

static void select_dev(int i) {
    sel_dev = i;
    if (i >= 0 && i < npci) {
        int w = pci[i].owner == KDRV_OWN_RING3 ? 1 : 0, k = -1;
        if (w) { for (int q = 0; q < npkg; q++) if (!strcmp(pkg[q].name, pci[i].name)) k = q; }
        else k = pkg_for_dev(&pci[i]);
        if (k >= 0) { sel_pkg = k; if (k < pkg_top) pkg_top = k; if (k >= pkg_top + PKG_ROWS) pkg_top = k - PKG_ROWS + 1; }
    }
}

static void on_key(u32 k, u32 ch, u32 mod) {
    if (confirming) {
        if (k == KEY_ENTER) { confirming = 0; lg("drvman: подтверждено пользователем, собираю и ставлю\n"); start_job(J_INSTALL, ""); }
        else if (k == KEY_ESC) { confirming = 0; lg("drvman: установка отменена пользователем, ничего не записано\n"); }
        return;
    }
    if (k == KEY_TAB) focus_pkgs ^= 1;
    else if (k == KEY_UP || k == KEY_DOWN) {
        int d = k == KEY_UP ? -1 : 1;
        k_mutex_lock(&lock);
        if (focus_pkgs) {
            int n = sel_pkg + d;
            if (n >= 0 && n < npkg) { sel_pkg = n; if (n < pkg_top) pkg_top = n; if (n >= pkg_top + PKG_ROWS) pkg_top = n - PKG_ROWS + 1; }
        } else {
            int n = sel_dev + d;
            if (n >= 0 && n < npci) { select_dev(n); if (n < dev_top) dev_top = n; if (n >= dev_top + DEV_ROWS) dev_top = n - DEV_ROWS + 1; }
        }
        k_mutex_unlock(&lock);
    }
    else if (k == KEY_PGUP) { if (log_scroll < LOG_LINES - LOG_ROWS) log_scroll += LOG_ROWS - 1; }
    else if (k == KEY_PGDN) { log_scroll -= LOG_ROWS - 1; if (log_scroll < 0) log_scroll = 0; }
    else if (k == KEY_BACKSPACE) { if (input_len) input[--input_len] = 0; }
    else if (k == KEY_ENTER) press(B_FIND);
    else if (k == KEY_ESC) { input[0] = 0; input_len = 0; }
    else if (k == KEY_F2) press(B_INSTALL);
    else if (k == KEY_F3) press(B_REMOVE);
    else if (k == KEY_F4) press(B_START);
    else if (k == KEY_F6) press(B_STOP);
    else if (k == KEY_F5) press(B_REFRESH);
    else if (ch >= 32 && ch < 127 && !(mod & MOD_CTRL) && input_len + 1 < (int)sizeof input) { input[input_len++] = (char)ch; input[input_len] = 0; }
}

static void on_click(int x, int y) {
    if (confirming) {
        if (hit(&yes_btn, CONF_BY, x, y)) { confirming = 0; lg("drvman: подтверждено пользователем, собираю и ставлю\n"); start_job(J_INSTALL, ""); }
        else if (hit(&no_btn, CONF_BY, x, y)) { confirming = 0; lg("drvman: установка отменена пользователем, ничего не записано\n"); }
        return;
    }
    for (unsigned i = 0; i < sizeof btns / sizeof *btns; i++) if (hit(&btns[i], BTN_Y, x, y)) { press(btns[i].id); return; }
    k_mutex_lock(&lock);
    if (y >= DEV_Y && y < DEV_Y + DEV_ROWS * CH) {
        int i = dev_top + (y - DEV_Y) / CH;
        focus_pkgs = 0;
        if (i < npci) select_dev(i);
    } else if (y >= PKG_Y && y < PKG_Y + PKG_ROWS * PKG_H) {
        int i = pkg_top + (y - PKG_Y) / PKG_H;
        focus_pkgs = 1;
        if (i < npkg) sel_pkg = i;
    }
    k_mutex_unlock(&lock);
}

/* ---------- режим do ---------- */
static int run_script(char *s) {
    int fails = 0;
    while (*s) {
        char *e = strchr(s, ';');
        if (e) *e = 0;
        while (*s == ' ') s++;
        char *arg = s;
        while (*arg && *arg != ' ') arg++;
        if (*arg) *arg++ = 0;
        while (*arg == ' ') arg++;
        if (!strcmp(s, "pci")) { refresh_pci(); report_pci(); }
        else if (!strcmp(s, "pkgs")) { refresh_pkgs(); report_pkgs(); }
        else if (!strcmp(s, "base")) drvget_base(arg);
        else if (!strcmp(s, "verify")) { if (drvget_verify(arg)) fails++; }
        else if (!strcmp(s, "list")) drvget_list();
        else if (!strcmp(s, "find")) run_job(J_FIND, arg);
        else if (!strcmp(s, "install")) { run_job(J_INSTALL, ""); }
        else if (!strcmp(s, "remove")) run_job(J_REMOVE, arg);
        else if (!strcmp(s, "start")) run_job(J_START, arg);
        else if (!strcmp(s, "stop")) run_job(J_STOP, arg);
        else if (*s) { lg("drvman: неизвестная команда «%s» (pci, pkgs, base, find, install, start, stop, remove)\n", s); fails++; }
        if (!e) break;
        s = e + 1;
    }
    return fails;
}

int umain(void) {
    char args[200];
    i64 al = k_args(args, sizeof args - 1);
    if (al < 0) al = 0;
    args[al] = 0;
    drvget_set_sink(sink);
    px = k_gfx_create(W, H);
    have_window = px != 0;
    if (have_window) lg("drvman: окно создано %dx%d\n", W, H);
    else lg("drvman: окно не создано (нет права на окно или нет экрана): работает без окна\n");
    refresh_pci();
    refresh_pkgs();
    int rows = have_window ? draw() : 0;
    if (have_window) k_gfx_present();
    lg("drvman: кадр: строк в списках %d, устройств %d, пакетов %d%s\n", rows, pci_err ? -1 : npci, npkg < 0 ? 0 : npkg, npkg < 0 ? " (каталога /drv нет)" : "");
    char *p = args;
    while (*p == ' ') p++;
    if (!strncmp(p, "do ", 3) || !strcmp(p, "do")) {
        int fails = run_script(p[2] ? p + 3 : p + 2);
        refresh_pci(); refresh_pkgs();
        rows = have_window ? draw() : 0;
        if (have_window) k_gfx_present();
        lg("drvman: кадр: строк в списках %d, устройств %d, пакетов %d%s\n", rows, pci_err ? -1 : npci, npkg < 0 ? 0 : npkg, npkg < 0 ? " (каталога /drv нет)" : "");
        lg("drvman: готово%s\n", fails ? " с ошибками" : "");
        k_sleep(100);
        return fails ? 1 : 0;
    }
    if (!have_window) return 1;
    stacks[0] = k_heap_grow(STACK_BYTES / 4096);              /* стеки рабочих потоков - до арены kcc: арена растёт непрерывно от своего начала */
    stacks[1] = k_heap_grow(STACK_BYTES / 4096);
    u64 last = k_clock_ns();
    for (;;) {
        kevent_t ev;
        while (k_gfx_event(&ev) > 0) {
            if (ev.type == KEV_CLOSE) return 0;
            if (ev.type == KEV_KEY_DOWN) { on_key(ev.a, ev.b, ev.c); dirty = 1; }
            else if (ev.type == KEV_MOUSE_BUTTON) {
                static int down;
                int d = (int)(ev.c & 1);
                if (d && !down) { on_click((int)ev.a, (int)ev.b); dirty = 1; }
                down = d;
            }
        }
        u64 now = k_clock_ns();
        if (now - last > 1000000000ull) { last = now; refresh_pci(); dirty = 1; }      /* раз в секунду: кто сейчас держит устройства */
        if (dirty) present();
        k_sleep(2);
    }
}
