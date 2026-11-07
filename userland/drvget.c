/* drvget - установка драйверов Ring 3 из исходников на самой Кире. Локальная сборка подтверждается пользователем и SHA-256;
   detached .sig отдельно подтверждает издателя Ed25519. Отсутствие подписи явно показывается как локальное, не издательское доверие.
     drvget install ИМЯ|https://адрес/drvget.rec   скачать рецепт и исходники по HTTPS (TLS 1.3, корневые /etc/ssl/roots.der), проверить лицензию
                                                    (BSD-2/BSD-3/MIT/ISC; GPL и бинарники отвергаются), показать источник, лицензию, SHA-256, права;
                                                    ничего не ставит
     drvget confirm ИМЯ ХЕШ                         ХЕШ - первые 8+ знаков SHA-256 набора исходников из отчёта install: сверяет набор, собирает
                                                    встроенным kcc (со слоем /etc/kcc/include/kdrv_bsd.h), пишет /drv/ИМЯ.kdrv и /drv/ИМЯ.rec
     drvget list | verify ИМЯ | remove ИМЯ | base [https://адрес]
   Права программы: CAP_NET, CAP_FS, CAP_SYSFILES. Запись в /drv - только отсюда (FS_OP_DRV_*, ядро проверяет заголовок и хеши само).
   Исходники и промежуточные файлы - песочница /data/drvget. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "kira.h"
#include "tls.h"
#include "drvget_core.h"
#include "drvget_api.h"
#include "../immune.h"
#include "../kdrv_sig.h"
#include "../kdrv_trust.h"
#include "../kcc/kcc.h"

#ifndef DRVGET_LIB
KIRA_ENTRY()
#endif

enum { KFS_INC_READ = 11, KFS_INC_STAT = 12, KFS_DRV_STAT = 13, KFS_DRV_READ = 14, KFS_DRV_WRITE = 15, KFS_DRV_REMOVE = 16, KFS_DRV_LIST = 17,
       KFS_DRV_START = 18, KFS_DRV_STOP = 19 };
#define INC_DIR "/etc/kcc/include"
#define STAGE_DIR "/stage/"
#define PAGE 4096ul
#define CHUNK_PAGES 256ul
#ifndef DRVGET_DEFAULT_BASE
#define DRVGET_DEFAULT_BASE "https://raw.githubusercontent.com/Masuku4129/kira_os_/main/drivers"
#endif

/* Куда идёт текст отчёта: по умолчанию в журнал процесса (консоль); окно «Драйверы» (drvman) собирает его в свою панель. */
static void (*sink)(const char *, size_t);

static void say(const char *s) {
    size_t n = strlen(s);
    if (sink) { sink(s, n); return; }
    while (n) { size_t k = n > 2048 ? 2048 : n; k_write(s, k); s += k; n -= k; }
}
static void sayf(const char *fmt, ...) {
    char b[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    say(b);
}

static tls_t tls;
static u8 roots[512 * 1024];
static i64 roots_len = -1;
static char index_text[DG_INDEX_MAX + 1];
static dg_index_entry_t index_entries[DG_INDEX_ENTRIES];
static u32 index_count;
static int index_loaded, index_state;
static u8 body[DG_SRC_MAX + 16 * 1024];
static char recipe_txt[DG_RECIPE_MAX + 1];
static dg_recipe_t R;
static char g_name[KDRV_NAME_MAX + 2];
static char g_staged_url[DG_URL_MAX];                /* адрес рецепта подготовленного набора */
static char g_staged_hash[65];                    /* SHA-256 набора, подготовленного последним успешным install (пусто - нет) */

/* ---------- сеть: HTTPS GET ---------- */
static int load_roots(void) {
    if (roots_len >= 0) return roots_len > 0 ? 0 : -1;
    i64 sz = k_sys_stat("roots.der"), rl = 0;
    roots_len = 0;
    if (sz > 0 && (u64)sz <= sizeof roots)
        while (rl < sz) { i64 r = k_sys_read("roots.der", roots + rl, (u64)(sz - rl), (u64)rl); if (r <= 0) break; rl += r; }
    if (rl != sz || sz <= 0) return -1;
    roots_len = sz;
    return 0;
}

static int load_index(void) {
    if (index_loaded) return index_state;
    index_loaded = 1;
    index_state = 0;
    i64 sz = k_sys_stat("drivers.index");
    if (sz <= 0) return 0;
    if ((u64)sz > DG_INDEX_MAX) { index_state = -1; return -1; }
    i64 off = 0;
    while (off < sz) {
        i64 n = k_sys_read("drivers.index", index_text + off, (u64)(sz - off), (u64)off);
        if (n <= 0) { index_state = -1; return -1; }
        off += n;
    }
    index_text[sz] = 0;
    const char *err = "";
    if (dg_parse_index(index_text, (u32)sz, index_entries, DG_INDEX_ENTRIES, &index_count, &err)) {
        sayf("drvget: системный каталог драйверов повреждён: %s\n", err);
        index_state = -1;
        return -1;
    }
    index_state = 1;
    return 1;
}
static const dg_index_entry_t *index_by_url(const char *url) {
    if (load_index() != 1) return 0;
    for (u32 i = 0; i < index_count; i++) if (!strcmp(index_entries[i].url, url)) return &index_entries[i];
    return 0;
}
int drvget_match_pci(uint16_t vendor, uint16_t device, char *name, size_t name_cap, char *url, size_t url_cap) {
    int state = load_index();
    if (state < 0) return -1;
    if (state == 0) return 0;
    int ix = dg_index_find_pci(index_entries, index_count, vendor, device);
    if (ix < 0) return 0;
    const dg_index_entry_t *e = &index_entries[ix];
    size_t nl = strlen(e->name), ul = strlen(e->url);
    if (!name || !url || name_cap <= nl || url_cap <= ul) return -1;
    memcpy(name, e->name, nl + 1);
    memcpy(url, e->url, ul + 1);
    return 1;
}


static int ci_has(const char *h, size_t hl, const char *needle) {        /* заголовок ответа: есть ли строка (без учёта регистра) */
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t k = 0;
        while (k < nl && ((h[i + k] | 0x20) == (needle[k] | 0x20))) k++;
        if (k == nl) return 1;
    }
    return 0;
}

