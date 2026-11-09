/* Общее для сборщика (mind_build.c, процесс kcc) и ядра (mind_build_grant.c, buildcmd.c). Без libc: подходит и ядру, и Ring 3, и хосту. */
#include "mind_build.h"

const char* mbj_strerror(int st) {
    switch (st) {
    case MBJ_OK:            return "ok";
    case MBJ_E_ARG:         return "неизвестный шаблон или параметр вне диапазона";
    case MBJ_E_QUOTA:       return "квота выше жёсткого предела";
    case MBJ_E_SRC_SIZE:    return "исходник больше квоты";
    case MBJ_E_SRC_FORBIDDEN: return "в исходнике запрещённая конструкция";
    case MBJ_E_TOKENS:      return "лексем больше квоты";
    case MBJ_E_COMPILE:     return "kcc отверг исходник";
    case MBJ_E_ARENA:       return "арена компилятора исчерпана";
    case MBJ_E_ELF_SIZE:    return "образ больше квоты";
    case MBJ_E_ELF_INVALID: return "образ не прошёл проверку ELF";
    case MBJ_E_TIME:        return "сборка дольше квоты времени";
    case MBJ_E_IO:          return "ошибка записи или чтения песочницы";
    case MBJ_E_NOREC:       return "нет записи о сборке или образа";
    case MBJ_E_NOGRANT:     return "разрешения нет";
    case MBJ_E_TAMPER:      return "образ изменён после сборки или разрешения";
    case MBJ_E_CONFIRM:     return "введённый хеш не совпал с SHA-256 образа";
    case MBJ_E_SIG:         return "подпись отвергнута";
    case MBJ_E_RATE:        return "слишком рано или слишком много ожидающих решения";
    default:                return "неизвестная ошибка";
    }
}

int mbj_job_valid(const mbj_job_t* job) {
    if (!job) return MBJ_E_ARG;
    if (job->tmpl == MBJ_T_PROBE) {
        if (job->p[0] < 1 || job->p[0] > 4194304u) return MBJ_E_ARG;
        return MBJ_OK;
    }
    if (job->tmpl == MBJ_T_TABLE) {
        if (job->p[0] < 1 || job->p[0] > 256 || job->p[2] < 1 || job->p[2] > 65536u) return MBJ_E_ARG;
        return MBJ_OK;
    }
    return MBJ_E_ARG;
}

int mbj_name_valid(const char* n) {
    unsigned k = 0;
    if (!n) return 0;
    while (n[k] && k < 40) k++;
    if (k < 4 || k > 32 || n[0] != 'm' || n[1] != 'b') return 0;
    for (unsigned i = 0; i < k; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

void mbj_hex(const uint8_t* d, int n, char* out) {
    for (int i = 0; i < n; i++) { out[2 * i] = "0123456789abcdef"[d[i] >> 4]; out[2 * i + 1] = "0123456789abcdef"[d[i] & 15]; }
    out[2 * n] = 0;
}

int mbj_hex_prefix(const char* typed, const uint8_t sha[32]) {
    char h[65];
    unsigned k = 0;
    if (!typed) return 0;
    while (typed[k] && k < 80) k++;
    if (k < 8 || k > 64) return 0;
    mbj_hex(sha, 32, h);
    for (unsigned i = 0; i < k; i++) {
        char c = typed[i];
        if (c >= 'A' && c <= 'F') c = (char)(c + 32);
        if (c != h[i]) return 0;
    }
    return 1;
}
