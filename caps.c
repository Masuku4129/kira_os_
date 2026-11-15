/* Таблица прав доверенных программ: ровно то, чем программа пользуется. Новая программа без записи не получает ничего. */
#include "caps.h"

static const struct { const char* name; uint32_t caps; } table[] = {
    { "plasma",   CAP_GFX },
    { "cube3d",   CAP_GFX },
    { "paint",    CAP_GFX | CAP_FS },
    { "edit",     CAP_GFX | CAP_FS },
    { "view",     CAP_GFX | CAP_GRANT },
    { "wasm",     CAP_GFX | CAP_GRANT | CAP_JIT },
    { "fetch",    CAP_NET | CAP_FS | CAP_SYSFILES },
    { "serve",    CAP_NET_SERVE },
    { "udpecho",  CAP_UDP },
    { "net6",     CAP_NET | CAP_UDP },
    { "melody",   CAP_SND },
    { "rec",      CAP_REC | CAP_FS },
    { "ipc",      CAP_IPC },
    { "ipcx",     CAP_IPC },                /* проверка границы доменов: живёт в домене media, открывает общий объект домена tools (ядро обязано отказать) */
    { "mt",       CAP_THREAD },
    { "kcc",      CAP_FS },                 /* компилятор: исходники и результат в /data/kcc, заголовки - FS_OP_INC_* (только kcc); больше ничего */
    { "drvget",   CAP_NET | CAP_FS | CAP_SYSFILES },   /* загрузка исходников драйвера по HTTPS, сборка встроенным kcc, запись пакета в /drv (FS_OP_DRV_*, только drvget) */
    { "drvman",   CAP_GFX | CAP_NET | CAP_FS | CAP_SYSFILES | CAP_THREAD | CAP_PCILIST },   /* окно «Драйверы»: те же возможности, что у drvget (скачать, собрать kcc, записать в /drv), плюс окно, поток для долгих шагов и чтение списка PCI */
    { "burn",     0 },
    { "fpu",      0 },
    { "demo",     0 },
    { "atk-mem",  0 },
    { "atk-priv", 0 },
    { "atk-sys",  0 },
    { "atk-gs",   0 },
    { "atk-jmp",  0 },
    { "atk-cap",  0 },
};

const char* caps_program_at(unsigned i) { return i < sizeof table / sizeof table[0] ? table[i].name : 0; }

uint32_t caps_for_program(const char* name) {
    /* Программа, собранная пользователем на Кире и запущенная командой runlocal (progs.c): доверие дано явной командой, а не манифестом
       сборки, поэтому прав нет вообще - ни файлов, ни сети, ни окон. Имена с этой приставкой в таблице не заводятся. */
    const char* lp = "local-";
    unsigned j = 0;
    while (lp[j] && name[j] == lp[j]) j++;
    if (!lp[j]) return 0;
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++) {
        unsigned k = 0;
        while (table[i].name[k] && name[k] == table[i].name[k]) k++;
        if (!table[i].name[k] && !name[k]) return table[i].caps;
    }
    return 0;
}

const char* caps_bit_name(uint32_t bit) {
    switch (bit) {
    case CAP_GFX: return "CAP_GFX";           case CAP_NET: return "CAP_NET";
    case CAP_NET_SERVE: return "CAP_NET_SERVE"; case CAP_UDP: return "CAP_UDP";
    case CAP_SND: return "CAP_SND";           case CAP_REC: return "CAP_REC";
    case CAP_IPC: return "CAP_IPC";           case CAP_THREAD: return "CAP_THREAD";
    case CAP_FS: return "CAP_FS";             case CAP_GRANT: return "CAP_GRANT";
    case CAP_SYSFILES: return "CAP_SYSFILES";
    case CAP_MMIO: return "CAP_MMIO";         case CAP_IRQ: return "CAP_IRQ";
    case CAP_DMA: return "CAP_DMA";           case CAP_PCIDEV: return "CAP_PCIDEV";
    case CAP_NETDEV: return "CAP_NETDEV";     case CAP_PCILIST: return "CAP_PCILIST";
    case CAP_JIT: return "CAP_JIT";           default: return "CAP_?";
    }
}

/* ---- домены ---- */
static const struct { const char* name; uint32_t caps_max; unsigned procs; uint32_t reach; } doms[DOM_COUNT] = {
    /* system: окна управления драйверами; видит общие объекты всех доменов (управление) */
    [DOM_SYSTEM]    = { "system",    CAP_GFX | CAP_NET | CAP_FS | CAP_SYSFILES | CAP_THREAD | CAP_PCILIST, 4,
                        (1u << DOM_COUNT) - 1 },
    [DOM_DRIVER]    = { "driver",    CAP_DRIVER_, 16, 1u << DOM_DRIVER },
    [DOM_MEDIA]     = { "media",     CAP_GFX | CAP_FS | CAP_GRANT | CAP_SND | CAP_REC | CAP_IPC, 16, 1u << DOM_MEDIA },
    [DOM_NET]       = { "net",       CAP_NET | CAP_NET_SERVE | CAP_UDP | CAP_FS | CAP_SYSFILES, 8, 1u << DOM_NET },
    [DOM_TOOLS]     = { "tools",     CAP_GFX | CAP_FS | CAP_GRANT | CAP_JIT | CAP_IPC | CAP_THREAD, 16, 1u << DOM_TOOLS },
    [DOM_UNTRUSTED] = { "untrusted", 0, 16, 1u << DOM_UNTRUSTED },
};

static int name_is(const char* a, const char* b) { unsigned k = 0; while (a[k] && a[k] == b[k]) k++; return !a[k] && !b[k]; }

uint32_t domain_for_program(const char* name) {
    static const struct { const char* name; uint32_t dom; } map[] = {
        { "drvget", DOM_SYSTEM }, { "drvman", DOM_SYSTEM },
        { "plasma", DOM_MEDIA }, { "cube3d", DOM_MEDIA }, { "paint", DOM_MEDIA }, { "view", DOM_MEDIA },
        { "melody", DOM_MEDIA }, { "rec", DOM_MEDIA }, { "ipcx", DOM_MEDIA },
        { "fetch", DOM_NET }, { "serve", DOM_NET }, { "udpecho", DOM_NET }, { "net6", DOM_NET },
        { "edit", DOM_TOOLS }, { "kcc", DOM_TOOLS }, { "wasm", DOM_TOOLS }, { "ipc", DOM_TOOLS }, { "mt", DOM_TOOLS },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (name_is(map[i].name, name)) return map[i].dom;
    return DOM_UNTRUSTED;
}
uint32_t domain_caps_max(uint32_t d) { return d < DOM_COUNT ? doms[d].caps_max : 0; }
unsigned domain_proc_max(uint32_t d) { return d < DOM_COUNT ? doms[d].procs : 0; }
int domain_may_reach(uint32_t from, uint32_t to) { return from < DOM_COUNT && to < DOM_COUNT && (doms[from].reach >> to & 1u); }
const char* domain_name(uint32_t d) { return d < DOM_COUNT ? doms[d].name : "?"; }