/* GET по https: тело ответа в body (с начала), длина в *len. 0 - готово (HTTP 200); иначе строка ошибки. */
static const char *http_get(const dg_url_t *u, u32 *len) {
    static char err[160];
    if (load_roots()) return "нет корневых сертификатов (/etc/ssl/roots.der): личность сервера проверить нечем, соединяться не буду";
    i64 ip = k_net_dns(u->host);
    if (ip < 0) { snprintf(err, sizeof err, "адрес не найден: %s (код %lld)", u->host, (long long)ip); return err; }
    i64 h = k_net_connect((u32)ip, u->port);
    if (h < 0) { snprintf(err, sizeof err, "соединение не установлено (код %lld; сети нет или порт закрыт)", (long long)h); return err; }
    i64 now = k_time();
    if (tls_connect(&tls, h, u->host, 10000, roots, (u32)roots_len, now > 0 ? now : 0, 0)) {
        snprintf(err, sizeof err, "TLS: %s", tls.err ? tls.err : "ошибка");
        k_net_close(h);
        return err;
    }
    char req[DG_URL_MAX + 160];
    int rn = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: KiraOS-drvget\r\nConnection: close\r\n\r\n", u->path, u->host);
    if (rn <= 0 || (size_t)rn >= sizeof req || tls_send(&tls, req, (u32)rn) != rn) { tls_close(&tls); k_net_close(h); return "запрос не отправлен"; }
    u32 total = 0;
    i64 r;
    while ((r = tls_recv(&tls, body + total, (u32)(sizeof body - total > 4096 ? 4096 : sizeof body - total), 10000)) > 0) {
        total += (u32)r;
        if (total >= sizeof body) { tls_close(&tls); k_net_close(h); return "ответ слишком велик"; }
    }
    tls_close(&tls);
    k_net_close(h);
    if (r < 0) return "соединение прервано до конца ответа";
    u32 he = 0;
    while (he + 3 < total && !(body[he] == '\r' && body[he + 1] == '\n' && body[he + 2] == '\r' && body[he + 3] == '\n')) he++;
    if (he + 3 >= total) return "ответ без заголовков";
    if (total < 12 || memcmp(body, "HTTP/1.", 7) != 0) return "это не ответ HTTP";
    int status = (body[9] - '0') * 100 + (body[10] - '0') * 10 + (body[11] - '0');
    if (status != 200) {
        snprintf(err, sizeof err, "сервер ответил HTTP %d%s", status, (status >= 300 && status < 400) ? " (перенаправления не выполняются: укажите конечный адрес)" : "");
        return err;
    }
    if (ci_has((const char *)body, he, "transfer-encoding: chunked")) return "сервер прислал chunked: нужен HTTP/1.0-ответ с Content-Length";
    if (ci_has((const char *)body, he, "content-encoding:")) return "сервер сжал ответ (Content-Encoding): не поддерживается";
    u32 off = he + 4, blen = total - off;
    const char *cl = 0;
    for (u32 i = 0; i + 16 < he; i++)
        if ((body[i] | 0x20) == 'c' && ci_has((const char *)body + i, 15, "content-length:")) { cl = (const char *)body + i + 15; break; }
    if (cl) {
        while (*cl == ' ') cl++;
        u64 want = 0;
        while (*cl >= '0' && *cl <= '9') want = want * 10 + (u64)(*cl++ - '0');
        if (want != blen) return "тело короче заявленного Content-Length (обрыв)";
    }
    memmove(body, body + off, blen);
    *len = blen;
    return 0;
}

/* ---------- файлы песочницы и /drv ---------- */
static void stage_name(char *out, size_t cap, const char *file) { snprintf(out, cap, "s-%s-%s", g_name, file); }

static i64 fread_all(const char *name, void *buf, u64 cap) {
    i64 sz = k_file_size(name);
    if (sz < 0) return sz;
    if ((u64)sz > cap) return -101;
    if (sz && k_file_read(name, buf, (u64)sz, 0) != sz) return -6;
    return sz;
}

static i64 drv_op(u32 op, const char *name, void *buf, u64 len, u64 off) { return k_fs(op, name, strlen(name), buf, len, off); }

static void stage_clear(void) {
    static char lst[4096];
    i64 n = k_file_list(lst, sizeof lst - 1);
    if (n <= 0) return;
    lst[n] = 0;
    /* One staged candidate per sandbox. Clear across process restarts too:
       g_name is not persistent between separate drvget CLI invocations. */
    for (char *p = lst; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (!strncmp(p, "s-", 2)) k_file_remove(p);
        if (!e) break;
        p = e + 1;
    }
}

