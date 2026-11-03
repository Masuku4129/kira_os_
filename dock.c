#include "dock.h"
#include "icons.h"
#include "theme.h"

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int dock_valid_name(const char* s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= DOCK_NAME - 1 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return n > 0;
}

int dock_find(const dock_t* d, const char* name) {
    for (int i = 0; i < d->n; i++) if (same(d->name[i], name)) return i;
    return -1;
}

int dock_add(dock_t* d, const char* name) {
    if (!dock_valid_name(name)) return -1;
    if (dock_find(d, name) >= 0) return 1;
    if (d->n >= DOCK_MAX) return -1;
    int i = 0; while (name[i]) { d->name[d->n][i] = name[i]; i++; } d->name[d->n][i] = 0;
    d->n++;
    return 0;
}

int dock_remove(dock_t* d, const char* name) {
    int k = dock_find(d, name);
    if (k < 0) return -1;
    for (int i = k; i + 1 < d->n; i++) for (int j = 0; j < DOCK_NAME; j++) d->name[i][j] = d->name[i + 1][j];
    d->n--;
    return 0;
}

void dock_default(dock_t* d) { d->n = 0; dock_add(d, "plasma"); dock_add(d, "paint"); dock_add(d, "cube3d"); dock_add(d, "edit"); dock_add(d, "drvman"); }

int dock_parse(dock_t* d, const char* t, unsigned len) {
    d->n = 0;
    char cur[DOCK_NAME + 1]; unsigned k = 0; int bad = 0;
    for (unsigned i = 0; i <= len; i++) {
        char c = i < len ? t[i] : '\n';
        if (c == '\n' || c == '\r') {
            cur[k] = 0;
            if (k && !bad) dock_add(d, cur);
            k = 0; bad = 0;
        } else if (k < DOCK_NAME) cur[k++] = c;
        else bad = 1;
    }
    return d->n;
}

unsigned dock_format(const dock_t* d, char* out, unsigned cap) {
    unsigned p = 0;
    for (int i = 0; i < d->n; i++) {
        for (int j = 0; d->name[i][j] && p + 2 < cap; j++) out[p++] = d->name[i][j];
        if (p + 1 < cap) out[p++] = '\n';
    }
    if (p < cap) out[p] = 0;
    return p;
}

static int starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }

/* Единая таблица. Порядок не важен; системное имя - ключ. Добавляя программу в /bin, добавляйте строку сюда. */
static const app_info_t apps[] = {
    { "plasma",  "Плазма",                        "Живая плазма на весь экран",                          ICON_WAVE,     TH_PINK   },
    { "paint",   "Рисование",                     "Рисование мышью, холст сохраняется",          ICON_BRUSH,    TH_BLUE   },
    { "cube3d",  "3D-куб",                        "Вращающийся 3D-куб, расчёт на процессоре",            ICON_CUBE,     TH_CYAN   },
    { "wasm",    "Запуск WebAssembly",            "Исполняет модули .wasm (интерпретатор и JIT)",        ICON_WASM,     TH_PURPLE },
    { "edit",    "Текстовый редактор",            "Правка текстовых файлов",                             ICON_EDIT,     TH_GREEN  },
    { "view",    "Просмотр",                      "Картинки (PNG, JPEG, GIF, BMP, PPM) и текст",         ICON_EYE,      TH_CYAN   },
    { "fetch",   "Загрузка из сети",              "Запрос HTTP или HTTPS (TLS 1.3), ответ в файл",                    ICON_DOWNLOAD, TH_BLUE   },
    { "serve",   "Веб-сервер",                    "Маленький HTTP-сервер на сети ядра (порт 8080)",                  ICON_SERVER,   TH_AMBER  },
    { "rec",     "Запись звука",                  "Запись со входа звуковой карты, файл WAV",                           ICON_MIC,      TH_RED    },
    { "net6",    "Сеть IPv6",                     "Проверка IPv6: датаграмма UDP и запрос TCP",                 ICON_NET,      TH_GREEN  },
    { "kcc",     "Компилятор C",                  "Собирает программы на C прямо на Кире",               ICON_BRACES,   TH_AMBER  },
    { "drvget",  "Установка драйверов (консоль)", "Скачивает и ставит драйверы из исходников (консоль)", ICON_DRVGET,   TH_PURPLE },
    { "drvman",  "Драйверы",                      "Окно драйверов: устройства PCI, установка, запуск",   ICON_CHIP,     TH_AMBER  },
    { "melody",  "Мелодия",                       "Проверка звука: гамма до мажор",                      ICON_NOTE,     TH_PINK   },
    { "burn",    "Нагрузка процессора",           "Три секунды счёта: проверка планировщика",            ICON_FLAME,    TH_RED    },
    { "udpecho", "UDP-эхо",                       "Ответ на датаграммы UDP для проверки сети",                    ICON_NET,      TH_CYAN   },
    { "mt",      "Проверка потоков",              "Потоки, мьютексы и futex на разных ядрах",            ICON_TERMINAL, TH_GREEN  },
    { "ipc",     "Проверка обмена",               "Порты и общая память между процессами",               ICON_TERMINAL, TH_GREEN  },
    { "fpu",     "Проверка FPU",                  "Состояние FPU и SSE между процессами",                ICON_TERMINAL, TH_GREEN  },
};
#define NAPPS ((int)(sizeof apps / sizeof apps[0]))

int app_info_count(void) { return NAPPS; }
const app_info_t* app_info_at(int i) { return i >= 0 && i < NAPPS ? &apps[i] : 0; }
const app_info_t* app_info(const char* name) {
    for (int i = 0; i < NAPPS; i++) if (same(apps[i].sys, name)) return &apps[i];
    return 0;
}
const char* app_label(const char* name) { const app_info_t* a = app_info(name); return a ? a->title : name; }
const char* app_desc(const char* name) { const app_info_t* a = app_info(name); return a ? a->desc : ""; }

int dock_icon(const char* n) {
    const app_info_t* a = app_info(n);
    if (a) return a->icon;
    if (starts(n, "atk")) return ICON_AI;
    return ICON_TERMINAL;                          /* неизвестная программа: общий значок */
}

const char* dock_title(const char* n) { const app_info_t* a = app_info(n); return a ? a->title : 0; }

uint32_t dock_color(const char* n) {
    const app_info_t* a = app_info(n);
    if (a) return a->color;
    static const uint32_t pal[6] = { TH_GREEN, TH_PURPLE, TH_AMBER, TH_CYAN, TH_PINK, TH_BLUE };
    uint32_t h = 2166136261u;
    for (; *n; n++) h = (h ^ (uint8_t)*n) * 16777619u;
    return pal[h % 6];
}
