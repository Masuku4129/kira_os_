#include "bbring.h"

/* Кольцо «чёрного ящика». Каждая запись занимает слот seq % BB_RING; слот пишется так: seq = 0 (идёт запись), поля, seq = номер
   (запись готова). Читатель берёт слот только если номер совпал до и после копирования. Запись: один fetch_add и десяток
   обычных записей в память - можно звать из планировщика и прерывания таймера. */

/* Тело записи лежит шестью 64-битными словами (читатель и писатель обращаются к ним атомарно, поэтому и гонок в понимании компилятора нет),
   готовность слота - в отдельном массиве номеров. */
typedef struct { uint64_t w[BB_REC_SIZE / 8]; } slot_t;
static slot_t ring[BB_RING];
static uint32_t ring_seq[BB_RING];
_Static_assert(sizeof(slot_t) == sizeof(bb_rec_t), "слот - ровно запись");
static volatile uint64_t head;            /* сколько событий занято слотов с запуска; следующий номер - head + 1 */
static volatile uint64_t base_total;      /* значение head при последнем bb_clear: события до него не показываются */
static volatile uint64_t lost_n, suppressed_n;
static volatile uint32_t severe_n;
static volatile uint32_t kind_n[BB_KIND_N], worst_a[BB_KIND_N];
static volatile uint64_t worst_t[BB_KIND_N];
static void mcopy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static uint64_t (*hook_ms)(void);
static unsigned (*hook_cpu)(void);

void bb_set_hooks(uint64_t (*now_ms)(void), unsigned (*cpu_id)(void)) { hook_ms = now_ms; hook_cpu = cpu_id; }