static int name_ok(const char *n) {
    size_t k = strlen(n);
    if (k < 1 || k > KDRV_NAME_MAX) return 0;
    for (size_t i = 0; i < k; i++) { char c = n[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0; }
    return 1;
}

static int base_url(char *out, size_t cap) {
    char b[DG_URL_MAX] = { 0 };
    i64 n = k_file_read("base", b, sizeof b - 1, 0);
    if (n > 0) {
        b[n] = 0;
        while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == ' ' || b[n - 1] == '/')) b[--n] = 0;
        if (n > 0) { snprintf(out, cap, "%s", b); return 1; }
    }
    snprintf(out, cap, "%s", DRVGET_DEFAULT_BASE);
    return 0;
}

static const char *cap_text(u32 c) {
    switch (c) {
    case CAP_MMIO: return "CAP_MMIO - отображать регистры (BAR) СВОЕГО устройства";
    case CAP_IRQ: return "CAP_IRQ - ждать прерывание СВОЕГО устройства";
    case CAP_DMA: return "CAP_DMA - получать непрерывную память для обмена устройства с ОЗУ";
    case CAP_PCIDEV: return "CAP_PCIDEV - читать и (частично) писать конфигурацию PCI СВОЕГО устройства";
    case CAP_NETDEV: return "CAP_NETDEV - отдать ядру сетевую карту (кольца кадров)";
    default: return "?";
    }
}

/* ---------- install ---------- */
static int cmd_install(const char *arg) {
    stage_clear();
    g_name[0] = 0;
    g_staged_hash[0] = 0;
    g_staged_url[0] = 0;
    char url[DG_URL_MAX];
    const char *err = "";
    const dg_index_entry_t *pinned = 0;
    int index_status_now = load_index();
    if (index_status_now < 0) { say("drvget: отказ: системный каталог повреждён; установка заблокирована\n"); return 1; }
    if (strstr(arg, "://")) {
        snprintf(url, sizeof url, "%s", arg);
        if (index_status_now > 0) pinned = index_by_url(url);
    } else {
        if (!name_ok(arg)) { say("drvget: имя драйвера: a-z 0-9 - _ (до 11 знаков) или полный адрес https://...\n"); return 2; }
        char b[DG_URL_MAX];
        int own = base_url(b, sizeof b);
        int ix = (!own && index_status_now > 0) ? dg_index_find_name(index_entries, index_count, arg) : -1;
        if (ix >= 0) {
            pinned = &index_entries[ix];
            snprintf(url, sizeof url, "%s", pinned->url);
            sayf("drvget: системный каталог выбрал %s для PCI %04x:%04x\n", pinned->name, pinned->vendor, pinned->device);
        } else {
            snprintf(url, sizeof url, "%s/%s/drvget.rec", b, arg);
            sayf("drvget: каталог рецептов: %s%s\n", b, own ? " (пользовательский; не закреплён системным каталогом)" : " (по умолчанию)");
        }
    }
    dg_url_t ru;
    if (dg_url_parse(url, &ru, &err)) { sayf("drvget: отказ: %s\n", err); return 1; }
    sayf("drvget: скачиваю рецепт %s\n", url);
    u32 n = 0;
    if ((err = http_get(&ru, &n))) { sayf("drvget: отказ: %s\n", err); return 1; }
    if (n > DG_RECIPE_MAX) { say("drvget: отказ: рецепт слишком велик\n"); return 1; }
    if (pinned) {
        u8 want[32], got[32];
        if (!dg_hex_parse(pinned->recipe_sha, want, 32)) { say("drvget: отказ: неверный хеш системного каталога\n"); return 1; }
        sha256(body, n, got);
        if (memcmp(want, got, 32)) { say("drvget: отказ: SHA-256 рецепта не совпал с системным каталогом\n"); return 1; }
        say("drvget: SHA-256 рецепта совпал с системным каталогом\n");
    } else {
        say("drvget: предупреждение: рецепт не закреплён системным каталогом; издатель не аутентифицирован, требуется ручная проверка и подтверждение\n");
    }
    memcpy(recipe_txt, body, n);
    recipe_txt[n] = 0;
    if (dg_parse_recipe(recipe_txt, n, &R, &err)) { sayf("drvget: отказ: рецепт: %s\n", err); return 1; }
    for (u32 i = 0; i < R.nfile; i++) if (!R.file[i].sha[0]) { sayf("drvget: отказ: исходный файл %s не закреплён SHA-256 в рецепте\n", R.file[i].name); return 1; }
    if (!strstr(arg, "://") && strcmp(arg, R.name)) { sayf("drvget: отказ: запрошен '%s', а рецепт называет драйвер '%s'\n", arg, R.name); return 1; }
    snprintf(g_name, sizeof g_name, "%s", R.name);

    dg_hash_t hs;
    dg_set_begin(&hs);
    dg_set_add(&hs, "drvget.rec", (const u8 *)recipe_txt, n);
    static char lic[DG_FILES][24];
    static u32 flen[DG_FILES];
    static u8 fsha[DG_FILES][32];
    for (u32 i = 0; i < R.nfile; i++) {
        char fu[DG_URL_MAX];
        dg_url_t fu_p;
        if (dg_url_join(&ru, R.file[i].url, fu, sizeof fu, &err) || dg_url_parse(fu, &fu_p, &err)) { sayf("drvget: отказ: файл %s: %s\n", R.file[i].name, err); stage_clear(); return 1; }
        sayf("drvget: скачиваю %s\n", fu);
        u32 len = 0;
        if ((err = http_get(&fu_p, &len))) { sayf("drvget: отказ: %s: %s\n", R.file[i].name, err); stage_clear(); return 1; }
        {
            u8 want[32];
            if (!dg_hex_parse(R.file[i].sha, want, 32)) { sayf("drvget: отказ: неверный SHA-256 файла %s в рецепте\n", R.file[i].name); stage_clear(); return 1; }
            sha256(body, len, fsha[i]);
            if (memcmp(want, fsha[i], 32)) { sayf("drvget: отказ: SHA-256 файла %s не совпал с рецептом (подмена или повреждение)\n", R.file[i].name); stage_clear(); return 1; }
        }
        if (dg_scan_source(R.file[i].name, body, len, lic[i], &err)) { sayf("drvget: отказ: %s: %s\n", R.file[i].name, err); stage_clear(); return 1; }
        dg_set_add(&hs, R.file[i].name, body, len);
        flen[i] = len;
        char sn[48];
        stage_name(sn, sizeof sn, R.file[i].name);
        i64 w = k_file_write(sn, body, len);
        if (w < 0) { sayf("drvget: не удалось сохранить %s (код %lld)\n", sn, (long long)w); stage_clear(); return 1; }
    }
    char sn[48];
    stage_name(sn, sizeof sn, "recipe");
    if (k_file_write(sn, recipe_txt, n) < 0) { say("drvget: не удалось сохранить рецепт\n"); stage_clear(); return 1; }
    stage_name(sn, sizeof sn, "url");
    if (k_file_write(sn, url, strlen(url)) < 0) { say("drvget: не удалось сохранить адрес\n"); stage_clear(); return 1; }
    u8 set[32];
    char hx[65];
    dg_set_end(&hs, set);
    dg_hex(set, 32, hx);
    memcpy(g_staged_hash, hx, sizeof hx);
    snprintf(g_staged_url, sizeof g_staged_url, "%s", url);

    sayf("\n=== драйвер '%s' версия %u ===\n", R.name, (unsigned)R.version);
    sayf("источник:   %s\n", url);
    sayf("лицензия:   заявлена %s; проверено по тексту каждого файла (SPDX/текст), GPL и бинарных файлов нет:\n", R.license);
    for (u32 i = 0; i < R.nfile; i++) {
        char fh[65];
        dg_hex(fsha[i], 32, fh);
        sayf("  %-24s %6u байт  %s  sha256 %s\n", R.file[i].name, (unsigned)flen[i], lic[i], fh);
    }
    sayf("SHA-256 набора исходников (рецепт + файлы): %s\n", hx);
    say("устройства PCI:");
    for (u32 i = 0; i < R.nmatch; i++) {
        if (R.match[i].vendor != KDRV_ANY) sayf(" %04x:%04x", R.match[i].vendor, R.match[i].device);
        else sayf(" класс %06x/маска %06x", (unsigned)R.match[i].cls, (unsigned)R.match[i].cls_mask);
    }
    say("\nзапрашиваемые права:\n");
    for (u32 b = 1; b; b <<= 1) if (R.caps & b) sayf("  %s\n", cap_text(b));
    say("ограничения: права действуют только на устройство драйвера. Сейчас ядро не настраивает IOMMU-домены,\n"
        "поэтому драйверы с CAP_DMA или CAP_IRQ будут отклонены и не смогут включить bus mastering/MSI.\n");
#ifdef DRVGET_LIB
    say("\nУстановка НЕ выполнена. Прочитайте источник и права выше.\n");
#else
    sayf("\nУстановка НЕ выполнена. Прочитайте источник и права выше. Чтобы собрать и установить ровно этот набор, введите:\n  drvget confirm %s %.8s\n", R.name, hx);
#endif
    return 0;
}

