/* drvget: разбор рецепта, адресов, проверка исходников и лицензии (см. drvget_core.h). Без libc и системных вызовов. */
#include "drvget_core.h"
#include "../immune.h"

_Static_assert(sizeof(sha256_ctx_t) <= sizeof(dg_hash_t), "dg_hash_t мал для sha256_ctx_t");

static size_t sl(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static int eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char* s, const char* p) { while (*p) { if (*s != *p) return 0; s++; p++; } return 1; }
static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int isl(char c) { c = low(c); return c >= 'a' && c <= 'z'; }
static int isd(char c) { return c >= '0' && c <= '9'; }

int dg_license_ok(const char* id) {
    return eq(id, "BSD-2-Clause") || eq(id, "BSD-3-Clause") || eq(id, "MIT") || eq(id, "ISC") || eq(id, "LicenseRef-Greenman");
}

int dg_license_matches(const char* declared, const char* detected) {
    return declared && detected && eq(declared, detected);
}

int dg_file_name_ok(const char* n) {
    size_t k = sl(n);
    if (k < 3 || k > DG_FILE_NAME) return 0;
    if (!(n[k - 2] == '.' && (n[k - 1] == 'c' || n[k - 1] == 'h'))) return 0;      /* только исходники C: ни .sys/.inf/.dll/.exe, ни .ko/.o/.bin */
    for (size_t i = 0; i < k; i++) {
        char c = n[i];
        int al = isl(c) || isd(c);
        if (i == 0 ? !al : !(al || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int dg_url_parse(const char* s, dg_url_t* u, const char** err) {
    static const char* const bad[] = { "http://", "ftp://", "file://", "ssh://", "git://", "data:", 0 };
    for (int i = 0; bad[i]; i++) if (starts(s, bad[i])) { *err = "разрешён только https:// (без шифрования и проверки сервера исходники не берутся)"; return -1; }
    if (!starts(s, "https://")) { *err = "адрес должен начинаться с https://"; return -1; }
    s += 8;
    size_t n = 0;
    while (s[n] && s[n] != '/' && s[n] != ':' && n < sizeof u->host - 1) {
        char c = s[n];
        if (!(isl(c) || isd(c) || c == '.' || c == '-')) { *err = "недопустимое имя узла"; return -1; }
        u->host[n] = c; n++;
    }
    if (!n || (s[n] && s[n] != '/' && s[n] != ':')) { *err = "недопустимое имя узла"; return -1; }
    u->host[n] = 0;
    u->port = 443;
    s += n;
    if (*s == ':') {
        s++; u->port = 0;
        if (!isd(*s)) { *err = "неверный порт"; return -1; }
        while (isd(*s)) { u->port = u->port * 10 + (unsigned)(*s - '0'); if (u->port > 65535) { *err = "неверный порт"; return -1; } s++; }
    }
    if (*s && *s != '/') { *err = "неверный адрес"; return -1; }
    size_t pl = 0;
    if (!*s) { u->path[pl++] = '/'; }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 32 || c == 127 || c == '#' || c == '\\') { *err = "недопустимый знак в пути"; return -1; }
        if (pl + 1 >= sizeof u->path) { *err = "адрес слишком длинный"; return -1; }
        u->path[pl++] = (char)c;
    }
    u->path[pl] = 0;
    return 0;
}

int dg_url_join(const dg_url_t* b, const char* rel, char* out, size_t cap, const char** err) {
    size_t n = 0;
    if (starts(rel, "https://") || starts(rel, "http://") || starts(rel, "ftp://") || starts(rel, "file://")) {
        if (sl(rel) + 1 > cap) { *err = "адрес слишком длинный"; return -1; }
        for (; rel[n]; n++) out[n] = rel[n];
        out[n] = 0;
        return 0;
    }
    char tmp[DG_URL_MAX];
    size_t t = 0;
    if (rel[0] == '/') { for (size_t i = 0; rel[i] && t + 1 < sizeof tmp; i++) tmp[t++] = rel[i]; }
    else {
        size_t last = 0;
        for (size_t i = 0; b->path[i]; i++) if (b->path[i] == '/') last = i;
        for (size_t i = 0; i <= last && t + 1 < sizeof tmp; i++) tmp[t++] = b->path[i];
        for (size_t i = 0; rel[i] && t + 1 < sizeof tmp; i++) tmp[t++] = rel[i];
    }
    tmp[t] = 0;
    for (size_t i = 0; tmp[i]; i++) if (tmp[i] == '.' && tmp[i + 1] == '.' && (i == 0 || tmp[i - 1] == '/')) { *err = "«..» в адресе не допускается"; return -1; }
    const char* pre = "https://";
    size_t need = 8 + sl(b->host) + (b->port != 443 ? 6 : 0) + t + 1;
    if (need > cap) { *err = "адрес слишком длинный"; return -1; }
    for (; pre[n]; n++) out[n] = pre[n];
    for (size_t i = 0; b->host[i]; i++) out[n++] = b->host[i];
    if (b->port != 443) {
        out[n++] = ':';
        char d[6]; int k = 0; unsigned p = b->port;
        do { d[k++] = (char)('0' + p % 10); p /= 10; } while (p);
        while (k) out[n++] = d[--k];
    }
    for (size_t i = 0; i < t; i++) out[n++] = tmp[i];
    out[n] = 0;
    return 0;
}

int dg_hex_parse(const char* s, uint8_t* out, int n) {
    for (int i = 0; i < n * 2; i++) {
        char c = low(s[i]);
        if (!(isd(c) || (c >= 'a' && c <= 'f'))) return 0;
    }
    if (s[n * 2]) return 0;
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = low(s[i * 2 + k]); v = v * 16 + (isd(c) ? c - '0' : c - 'a' + 10); }
        out[i] = (uint8_t)v;
    }
    return 1;
}