static void note_worst(uint8_t kind, uint32_t a, uint64_t t) {
    uint32_t cur = __atomic_load_n(&worst_a[kind], __ATOMIC_RELAXED);
    while (a > cur) {
        if (__atomic_compare_exchange_n(&worst_a[kind], &cur, a, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            __atomic_store_n(&worst_t[kind], t, __ATOMIC_RELAXED);
            break;
        }
    }
}

void bb_event(uint8_t kind, uint8_t sev, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t flags, const char* name) {
    if (kind == 0 || kind >= BB_KIND_N) return;
    uint64_t now = hook_ms ? hook_ms() : 0;
    uint64_t idx = __atomic_fetch_add(&head, 1, __ATOMIC_ACQ_REL);
    unsigned si = (unsigned)(idx % BB_RING);
    if (idx >= BB_RING) __atomic_fetch_add(&lost_n, 1, __ATOMIC_RELAXED);
    bb_rec_t r;
    r.t_ms = now;
    r.kind = kind; r.cpu = hook_cpu ? (uint8_t)hook_cpu() : 0; r.flags = flags; r.sev = sev;
    r.a = a; r.b = b; r.c = c; r.d = d;
    unsigned i = 0;
    if (name) {
        unsigned n = 0;
        while (n < BB_NAME - 1 && name[n]) n++;
        if (name[n]) while (n > 0 && ((uint8_t)name[n] & 0xC0) == 0x80) n--;      /* усечение не режет букву UTF-8 пополам */
        for (; i < n; i++) r.name[i] = name[i];
    }
    for (; i < BB_NAME; i++) r.name[i] = 0;
    r.seq = (uint32_t)(idx + 1);
    slot_t sl; mcopy(&sl, &r, sizeof sl);
    __atomic_store_n(&ring_seq[si], 0, __ATOMIC_RELEASE);                    /* идёт запись */
    for (unsigned k = 0; k < BB_REC_SIZE / 8; k++) __atomic_store_n(&ring[si].w[k], sl.w[k], __ATOMIC_RELAXED);
    __atomic_fetch_add(&kind_n[kind], 1, __ATOMIC_RELAXED);
    note_worst(kind, a, now);
    if (sev >= BB_SEV_SEVERE) __atomic_fetch_add(&severe_n, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&ring_seq[si], (uint32_t)(idx + 1), __ATOMIC_RELEASE);  /* готова */
}

void bb_note_suppressed(uint8_t kind, uint32_t a) {
    if (kind == 0 || kind >= BB_KIND_N) return;
    __atomic_fetch_add(&suppressed_n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&kind_n[kind], 1, __ATOMIC_RELAXED);
    note_worst(kind, a, hook_ms ? hook_ms() : 0);
}

uint64_t bb_total(void) { return __atomic_load_n(&head, __ATOMIC_ACQUIRE) - __atomic_load_n(&base_total, __ATOMIC_ACQUIRE); }
uint32_t bb_severe_count(void) { return __atomic_load_n(&severe_n, __ATOMIC_ACQUIRE); }

void bb_get_stats(bb_stats_t* o) {
    o->total = bb_total();
    o->lost = __atomic_load_n(&lost_n, __ATOMIC_RELAXED);
    o->suppressed = __atomic_load_n(&suppressed_n, __ATOMIC_RELAXED);
    for (int k = 0; k < BB_KIND_N; k++) {
        o->kind_n[k] = __atomic_load_n(&kind_n[k], __ATOMIC_RELAXED);
        o->worst[k] = __atomic_load_n(&worst_a[k], __ATOMIC_RELAXED);
        o->worst_t_ms[k] = __atomic_load_n(&worst_t[k], __ATOMIC_RELAXED);
    }
    o->severe = __atomic_load_n(&severe_n, __ATOMIC_RELAXED);
}

unsigned bb_snapshot(bb_rec_t* out, unsigned max) {
    uint64_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    uint64_t from = __atomic_load_n(&base_total, __ATOMIC_ACQUIRE);
    if (h - from > BB_RING) from = h - BB_RING;
    unsigned n = 0;
    if (h - from > max) from = h - max;
    for (uint64_t i = from; i < h && n < max; i++) {
        unsigned si = (unsigned)(i % BB_RING);
        uint32_t s1 = __atomic_load_n(&ring_seq[si], __ATOMIC_ACQUIRE);
        if (s1 != (uint32_t)(i + 1)) continue;                       /* перезаписано или пишется сейчас */
        slot_t sl;
        for (unsigned k = 0; k < BB_REC_SIZE / 8; k++) sl.w[k] = __atomic_load_n(&ring[si].w[k], __ATOMIC_ACQUIRE);   /* acquire: перепроверка номера ниже не уйдёт выше чтения слов */
        if (__atomic_load_n(&ring_seq[si], __ATOMIC_RELAXED) != s1) continue;
        mcopy(&out[n++], &sl, sizeof sl);
    }
    return n;
}

void bb_clear(void) {
    /* Записи не стираются: кольцо просто «начинается» с этого места; счётчики сбрасываются. */
    __atomic_store_n(&base_total, __atomic_load_n(&head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    __atomic_store_n(&lost_n, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&suppressed_n, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&severe_n, 0, __ATOMIC_RELAXED);
    for (int k = 0; k < BB_KIND_N; k++) {
        __atomic_store_n(&kind_n[k], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&worst_a[k], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&worst_t[k], 0, __ATOMIC_RELAXED);
    }
}

int bb_gate_pass(bb_gate_t* g, uint64_t now_ms, uint32_t per_sec, uint32_t* skipped_before) {
    if (skipped_before) *skipped_before = 0;
    if (now_ms - g->win_ms >= 1000 || now_ms < g->win_ms) { g->win_ms = now_ms; g->n = 0; }
    if (g->n < per_sec) {
        g->n++;
        if (skipped_before) { *skipped_before = g->skipped; }
        g->skipped = 0;
        return 1;
    }
    g->skipped++;
    return 0;
}

/* ---------------- файл ---------------- */

uint32_t bb_crc32(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}


unsigned bb_file_seal(uint8_t* buf, unsigned n, uint32_t boot_id, uint32_t reason, uint64_t saved_ms, uint64_t total, uint32_t lost) {
    bb_file_hdr_t h;
    h.magic[0] = 'K'; h.magic[1] = 'B'; h.magic[2] = 'B'; h.magic[3] = '1';
    h.version = BB_FILE_VERSION; h.rec_size = BB_REC_SIZE; h.count = n; h.boot_id = boot_id; h.reason = reason;
    h.saved_ms = saved_ms; h.total = total; h.lost = lost;
    h.crc = bb_crc32(buf + sizeof h, (size_t)n * BB_REC_SIZE);
    mcopy(buf, &h, sizeof h);
    return (unsigned)(sizeof h + (size_t)n * BB_REC_SIZE);
}

unsigned bb_file_build(uint8_t* out, const bb_rec_t* recs, unsigned n, uint32_t boot_id, uint32_t reason,
                       uint64_t saved_ms, uint64_t total, uint32_t lost) {
    mcopy(out + sizeof(bb_file_hdr_t), recs, (size_t)n * BB_REC_SIZE);
    return bb_file_seal(out, n, boot_id, reason, saved_ms, total, lost);
}

int bb_file_parse(const uint8_t* buf, unsigned len, bb_file_hdr_t* hdr, bb_rec_t* recs, unsigned max) {
    bb_file_hdr_t h;
    if (len < sizeof h) return BB_FILE_SHORT;
    mcopy(&h, buf, sizeof h);
    if (h.magic[0] != 'K' || h.magic[1] != 'B' || h.magic[2] != 'B' || h.magic[3] != '1') return BB_FILE_MAGIC_BAD;
    if (h.version != BB_FILE_VERSION) return BB_FILE_VERSION_BAD;
    if (h.rec_size != BB_REC_SIZE || h.count > BB_RING || (uint64_t)len != sizeof h + (uint64_t)h.count * BB_REC_SIZE) return BB_FILE_SIZE_BAD;
    if (bb_crc32(buf + sizeof h, (size_t)h.count * BB_REC_SIZE) != h.crc) return BB_FILE_CRC_BAD;
    if (hdr) *hdr = h;
    unsigned n = h.count < max ? h.count : max;
    if (recs) mcopy(recs, buf + sizeof h + (size_t)(h.count - n) * BB_REC_SIZE, (size_t)n * BB_REC_SIZE);   /* если места мало - самые новые */
    return (int)n;
}

/* ---------------- текст ---------------- */

typedef struct { char* b; unsigned n, cap; } sb_t;
static void sb_s(sb_t* s, const char* t) { while (*t && s->n + 1 < s->cap) s->b[s->n++] = *t++; s->b[s->n] = 0; }
static void sb_u(sb_t* s, uint64_t v) {
    char t[21]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && s->n + 1 < s->cap) s->b[s->n++] = t[--k];
    s->b[s->n] = 0;
}
static void sb_x10(sb_t* s, uint64_t x10) { sb_u(s, x10 / 10); sb_s(s, ","); sb_u(s, x10 % 10); }        /* десятые: 482 -> 48,2 */
static void sb_ms_us(sb_t* s, uint64_t us) { sb_x10(s, us / 100); }                                       /* микросекунды -> мс с десятой */

const char* bb_kind_name(unsigned k) {
    static const char* const n[BB_KIND_N] = { "?", "запуск", "кадр", "замок", "ожидание процессора", "пауза", "сторож", "Разум", "пометка" };
    return k < BB_KIND_N ? n[k] : "?";
}

unsigned bb_format(const bb_rec_t* r, char* out, unsigned cap) {
    sb_t s = { out, 0, cap };
    if (cap) out[0] = 0;
    sb_s(&s, "#"); sb_u(&s, r->seq); sb_s(&s, "  "); sb_u(&s, r->t_ms / 1000); sb_s(&s, ",");
    { uint64_t f = r->t_ms % 1000; if (f < 100) sb_s(&s, "0"); if (f < 10) sb_s(&s, "0"); sb_u(&s, f); }
    sb_s(&s, " с  ядро "); sb_u(&s, r->cpu); sb_s(&s, "  ");
    if (r->sev >= BB_SEV_SEVERE) sb_s(&s, "[ТЯЖЁЛО] "); else if (r->sev == BB_SEV_WARN) sb_s(&s, "[заметно] ");
    switch (r->kind) {
    case BB_BOOT:
        sb_s(&s, "запуск системы, номер "); sb_u(&s, r->a);
        break;
    case BB_FRAME: {
        sb_s(&s, "кадр дисплея "); sb_ms_us(&s, r->a); sb_s(&s, " мс при бюджете 16,7, точек "); sb_u(&s, r->b);
        sb_s(&s, "; этапы, мс: gfx "); sb_ms_us(&s, r->c >> 16); sb_s(&s, ", ввод "); sb_ms_us(&s, r->c & 0xFFFF);
        sb_s(&s, ", середина "); sb_ms_us(&s, r->d >> 16); sb_s(&s, ", сборка "); sb_ms_us(&s, r->d & 0xFFFF);
        sb_s(&s, "; дольше всех: "); sb_s(&s, r->name);
        if (r->flags) { sb_s(&s, " (ещё "); sb_u(&s, r->flags); sb_s(&s, r->flags == 255 ? "+" : ""); sb_s(&s, " таких не записано)"); }
        break;
    }
    case BB_LOCK:
        sb_s(&s, "ожидание замка "); sb_ms_us(&s, r->a); sb_s(&s, " мс: ");
        if (r->b == 1) sb_s(&s, "таблица потоков");
        else if (r->b == 0xFFFF) { sb_s(&s, "мьютекс ядра, владелец - поток #"); sb_u(&s, r->c); }
        else { sb_s(&s, "очередь готовых ядра "); sb_u(&s, r->b >= 2 ? r->b - 2 : 0); }
        if (r->b != 0xFFFF && r->c) { sb_s(&s, ", держало ядро "); sb_u(&s, r->c - 1); }
        sb_s(&s, ", ждало ядро "); sb_u(&s, r->d); sb_s(&s, ", поток "); sb_s(&s, r->name[0] ? r->name : "?");
        break;
    case BB_CPUWAIT:
        sb_s(&s, "поток ядра «"); sb_s(&s, r->name); sb_s(&s, "» ждал процессор "); sb_u(&s, r->a); sb_s(&s, " мс (приоритет ");
        { static const char* const pr[] = { "idle", "normal", "critical", "realtime" }; sb_s(&s, r->b < 4 ? pr[r->b] : "?"); }
        sb_s(&s, ", получил ядро "); sb_u(&s, r->c); sb_s(&s, ")");
        break;
    case BB_GAP:
        sb_s(&s, "пауза "); sb_u(&s, r->a); sb_s(&s, " мс: ");
        if (r->b == BB_GAP_CORE) sb_s(&s, "основной цикл ядра не возвращался");
        else if (r->b == BB_GAP_DISPLAY) sb_s(&s, "цикл дисплея не начинался");
        else if (r->b == BB_GAP_MACHINE) sb_s(&s, "тиков таймера не было (все ядра стояли или хозяин приостановил машину)");
        else sb_s(&s, "цикл не определён");
        if (r->name[0]) { sb_s(&s, " ["); sb_s(&s, r->name); sb_s(&s, "]"); }
        break;
    case BB_WATCHDOG:
        sb_s(&s, "сторож: ");
        if (r->b == BB_WD_WARN) sb_s(&s, "предупреждение, основной поток не кормил сторожа ");
        else if (r->b == BB_WD_STALL) sb_s(&s, "машина стояла целиком, не засчитано: ");
        else sb_s(&s, "ПАНИКА, не кормили ");
        sb_u(&s, r->a); sb_s(&s, " мс");
        if (r->name[0]) { sb_s(&s, ", причина: "); sb_s(&s, r->name); }
        break;
    case BB_MIND:
        sb_s(&s, "Разум: "); sb_s(&s, r->name); sb_s(&s, " ["); sb_u(&s, r->a); sb_s(&s, "] -> "); sb_u(&s, r->b);
        if (r->c) { sb_s(&s, " (было "); sb_u(&s, r->c - 1); sb_s(&s, ")"); }
        break;
    case BB_NOTE:
        sb_s(&s, "пометка: "); sb_s(&s, r->name); sb_s(&s, " "); sb_u(&s, r->a);
        break;
    default:
        sb_s(&s, "неизвестное событие "); sb_u(&s, r->kind);
        break;
    }
    return s.n;
}

unsigned bb_summary(char* out, unsigned cap) {
    bb_stats_t st; bb_get_stats(&st);
    sb_t s = { out, 0, cap };
    if (cap) out[0] = 0;
    uint64_t in_ring = st.total < BB_RING ? st.total : BB_RING;
    sb_s(&s, "[BB] событий "); sb_u(&s, st.total); sb_s(&s, " (в кольце "); sb_u(&s, in_ring); sb_s(&s, ", вытеснено "); sb_u(&s, st.lost);
    sb_s(&s, ", не записано из-за частоты "); sb_u(&s, st.suppressed);
    sb_s(&s, "): кадры "); sb_u(&s, st.kind_n[BB_FRAME]); sb_s(&s, ", замки "); sb_u(&s, st.kind_n[BB_LOCK]);
    sb_s(&s, ", ожидание процессора "); sb_u(&s, st.kind_n[BB_CPUWAIT]); sb_s(&s, ", паузы "); sb_u(&s, st.kind_n[BB_GAP]);
    sb_s(&s, ", сторож "); sb_u(&s, st.kind_n[BB_WATCHDOG]); sb_s(&s, ", Разум "); sb_u(&s, st.kind_n[BB_MIND]);
    sb_s(&s, "; худшее: кадр "); sb_ms_us(&s, st.worst[BB_FRAME]); sb_s(&s, " мс, замок "); sb_ms_us(&s, st.worst[BB_LOCK]);
    sb_s(&s, " мс, ожидание процессора "); sb_u(&s, st.worst[BB_CPUWAIT]); sb_s(&s, " мс, пауза "); sb_u(&s, st.worst[BB_GAP]);
    sb_s(&s, " мс, тяжёлых "); sb_u(&s, st.severe);
    return s.n;
}

void bb_dump_tail(unsigned n, void (*out)(const char*)) {
    static bb_rec_t tmp[64];       /* аварийный отчёт идёт на остановленной машине: одно ядро, стек беречь */
    if (n > 64) n = 64;
    unsigned got = bb_snapshot(tmp, n);
    char line[256];
    out("[KIRA BLACKBOX] последние события:\n");
    for (unsigned i = 0; i < got; i++) {
        bb_format(&tmp[i], line, sizeof line - 1);
        unsigned l = 0; while (line[l]) l++;
        line[l] = '\n'; line[l + 1] = 0;
        out("  "); out(line);
    }
}