/* ---------- сборка kcc ---------- */
static char *arena;
static char *g_arena;                             /* арена kcc одна на процесс: повторные сборки (окно «Драйверы») её переиспользуют, куча не растёт */
static size_t g_arena_cap;

static size_t grow(void *ud, size_t cap, size_t need) {
    (void)ud;
    unsigned long pages = (unsigned long)((need - cap + PAGE - 1) / PAGE), want = pages > CHUNK_PAGES ? pages : CHUNK_PAGES;
    void *p = k_heap_grow(want);
    if (!p && want > pages) { want = pages; p = k_heap_grow(want); }
    if (!p || (char *)p != arena + cap) return 0;
    return cap + want * PAGE;
}

static const char *rd(void *ud, const char *path, size_t *len) {
    (void)ud;
    const char *n = path;
    int inc = 0;
    char sn[48];
    if (!strncmp(path, INC_DIR "/", sizeof INC_DIR)) { n = path + sizeof INC_DIR; inc = 1; }
    else if (!strncmp(path, STAGE_DIR, sizeof STAGE_DIR - 1)) { n = path + sizeof STAGE_DIR - 1; if (!*n || strchr(n, '/')) return 0; stage_name(sn, sizeof sn, n); n = sn; }
    else return 0;
    if (!*n || strchr(n, '/')) return 0;
    u64 nl = strlen(n);
    i64 sz = inc ? k_fs(KFS_INC_STAT, n, nl, 0, 0, 0) : k_file_size(n);
    if (sz < 0) return 0;
    char *buf = ka_new((size_t)sz + 1);
    if (sz) {
        i64 got = inc ? k_fs(KFS_INC_READ, n, nl, buf, (u64)sz, 0) : k_file_read(n, buf, (u64)sz, 0);
        if (got != sz) return 0;
    }
    *len = (size_t)sz;
    return buf;
}