void dg_hex(const uint8_t* d, int n, char* out) {
    for (int i = 0; i < n; i++) { out[2 * i] = "0123456789abcdef"[d[i] >> 4]; out[2 * i + 1] = "0123456789abcdef"[d[i] & 15]; }
    out[2 * n] = 0;
}

int dg_hex_prefix(const char* typed, const uint8_t sha[32]) {
    char h[65]; dg_hex(sha, 32, h);
    size_t k = sl(typed);
    if (k < 8 || k > 64) return 0;
    for (size_t i = 0; i < k; i++) if (low(typed[i]) != h[i]) return 0;
    return 1;
}

void dg_set_begin(dg_hash_t* h) { sha256_init((sha256_ctx_t*)h); }
void dg_set_add(dg_hash_t* h, const char* name, const uint8_t* d, uint32_t len) {
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)((uint64_t)len >> (8 * i));
    sha256_update((sha256_ctx_t*)h, name, sl(name) + 1);
    sha256_update((sha256_ctx_t*)h, l, 8);
    sha256_update((sha256_ctx_t*)h, d, len);
}
void dg_set_end(dg_hash_t* h, uint8_t out[32]) { sha256_final((sha256_ctx_t*)h, out); }

/* ---- рецепт ---- */
static int parse_cap(const char* w, size_t n, uint32_t* out) {
    static const struct { const char* n; uint32_t c; } t[] = { { "mmio", CAP_MMIO }, { "irq", CAP_IRQ }, { "dma", CAP_DMA }, { "pcidev", CAP_PCIDEV }, { "netdev", CAP_NETDEV } };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (sl(t[i].n) == n && starts(w, t[i].n)) { *out |= t[i].c; return 1; }
    return 0;
}
static int hexn(const char* s, size_t n, uint32_t* v) {
    if (!n || n > 8) return 0;
    *v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = low(s[i]);
        if (!(isd(c) || (c >= 'a' && c <= 'f'))) return 0;
        *v = *v * 16 + (uint32_t)(isd(c) ? c - '0' : c - 'a' + 10);
    }
    return 1;
}

