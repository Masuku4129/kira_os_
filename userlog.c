#include "userlog.h"

static void emit_slot(userlog_slot_t* s, userlog_emit_fn emit) {
    char line[USERLOG_LINE + 32];
    unsigned n = 0;
    const char* pre = "[USER pid ";
    while (*pre) line[n++] = *pre++;
    char t[11]; int k = 0; uint32_t v = s->pid;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) line[n++] = t[--k];
    line[n++] = ']'; line[n++] = ' ';
    for (uint32_t i = 0; i < s->len; i++) line[n++] = s->buf[i];
    line[n++] = '\n';
    line[n] = 0;
    emit(line);
    s->len = 0;
}

static userlog_slot_t* slot_for(userlog_t* st, uint32_t pid, userlog_emit_fn emit) {
    userlog_slot_t* free_s = 0;
    userlog_slot_t* oldest = &st->slot[0];
    for (unsigned i = 0; i < USERLOG_SLOTS; i++) {
        userlog_slot_t* s = &st->slot[i];
        if (s->pid == pid && pid) return s;
        if (!s->pid && !free_s) free_s = s;
        if (s->age < oldest->age) oldest = s;
    }
    if (!free_s) {                                     /* все места заняты: незаконченная строка самого давнего уходит */
        if (oldest->len) emit_slot(oldest, emit);
        free_s = oldest;
    }
    free_s->pid = pid; free_s->len = 0;
    return free_s;
}

void userlog_write(userlog_t* st, uint32_t pid, const char* b, size_t n, userlog_emit_fn emit) {
    if (!n) return;
    userlog_slot_t* s = slot_for(st, pid, emit);
    s->age = ++st->clock;
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\n') { emit_slot(s, emit); continue; }
        s->buf[s->len++] = c;
        if (s->len == USERLOG_LINE) emit_slot(s, emit);
    }
}

void userlog_flush(userlog_t* st, uint32_t pid, userlog_emit_fn emit) {
    for (unsigned i = 0; i < USERLOG_SLOTS; i++) {
        userlog_slot_t* s = &st->slot[i];
        if (s->pid != pid || !pid) continue;
        if (s->len) emit_slot(s, emit);
        s->pid = 0;
    }
}