static int load_stage(u8 set[32], char *lic_seen) {
    const char *err = "";
    char sn[48];
    stage_name(sn, sizeof sn, "recipe");
    i64 n = fread_all(sn, recipe_txt, DG_RECIPE_MAX);
    if (n <= 0) { sayf("drvget: нет подготовленного набора для '%s': сначала drvget install %s\n", g_name, g_name); return -1; }
    recipe_txt[n] = 0;
    if (dg_parse_recipe(recipe_txt, (u32)n, &R, &err)) { sayf("drvget: рецепт в песочнице повреждён: %s\n", err); return -1; }
    if (strcmp(R.name, g_name)) { say("drvget: имя в рецепте не совпало с запрошенным\n"); return -1; }
    dg_hash_t hs;
    dg_set_begin(&hs);
    dg_set_add(&hs, "drvget.rec", (const u8 *)recipe_txt, (u32)n);
    for (u32 i = 0; i < R.nfile; i++) {
        char lic[24];
        stage_name(sn, sizeof sn, R.file[i].name);
        i64 len = fread_all(sn, body, DG_SRC_MAX);
        if (len <= 0) { sayf("drvget: нет файла %s в подготовленном наборе\n", R.file[i].name); return -1; }
        if (dg_scan_source(R.file[i].name, body, (u32)len, lic, &err)) { sayf("drvget: %s: %s\n", R.file[i].name, err); return -1; }
        dg_set_add(&hs, R.file[i].name, body, (u32)len);
        if (lic_seen && !i) snprintf(lic_seen, 24, "%s", lic);
    }
    dg_set_end(&hs, set);
    return 0;
}

/* «слово1 слово2» -> a, b; 0 - ровно два слова, помещаются */
static int split2(const char *s, char *a, size_t ca, char *b, size_t cb) {
    size_t i = 0, k = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ') { if (i + 1 >= ca) return -1; a[i++] = *s++; }
    a[i] = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ') { if (k + 1 >= cb) return -1; b[k++] = *s++; }
    b[k] = 0;
    while (*s == ' ') s++;
    return (i && k && !*s) ? 0 : -1;
}

static int cmd_confirm(const char *arg) {
    char nm[16], typed[80];
    if (split2(arg, nm, sizeof nm, typed, sizeof typed)) { say("использование: drvget confirm ИМЯ ХЕШ (первые 8 и более знаков SHA-256 набора из отчёта install)\n"); return 2; }
    if (!name_ok(nm)) { say("drvget: неверное имя\n"); return 2; }
    snprintf(g_name, sizeof g_name, "%s", nm);
    u8 set[32];
    if (load_stage(set, 0)) return 1;
    if (!dg_hex_prefix(typed, set)) { say("drvget: введённый хеш не совпал с SHA-256 подготовленного набора - установка отменена (повторите install и сверьте хеш)\n"); return 1; }
    char url[DG_URL_MAX] = { 0 };
    char sn[48];
    stage_name(sn, sizeof sn, "url");
    i64 un = k_file_read(sn, url, sizeof url - 1, 0);
    if (un < 0) un = 0;
    url[un] = 0;

    if (!g_arena) {
        void *base = k_heap_grow(CHUNK_PAGES);
        if (!base) { say("drvget: нет памяти для компилятора\n"); return 1; }
        g_arena = base;
        g_arena_cap = CHUNK_PAGES * PAGE;
    }
    arena = g_arena;
    size_t arena_cap = g_arena_cap;
    static const char *incs[1] = { INC_DIR };
#if defined(__aarch64__)
    KCfg cfg = { rd, 0, incs, 1, 0, 0, "aarch64", grow };
#else
    KCfg cfg = { rd, 0, incs, 1, 0, 0, "x86_64", grow };
#endif
    char mainp[64];
    snprintf(mainp, sizeof mainp, STAGE_DIR "%s", R.main_file);
    sayf("drvget: собираю %s встроенным kcc (%s)...\n", R.main_file, cfg.target);
    KResult kr;
    int rc = kcc_compile(&cfg, arena, arena_cap, mainp, 2, &kr);
    if (K.acap > g_arena_cap) g_arena_cap = K.acap;          /* арена могла вырасти по запросу компилятора */
    if (kr.warn && *kr.warn) say(kr.warn);
    if (rc) { say(kr.err); say("\ndrvget: сборка не удалась, ничего не установлено\n"); return 1; }
    sayf("drvget: собрано, образ ELF %lu байт\n", (unsigned long)kr.elflen);

    size_t total = sizeof(kdrv_header_t) + kr.elflen;
    u8 *pkg = ka_new(total);
    kdrv_header_t *h = (kdrv_header_t *)pkg;
    h->magic = KDRV_MAGIC; h->format = KDRV_FORMAT; h->hdr_size = sizeof *h;
    memcpy(h->name, R.name, strlen(R.name));
    h->version = R.version; h->caps = R.caps; h->elf_size = (u32)kr.elflen; h->nmatch = R.nmatch;
    for (u32 i = 0; i < R.nmatch; i++) h->match[i] = R.match[i];
    memcpy(pkg + sizeof *h, kr.elf, kr.elflen);

    kdrv_rec_t rec;
    memset(&rec, 0, sizeof rec);
    rec.magic = KREC_MAGIC; rec.format = KDRV_FORMAT; rec.rec_size = sizeof rec;
    sha256(kr.elf, kr.elflen, rec.elf_sha);
    sha256(pkg, total, rec.pkg_sha);
    memcpy(rec.src_sha, set, 32);
    rec.caps_ok = R.caps;                        /* пользователь подтвердил именно эти права, введя хеш отчёта, где они перечислены */
    rec.confirmed = KREC_CONFIRMED;
    i64 now = k_time();
    rec.date = now > 0 ? (u64)now : 0;
    {   /* источник, затем после нуля метка лицензии «L:ИД» (читатели видят только адрес до нуля); адрес укорачивается, лицензия - нет */
        char lt[32];
        int ll = snprintf(lt, sizeof lt, "L:%s", R.license);
        size_t room = sizeof rec.source - 1 - (size_t)(ll > 0 && (size_t)ll < sizeof lt ? ll + 1 : 0), ul = strlen(url);
        if (ul > room) ul = room;
        memcpy(rec.source, url, ul);
        if (ll > 0 && (size_t)ll < sizeof lt) memcpy(rec.source + ul + 1, lt, (size_t)ll);
    }

    char f1[32], f2[32];
    snprintf(f1, sizeof f1, "%s.kdrv", R.name);
    snprintf(f2, sizeof f2, "%s.rec", R.name);
    i64 w = drv_op(KFS_DRV_WRITE, f1, pkg, total, 0);
    if (w < 0) { sayf("drvget: запись /drv/%s отклонена (код %lld)\n", f1, (long long)w); return 1; }
    w = drv_op(KFS_DRV_WRITE, f2, &rec, sizeof rec, 0);
    if (w < 0) { sayf("drvget: запись /drv/%s отклонена (код %lld)\n", f2, (long long)w); drv_op(KFS_DRV_REMOVE, f1, 0, 0, 0); return 1; }
    stage_clear();
    sayf("drvget: установлено: /drv/%s и /drv/%s; драйвер подойдёт устройствам из списка выше, ядро просматривает /drv сразу (журнал: [KIRA DRV])\n", f1, f2);
    return 0;
}