int dg_parse_recipe(const char* text, uint32_t len, dg_recipe_t* r, const char** err) {
    char* z = (char*)r; for (size_t i = 0; i < sizeof *r; i++) z[i] = 0;
    if (len == 0 || len > DG_RECIPE_MAX) { *err = "рецепт пуст или слишком велик"; return -1; }
    int header = 0;
    uint32_t pos = 0;
    while (pos < len) {
        char line[400]; size_t ln = 0;
        while (pos < len && text[pos] != '\n') { if (ln + 1 >= sizeof line) { *err = "строка рецепта слишком длинна"; return -1; } if (text[pos] == 0) { *err = "нулевой байт в рецепте"; return -1; } line[ln++] = text[pos++]; }
        pos++;
        while (ln && (line[ln - 1] == '\r' || line[ln - 1] == ' ' || line[ln - 1] == '\t')) ln--;
        line[ln] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char* w[8]; size_t wl[8]; int nw = 0;
        while (*p && nw < 8) {
            w[nw] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            wl[nw] = (size_t)(p - w[nw]);
            if (*p) *p++ = 0;
            while (*p == ' ' || *p == '\t') p++;
            nw++;
        }
        if (*p) { *err = "слишком много слов в строке рецепта"; return -1; }
        const char* k = w[0];
        if (!header) {
            if (!(eq(k, "drvget-recipe") && nw == 2 && eq(w[1], "1"))) { *err = "первая строка рецепта должна быть «drvget-recipe 1»"; return -1; }
            header = 1; continue;
        }
        if (eq(k, "name") && nw == 2) {
            if (wl[1] < 1 || wl[1] > KDRV_NAME_MAX) { *err = "имя драйвера: 1..11 знаков"; return -1; }
            for (size_t i = 0; i < wl[1]; i++) { char c = w[1][i]; if (!(isl(c) && c == low(c)) && !isd(c) && c != '-' && c != '_') { *err = "имя драйвера: только a-z 0-9 - _"; return -1; } r->name[i] = c; }
        } else if (eq(k, "version") && nw == 2) {
            uint32_t v = 0; if (!isd(w[1][0]) || wl[1] > 9) { *err = "версия - число"; return -1; }
            for (size_t i = 0; i < wl[1]; i++) { if (!isd(w[1][i])) { *err = "версия - число"; return -1; } v = v * 10 + (uint32_t)(w[1][i] - '0'); }
            r->version = v;
        } else if (eq(k, "license") && nw == 2) {
            if (!dg_license_ok(w[1])) { *err = "лицензия вне списка (BSD-2-Clause, BSD-3-Clause, MIT, ISC, LicenseRef-Greenman); GPL и прочее не берётся"; return -1; }
            for (size_t i = 0; i <= wl[1]; i++) r->license[i] = w[1][i];
        } else if (eq(k, "pci") && nw == 2) {
            if (r->nmatch >= KDRV_MATCH_MAX) { *err = "слишком много строк pci/class"; return -1; }
            const char* c = w[1]; size_t cp = 0; while (c[cp] && c[cp] != ':') cp++;
            uint32_t v, d;
            if (!c[cp] || !hexn(c, cp, &v) || !hexn(c + cp + 1, sl(c + cp + 1), &d) || v > 0xFFFF || d > 0xFFFF || v == 0xFFFF || d == 0xFFFF) { *err = "pci: ожидается VVVV:DDDD (шестнадцатеричные, не FFFF)"; return -1; }
            kdrv_match_t* m = &r->match[r->nmatch++]; m->vendor = (uint16_t)v; m->device = (uint16_t)d; m->cls = 0; m->cls_mask = 0;
        } else if (eq(k, "class") && nw == 2) {
            if (r->nmatch >= KDRV_MATCH_MAX) { *err = "слишком много строк pci/class"; return -1; }
            const char* c = w[1]; size_t cp = 0; while (c[cp] && c[cp] != '/') cp++;
            uint32_t cl, mk;
            if (!c[cp] || !hexn(c, cp, &cl) || !hexn(c + cp + 1, sl(c + cp + 1), &mk) || cl > 0xFFFFFF || mk > 0xFFFFFF || !mk) { *err = "class: ожидается КЛАСС/МАСКА (шестнадцатеричные, 24 бита, маска не нуль)"; return -1; }
            kdrv_match_t* m = &r->match[r->nmatch++]; m->vendor = KDRV_ANY; m->device = KDRV_ANY; m->cls = cl; m->cls_mask = mk;
        } else if (eq(k, "caps") && nw >= 2) {
            for (int i = 1; i < nw; i++) if (!parse_cap(w[i], wl[i], &r->caps)) { *err = "caps: допустимы mmio irq dma pcidev netdev"; return -1; }
        } else if (eq(k, "main") && nw == 2) {
            if (!dg_file_name_ok(w[1])) { *err = "main: неверное имя файла"; return -1; }
            for (size_t i = 0; i <= wl[1]; i++) r->main_file[i] = w[1][i];
        } else if (eq(k, "file") && (nw == 3 || (nw == 5 && eq(w[3], "sha256")))) {
            if (r->nfile >= DG_FILES) { *err = "слишком много файлов в рецепте"; return -1; }
            if (!dg_file_name_ok(w[1])) { *err = "file: имя - только исходник .c или .h, буквы, цифры, . - _ (бинарные файлы не берутся)"; return -1; }
            for (uint32_t i = 0; i < r->nfile; i++) if (eq(r->file[i].name, w[1])) { *err = "файл указан дважды"; return -1; }
            if (wl[2] + 1 > DG_URL_MAX) { *err = "адрес файла слишком длинный"; return -1; }
            uint32_t f = r->nfile++;
            for (size_t i = 0; i <= wl[1]; i++) r->file[f].name[i] = w[1][i];
            for (size_t i = 0; i <= wl[2]; i++) r->file[f].url[i] = w[2][i];
            if (nw == 5) {
                uint8_t t[32];
                if (!dg_hex_parse(w[4], t, 32)) { *err = "sha256: ожидается 64 шестнадцатеричных знака"; return -1; }
                for (int i = 0; i < 65; i++) r->file[f].sha[i] = low(w[4][i]);
            }
        } else { *err = "неизвестная или неполная строка рецепта"; return -1; }
    }
    if (!header) { *err = "нет строки drvget-recipe 1"; return -1; }
    if (!r->name[0]) { *err = "нет name"; return -1; }
    if (!r->license[0]) { *err = "нет license"; return -1; }
    if (!r->version) { *err = "нет version (>= 1)"; return -1; }
    if (!r->nmatch) { *err = "нет ни одной строки pci или class"; return -1; }
    if (!r->caps) { *err = "нет caps"; return -1; }
    if (r->caps & ~(uint32_t)CAP_DRIVER_) { *err = "caps вне прав драйвера"; return -1; }
    if (!r->nfile || !r->main_file[0]) { *err = "нет file или main"; return -1; }
    int found = 0;
    for (uint32_t i = 0; i < r->nfile; i++) if (eq(r->file[i].name, r->main_file)) found = 1;
    if (!found) { *err = "main не назван среди file"; return -1; }
    if (r->main_file[sl(r->main_file) - 1] != 'c') { *err = "main должен быть файлом .c"; return -1; }
    return 0;
}

