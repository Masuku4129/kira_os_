/* Таблица прав доверенных программ: ровно то, чем программа пользуется. Новая программа без записи не получает ничего. */
#include "caps.h"

static const struct { const char* name; uint32_t caps; } table[] = {
    { "plasma",   CAP_GFX },
    { "cube3d",   CAP_GFX },
    { "paint",    CAP_GFX | CAP_FS },
    { "edit",     CAP_GFX | CAP_FS },
    { "view",     CAP_GFX | CAP_GRANT },
    { "wasm",     CAP_GFX | CAP_GRANT },
    { "fetch",    CAP_NET | CAP_FS | CAP_SYSFILES },
    { "serve",    CAP_NET_SERVE },
    { "udpecho",  CAP_UDP },
    { "melody",   CAP_SND },
    { "rec",      CAP_REC | CAP_FS },
    { "ipc",      CAP_IPC },
    { "mt",       CAP_THREAD },
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

uint32_t caps_for_program(const char* name) {
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
    case CAP_SYSFILES: return "CAP_SYSFILES"; default: return "CAP_?";
    }
}