/* ---------- list / verify / remove ---------- */
static int read_pkg(const char *name, kdrv_header_t *h, kdrv_rec_t *rec, int *sha_ok, int *auth_state) {
    char f1[32], f2[32], f3[32];
    *auth_state = DGP_AUTH_UNKNOWN;
    snprintf(f1, sizeof f1, "%s.kdrv", name);
    snprintf(f2, sizeof f2, "%s.rec", name);
    snprintf(f3, sizeof f3, "%s.sig", name);
    i64 sz = drv_op(KFS_DRV_STAT, f1, 0, 0, 0);
    if (sz < (i64)sizeof *h) return -1;
    if (sz > (i64)sizeof body) { *sha_ok = -1; sz = (i64)sizeof body; }
    i64 got = drv_op(KFS_DRV_READ, f1, body, (u64)sz, 0);
    if (got != sz && *sha_ok != -1) return -1;
    memcpy(h, body, sizeof *h);
    memset(rec, 0, sizeof *rec);
    if (drv_op(KFS_DRV_READ, f2, rec, sizeof *rec, 0) != (i64)sizeof *rec) return -2;
    if (*sha_ok == -1) return 0;
    uint8_t d1[32], d2[32];
    int hc = kdrv_header_check(body, (uint32_t)sz, h);
    if (hc != KDRV_OK) { *sha_ok = 0; return 0; }
    sha256(body + sizeof *h, h->elf_size, d1);
    sha256(body, (size_t)sz, d2);
    *sha_ok = !memcmp(d1, rec->elf_sha, 32) && !memcmp(d2, rec->pkg_sha, 32) && kdrv_rec_check(rec, sizeof *rec, rec) == KDRV_OK;
    i64 sig_size = drv_op(KFS_DRV_STAT, f3, 0, 0, 0);
    if (sig_size == -2) *auth_state = DGP_AUTH_LOCAL;
    else if (sig_size < 0 || sig_size != (i64)sizeof(kdrv_sig_t)) *auth_state = DGP_AUTH_INVALID;
    else {
        kdrv_sig_t sig;
        if (drv_op(KFS_DRV_READ, f3, &sig, sizeof sig, 0) != (i64)sizeof sig) *auth_state = DGP_AUTH_INVALID;
        else {
            i64 now = k_time();
            int vr = kdrv_sig_verify(body, (size_t)sz, &sig, sizeof sig,
                                     kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, now);
            *auth_state = vr == KDRV_SIG_OK ? DGP_AUTH_PUBLISHER : DGP_AUTH_INVALID;
        }
    }
    return 0;
}

static void show_caps(u32 c) {
    static const struct { u32 b; const char *n; } t[] = { { CAP_MMIO, "MMIO" }, { CAP_IRQ, "IRQ" }, { CAP_DMA, "DMA" }, { CAP_PCIDEV, "PCIDEV" }, { CAP_NETDEV, "NETDEV" } };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (c & t[i].b) sayf(" %s", t[i].n);
    if (!c) say(" -");
}