/* ---- проверка исходного файла ---- */
static const char* find_ci(const uint8_t* d, uint32_t len, const char* pat) {       /* поиск без учёта регистра */
    size_t pl = sl(pat);
    for (uint32_t i = 0; i + pl <= len; i++) {
        size_t k = 0;
        while (k < pl && low((char)d[i + k]) == low(pat[k])) k++;
        if (k == pl) return (const char*)d + i;
    }
    return 0;
}

/* Поиск фразы без учёта регистра, где пробел шаблона = любая серия пробелов, переводов строки и украшений комментария (* /): текст лицензии в исходниках перенесён по строкам. */
static int sepc(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '*' || c == '/'; }
static int find_wrapped(const uint8_t* d, uint32_t len, const char* pat) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t k = i; const char* q = pat; int ok = 1;
        while (*q) {
            if (*q == ' ') { if (k >= len || !sepc(d[k])) { ok = 0; break; } while (k < len && sepc(d[k])) k++; q++; }
            else { if (k >= len || low((char)d[k]) != low(*q)) { ok = 0; break; } k++; q++; }
        }
        if (ok) return 1;
    }
    return 0;
}
int dg_scan_source(const char* fname, const uint8_t* d, uint32_t len, char lic[24], const char** err) {
    (void)fname;
    lic[0] = 0;
    if (len == 0) { *err = "пустой файл"; return -1; }
    if (len > DG_SRC_MAX) { *err = "файл слишком велик"; return -1; }
    if ((len > 1 && d[0] == 'M' && d[1] == 'Z') || (len > 3 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') ||
        (len > 3 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) || (len > 7 && starts((const char*)d, "!<arch>\n")) ||
        (len > 3 && d[0] == 0x1f && d[1] == 0x8b)) { *err = "это бинарник или архив (Windows .sys/.dll, модуль ELF, zip, gzip), а не исходный текст - отвергнут"; return -1; }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = d[i];
        if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f') { *err = "в файле управляющие или нулевые байты: это не исходный текст - отвергнут"; return -1; }
    }
    /* GPL: любое упоминание слова GPL / GNU General Public License / Lesser / Affero отвергает файл целиком (двойная лицензия тоже: чище не брать). */
    for (uint32_t i = 0; i + 3 <= len; i++) {
        if (low((char)d[i]) == 'g' && low((char)d[i + 1]) == 'p' && low((char)d[i + 2]) == 'l' && (i == 0 || !isl((char)d[i - 1])))
        { *err = "в тексте есть GPL: код под GPL в образ не берётся (решение проекта) - отвергнут"; return -1; }
    }
    if (find_ci(d, len, "lgpl") || find_ci(d, len, "agpl") || find_ci(d, len, "general public license") || find_ci(d, len, "lesser general") || find_ci(d, len, "affero") || find_ci(d, len, "gnu library"))
    { *err = "в тексте есть GNU General/Lesser Public License: отвергнут"; return -1; }
    if (find_wrapped(d, len, "all advertising materials mentioning")) { *err = "рекламная оговорка (BSD с 4 пунктами): вне списка лицензий"; return -1; }
    if (find_ci(d, len, "proprietary") || find_ci(d, len, "all rights reserved. confidential")) { *err = "признаки закрытой лицензии: отвергнут"; return -1; }
    const char* sp = find_ci(d, len, "SPDX-License-Identifier:");
    if (sp) {
        const char* e = (const char*)d + len;
        if (find_ci((const uint8_t*)sp + 24, (uint32_t)(e - (sp + 24)), "SPDX-License-Identifier:")) {
            *err = "в файле несколько SPDX-объявлений: неоднозначная лицензия - отказ";
            return -1;
        }
        const char* p = sp + 24;
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        char id[32]; size_t n = 0;
        while (p < e && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' && *p != '*' && n + 1 < sizeof id) id[n++] = *p++;
        id[n] = 0;
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        if (p < e && *p != '\n' && *p != '\r' && !(p + 1 < e && p[0] == '*' && p[1] == '/')) { *err = "SPDX: составное выражение (OR/AND/WITH) не принимается - нужен один идентификатор из списка"; return -1; }
        if (!dg_license_ok(id)) { *err = "SPDX-идентификатор вне списка (BSD-2-Clause, BSD-3-Clause, MIT, ISC, LicenseRef-Greenman)"; return -1; }
        for (size_t i = 0; i <= n; i++) lic[i] = id[i];
        return 0;
    }
    if (find_wrapped(d, len, "Redistribution and use in source and binary forms") && find_wrapped(d, len, "THIS SOFTWARE IS PROVIDED")) {
        const char* t = find_wrapped(d, len, "neither the name of") ? "BSD-3-Clause" : "BSD-2-Clause";
        for (int i = 0; ; i++) { lic[i] = t[i]; if (!t[i]) break; }
        return 0;
    }
    if (find_ci(d, len, "Permission is hereby granted, free of charge") && find_ci(d, len, "THE SOFTWARE IS PROVIDED \"AS IS\"")) {
        const char* t = "MIT"; for (int i = 0; ; i++) { lic[i] = t[i]; if (!t[i]) break; }
        return 0;
    }
    if (find_ci(d, len, "Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted")) {
        const char* t = "ISC"; for (int i = 0; ; i++) { lic[i] = t[i]; if (!t[i]) break; }
        return 0;
    }
    /* Короткая разрешительная лицензия Дэвида Гринмана (dp8390, ne2000 в OpenBSD/NetBSD): использование, изменение, копирование, распространение и продажа
       разрешены при сохранении уведомления. Принимается только точный текст обеих фраз (не «похожее»). Решение проекта: list допустимых лицензий расширен этим случаем. */
    if (find_wrapped(d, len, "may be used, modified, copied, distributed, and sold, in both source and binary form provided that the above copyright and these terms are retained") &&
        find_wrapped(d, len, "Under no circumstances is the author responsible for the proper functioning of this software")) {
        const char* t = "LicenseRef-Greenman"; for (int i = 0; ; i++) { lic[i] = t[i]; if (!t[i]) break; }
        return 0;
    }
    *err = "лицензия файла не найдена (нужны строка SPDX-License-Identifier из списка или текст BSD/MIT/ISC) - не берём";
    return -1;
}


/* Read-only PCI catalogue: SHA-256 pins the recipe; each recipe can pin its source files. */
static int ix_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int ix_name_ok(const char *s, size_t n) {
    if (!n || n > 11) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return 0;
    return 1;
}
int dg_parse_index(const char *text, uint32_t len, dg_index_entry_t *out, uint32_t cap, uint32_t *count, const char **err) {
    if (!text || !out || !count || !cap || len > DG_INDEX_MAX) { if (err) *err = "invalid catalogue size or buffer"; return -1; }
    *count = 0;
    int header = 0;
    uint32_t pos = 0;
    while (pos < len) {
        char line[400];
        size_t ln = 0;
        while (pos < len && text[pos] != '\n') {
            if (ln + 1 >= sizeof line) { if (err) *err = "catalogue line too long"; return -1; }
            line[ln++] = text[pos++];
        }
        if (pos < len) pos++;
        while (ln && (line[ln - 1] == '\r' || line[ln - 1] == ' ' || line[ln - 1] == '\t')) ln--;
        line[ln] = 0;
        char *p = line;
        while (ix_space(*p)) p++;
        if (!*p || *p == '#') continue;
        char *w[6];
        uint32_t nw = 0;
        while (*p) {
            if (nw >= 6) { if (err) *err = "too many catalogue fields"; return -1; }
            w[nw++] = p;
            while (*p && !ix_space(*p)) p++;
            if (*p) *p++ = 0;
            while (ix_space(*p)) p++;
        }
        if (!header) {
            if (nw != 2 || !eq(w[0], "kira-driver-index") || !eq(w[1], "1")) {
                if (err) { *err = "unsupported catalogue version"; }
                return -1;
            }
            header = 1;
            continue;
        }
        if ((nw != 5 && nw != 6) || !eq(w[0], "pci")) { if (err) *err = "expected pci VVVV:DDDD name url sha256 [publisher-required]"; return -1; }
        if (nw == 6 && !eq(w[5], "publisher-required")) { if (err) *err = "unknown catalogue publisher policy"; return -1; }
        if (sl(w[1]) != 9 || w[1][4] != ':') { if (err) *err = "invalid PCI ID"; return -1; }
        uint32_t vendor = 0, device = 0;
        if (!hexn(w[1], 4, &vendor) || !hexn(w[1] + 5, 4, &device)) { if (err) *err = "invalid hexadecimal PCI ID"; return -1; }
        size_t name_len = sl(w[2]);
        if (!ix_name_ok(w[2], name_len)) { if (err) *err = "invalid driver name"; return -1; }
        if (sl(w[3]) >= DG_URL_MAX) { if (err) *err = "recipe URL too long"; return -1; }
        dg_url_t parsed;
        if (dg_url_parse(w[3], &parsed, err)) return -1;
        uint8_t digest[32];
        if (!dg_hex_parse(w[4], digest, 32)) { if (err) *err = "invalid recipe SHA-256"; return -1; }
        if (*count >= cap || *count >= DG_INDEX_ENTRIES) { if (err) *err = "too many catalogue entries"; return -1; }
        for (uint32_t i = 0; i < *count; i++) {
            if (out[i].vendor == (uint16_t)vendor && out[i].device == (uint16_t)device) {
                if (err) *err = "duplicate PCI ID";
                return -1;
            }
            if (eq(out[i].name, w[2])) { if (err) *err = "duplicate driver name"; return -1; }
        }
        dg_index_entry_t *e = &out[(*count)++];
        e->vendor = (uint16_t)vendor;
        e->device = (uint16_t)device;
        for (size_t j = 0; j < name_len; j++) e->name[j] = w[2][j];
        e->name[name_len] = 0;
        size_t url_len = sl(w[3]);
        for (size_t j = 0; j < url_len; j++) e->url[j] = w[3][j];
        e->url[url_len] = 0;
        for (size_t j = 0; j < 64; j++) e->recipe_sha[j] = w[4][j];
        e->recipe_sha[64] = 0;
        e->publisher_required = (uint8_t)(nw == 6);
    }
    if (!header) { if (err) *err = "missing catalogue header"; return -1; }
    return 0;
}
int dg_index_find_pci(const dg_index_entry_t *entries, uint32_t count, uint16_t vendor, uint16_t device) {
    if (!entries) return -1;
    for (uint32_t i = 0; i < count; i++) if (entries[i].vendor == vendor && entries[i].device == device) return (int)i;
    return -1;
}
int dg_index_find_name(const dg_index_entry_t *entries, uint32_t count, const char *name) {
    if (!entries || !name) return -1;
    for (uint32_t i = 0; i < count; i++) if (eq(entries[i].name, name)) return (int)i;
    return -1;
}