static int cmd_list(void) {
    static char lst[4096];
    i64 n = drv_op(KFS_DRV_LIST, "", lst, sizeof lst - 1, 0);
    if (n < 0) { say("drvget: каталога /drv нет или он недоступен\n"); return 1; }
    lst[n] = 0;
    int cnt = 0;
    for (char *p = lst; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        size_t l = strlen(p);
        if (l > 5 && !strcmp(p + l - 5, ".kdrv")) {
            char nm[16];
            kdrv_header_t h; kdrv_rec_t rec; int ok = 0, auth = DGP_AUTH_UNKNOWN;
            snprintf(nm, sizeof nm, "%.*s", (int)(l - 5 > 11 ? 11 : l - 5), p);
            cnt++;
            int rc = read_pkg(nm, &h, &rec, &ok, &auth);
            if (rc == -1) sayf("  %s: пакет не читается\n", nm);
            else if (rc == -2) sayf("  %s: нет подтверждения (%s.rec) - не будет запущен\n", nm, nm);
            else { sayf("  %s: версия %u, права:", nm, (unsigned)h.version); show_caps(h.caps); sayf(", SHA-256 %s, издатель: %s, источник: %s\n", ok ? "сверен" : "НЕ СОВПАДАЕТ", auth == DGP_AUTH_PUBLISHER ? "Ed25519 подтверждён" : auth == DGP_AUTH_LOCAL ? "не подтверждён (локально)" : auth == DGP_AUTH_INVALID ? "ПОДПИСЬ ОТКЛОНЕНА" : "не проверена", rec.source); }
        }
        if (!e) break;
        p = e + 1;
    }
    sayf("drvget: пакетов в /drv: %d (работающие драйверы: команда drv)\n", cnt);
    return 0;
}

static int cmd_verify(const char *nm) {
    kdrv_header_t h; kdrv_rec_t rec; int ok = 0, auth = DGP_AUTH_UNKNOWN;
    if (!name_ok(nm)) { say("drvget: неверное имя\n"); return 2; }
    int rc = read_pkg(nm, &h, &rec, &ok, &auth);
    if (rc) { sayf("drvget: verify %s: пакета или подтверждения нет\n", nm); return 1; }
    char a[65], b[65], c[65];
    dg_hex(rec.elf_sha, 32, a); dg_hex(rec.pkg_sha, 32, b); dg_hex(rec.src_sha, 32, c);
    sayf("drvget: %s: SHA-256 образа %s, пакета %s, исходников %s\n", nm, a, b, c);
    sayf("drvget: подпись издателя: %s\n", auth == DGP_AUTH_PUBLISHER ? "Ed25519 подтверждена" : auth == DGP_AUTH_LOCAL ? "отсутствует; пакет только локально подтверждён" : auth == DGP_AUTH_INVALID ? "ОТКЛОНЕНА" : "не проверена");
    sayf("drvget: verify %s: %s\n", nm, ok == 1 ? "OK (хеши сходятся с локальной записью подтверждения)" : "ОШИБКА: хеши не сходятся или запись недействительна");
    return ok == 1 && auth != DGP_AUTH_INVALID && auth != DGP_AUTH_UNKNOWN ? 0 : 1;
}

static int cmd_remove(const char *nm) {
    char f[32];
    if (!name_ok(nm)) { say("drvget: неверное имя\n"); return 2; }
    snprintf(f, sizeof f, "%s.kdrv", nm);
    i64 r = drv_op(KFS_DRV_REMOVE, f, 0, 0, 0);
    if (r < 0) { sayf("drvget: remove %s: пакета нет (код %lld)\n", nm, (long long)r); return 1; }
    sayf("drvget: пакет %s удалён из /drv вместе с подтверждением. Если драйвер работает, остановите его: drv stop %s\n", nm, nm);
    return 0;
}


#ifndef DRVGET_LIB
static int parse_pci_id(const char *s, uint16_t *vendor, uint16_t *device) {
    if (!s || strlen(s) != 9 || s[4] != ':') return 0;
    char a[5], b[5]; u8 av[2], bv[2];
    memcpy(a, s, 4); a[4] = 0; memcpy(b, s + 5, 4); b[4] = 0;
    if (!dg_hex_parse(a, av, 2) || !dg_hex_parse(b, bv, 2)) return 0;
    *vendor = (uint16_t)(((uint16_t)av[0] << 8) | av[1]);
    *device = (uint16_t)(((uint16_t)bv[0] << 8) | bv[1]);
    return 1;
}
static int cmd_match(const char *arg) {
    uint16_t vendor, device;
    if (!parse_pci_id(arg, &vendor, &device)) { say("использование: drvget match VVVV:DDDD (например 10ec:8139)\n"); return 2; }
    char name[12], url[DG_URL_MAX];
    int rc = drvget_match_pci(vendor, device, name, sizeof name, url, sizeof url);
    if (rc < 0) { say("drvget: системный каталог отсутствует или повреждён\n"); return 1; }
    if (!rc) { sayf("drvget: для PCI %04x:%04x подходящий рецепт в системном каталоге не найден\n", vendor, device); return 1; }
    sayf("drvget match %04x:%04x: %s\nрецепт: %s\n", vendor, device, name, url);
    return 0;
}
#endif /* !DRVGET_LIB: CLI-only match command */

static int cmd_base(const char *arg) {
    char b[DG_URL_MAX];
    if (!*arg) { int own = base_url(b, sizeof b); sayf("drvget: каталог рецептов: %s%s\n", b, own ? "" : " (по умолчанию)"); return 0; }
    dg_url_t u;
    const char *err = "";
    if (dg_url_parse(arg, &u, &err)) { sayf("drvget: %s\n", err); return 1; }
    if (k_file_write("base", arg, strlen(arg)) < 0) { say("drvget: не удалось сохранить\n"); return 1; }
    sayf("drvget: каталог рецептов теперь %s (рецепт ИМЯ ищется как %s/ИМЯ/drvget.rec)\n", arg, arg);
    return 0;
}

#ifdef DRVGET_LIB
/* ---------- та же логика для окна «Драйверы» (userland/drvman.c, userland/drvget_api.h): без разбора командной строки ---------- */
#include "drvget_api.h"
void drvget_set_sink(void (*f)(const char *, size_t)) { sink = f; }
int drvget_find(const char *arg) { return cmd_install(arg); }
const dg_recipe_t *drvget_staged(void) { return g_staged_hash[0] ? &R : 0; }
const char *drvget_staged_hash(void) { return g_staged_hash; }
const char *drvget_staged_url(void) { return g_staged_url; }
int drvget_install_staged(void) {
    char a[16 + 66];
    if (!g_staged_hash[0]) { say("drvget: нечего ставить: сначала «Найти и показать»\n"); return 1; }
    snprintf(a, sizeof a, "%s %s", R.name, g_staged_hash);   /* полный хеш набора, показанный в отчёте; confirm заново пересчитывает набор из песочницы */
    return cmd_confirm(a);
}
int drvget_remove(const char *name) { return cmd_remove(name); }
int drvget_verify(const char *name) { return cmd_verify(name); }
int drvget_list(void) { return cmd_list(); }
int drvget_base(const char *url) { return cmd_base(url); }
int drvget_ctl(int start, const char *name) {
    char f[32];
    if (!name_ok(name)) { say("drvget: неверное имя\n"); return -1; }
    snprintf(f, sizeof f, "%s.kdrv", name);
    return (int)drv_op(start ? KFS_DRV_START : KFS_DRV_STOP, f, 0, 0, 0);
}
int drvget_pkgs(drvget_pkg_t *out, int max) {
    static char lst[4096];
    i64 n = drv_op(KFS_DRV_LIST, "", lst, sizeof lst - 1, 0);
    if (n < 0) return -1;
    lst[n] = 0;
    int cnt = 0;
    for (char *p = lst; *p && cnt < max;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        size_t l = strlen(p);
        if (l > 5 && l - 5 <= KDRV_NAME_MAX && !strcmp(p + l - 5, ".kdrv")) {
            kdrv_header_t h; kdrv_rec_t rec; int ok = 0, auth = DGP_AUTH_UNKNOWN;
            drvget_pkg_t *o = &out[cnt++];
            memset(o, 0, sizeof *o);
            memcpy(o->name, p, l - 5);
            int rc = read_pkg(o->name, &h, &rec, &ok, &auth);
            o->auth_state = auth;
            o->state = rc == -1 ? DGP_UNREADABLE : rc == -2 ? DGP_NOREC : ok == 1 ? DGP_OK : ok == -1 ? DGP_TOOBIG : DGP_BAD;
            if (o->state == DGP_BAD && kdrv_rec_check(&rec, sizeof rec, 0) == KDRV_E_UNCONFIRMED) o->state = DGP_UNCONF;
            if (rc == 0 || rc == -2) {
                o->version = h.version; o->caps = h.caps; o->nmatch = h.nmatch > KDRV_MATCH_MAX ? 0 : h.nmatch;
                for (uint32_t i = 0; i < o->nmatch; i++) o->match[i] = h.match[i];
            }
            if (rc == 0) {
                o->caps_ok = rec.caps_ok;
                memcpy(o->elf_sha, rec.elf_sha, 32); memcpy(o->src_sha, rec.src_sha, 32);
                size_t sl = 0;
                while (sl < sizeof rec.source && rec.source[sl]) sl++;
                memcpy(o->source, rec.source, sl < sizeof o->source - 1 ? sl : sizeof o->source - 1);
                if (sl + 3 < sizeof rec.source && rec.source[sl + 1] == 'L' && rec.source[sl + 2] == ':') {      /* метка лицензии после нуля */
                    char lic[24] = { 0 };
                    for (size_t k = 0; k < sizeof lic - 1 && sl + 3 + k < sizeof rec.source && rec.source[sl + 3 + k]; k++) lic[k] = rec.source[sl + 3 + k];
                    if (dg_license_ok(lic)) memcpy(o->license, lic, sizeof lic);
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return cnt;
}
#else
int umain(void) {
    char args[200];
    i64 al = k_args(args, sizeof args - 1);
    if (al < 0) al = 0;
    args[al] = 0;
    char *p = args;
    while (*p == ' ') p++;
    char *verb = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    if (!strcmp(verb, "install") && *p) return cmd_install(p);
    if (!strcmp(verb, "match") && *p) return cmd_match(p);
    if (!strcmp(verb, "confirm") && *p) return cmd_confirm(p);
    if (!strcmp(verb, "list")) return cmd_list();
    if (!strcmp(verb, "verify") && *p) return cmd_verify(p);
    if (!strcmp(verb, "remove") && *p) return cmd_remove(p);
    if (!strcmp(verb, "base")) return cmd_base(p);
    say("использование:\n"
        "  drvget match VVVV:DDDD                       подбор рецепта по PCI ID из системного каталога\n"
        "  drvget install ИМЯ|https://адрес/drvget.rec   скачать исходники, проверить лицензию, показать источник, SHA-256 и права (ничего не ставит)\n"
        "  drvget confirm ИМЯ ХЕШ                        собрать на устройстве и установить, если ХЕШ (8+ знаков) - SHA-256 набора из отчёта\n"
        "  drvget list | verify ИМЯ | remove ИМЯ         пакеты в /drv\n"
        "  drvget base [https://адрес]                   каталог рецептов (по умолчанию " DRVGET_DEFAULT_BASE ")\n"
        "GPL-код и бинарные драйверы (Windows .sys, модули Linux .ko) не принимаются.\n");
    return 2;
}
#endif
