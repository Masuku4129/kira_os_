// Хост-тест «чёрного ящика» (bbring.c): кольцо, переполнение, очистка, ограничитель частоты, сборка и разбор файла
// (включая повреждения), текст записей, многопоточная запись под ThreadSanitizer (--threads).
#include "bbring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SANITIZE_THREAD__
#include <pthread.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint64_t fake_ms;
static uint64_t hook_ms(void) { return fake_ms; }
static unsigned hook_cpu(void) { return 2; }

static void test_ring(void) {
    bb_set_hooks(hook_ms, hook_cpu);
    static bb_rec_t r[BB_RING];
    CHECK(bb_snapshot(r, BB_RING) == 0 && bb_total() == 0);
    fake_ms = 1500;
    bb_event(BB_FRAME, BB_SEV_WARN, 48200, 41230, 0x0001u << 16 | 0x0002, 3100u << 16 | 44900, 0, "сборка");
    bb_event(BB_CPUWAIT, BB_SEV_INFO, 87, 2, 1, 0, 0, "display");
    unsigned n = bb_snapshot(r, BB_RING);
    CHECK(n == 2 && bb_total() == 2);
    CHECK(r[0].kind == BB_FRAME && r[0].t_ms == 1500 && r[0].cpu == 2 && r[0].a == 48200 && r[0].b == 41230 && r[0].seq == 1);
    CHECK(!strcmp(r[1].name, "display") && r[1].seq == 2 && r[1].sev == BB_SEV_INFO);
    /* неверный вид не записывается, длинное имя усекается с нулём */
    bb_event(0, 0, 0, 0, 0, 0, 0, "x"); bb_event(BB_KIND_N, 0, 0, 0, 0, 0, 0, "x");
    CHECK(bb_total() == 2);
    bb_event(BB_NOTE, 0, 0, 0, 0, 0, 0, "0123456789abcdefXYZ");
    n = bb_snapshot(r, BB_RING);
    CHECK(n == 3 && strlen(r[2].name) == BB_NAME - 1 && !strncmp(r[2].name, "0123456789abcde", 15));
    /* свёрнутые счётчики и худшие значения */
    bb_stats_t st; bb_get_stats(&st);
    CHECK(st.total == 3 && st.kind_n[BB_FRAME] == 1 && st.worst[BB_FRAME] == 48200 && st.worst_t_ms[BB_FRAME] == 1500 && st.severe == 0);
    bb_event(BB_GAP, BB_SEV_SEVERE, 21000, BB_GAP_MACHINE, 0, 0, 0, "machine");
    CHECK(bb_severe_count() == 1);
    bb_note_suppressed(BB_FRAME, 99000);                    /* не попало в кольцо, но учтено */
    bb_get_stats(&st);
    CHECK(st.suppressed == 1 && st.worst[BB_FRAME] == 99000 && st.kind_n[BB_FRAME] == 2 && bb_total() == 4);
    /* ограниченный max: берутся самые новые */
    static bb_rec_t two[2];
    CHECK(bb_snapshot(two, 2) == 2 && two[1].kind == BB_GAP && two[0].kind == BB_NOTE);
    /* очистка: события до неё не видны, счётчики с нуля, нумерация продолжается */
    bb_clear();
    CHECK(bb_snapshot(r, BB_RING) == 0 && bb_total() == 0 && bb_severe_count() == 0);
    bb_get_stats(&st); CHECK(st.kind_n[BB_FRAME] == 0 && st.worst[BB_FRAME] == 0 && st.suppressed == 0);
    bb_event(BB_BOOT, 0, 7, 0, 0, 0, 0, "boot");
    n = bb_snapshot(r, BB_RING);
    CHECK(n == 1 && r[0].seq == 5 && r[0].a == 7);
    bb_clear();
}

static void test_overflow(void) {
    static bb_rec_t r[BB_RING];
    bb_clear();
    for (uint32_t i = 0; i < BB_RING + 300; i++) { fake_ms = 10000 + i; bb_event(BB_CPUWAIT, 0, i, 0, 0, 0, 0, "t"); }
    unsigned n = bb_snapshot(r, BB_RING);
    CHECK(n == BB_RING);
    CHECK(r[0].a == 300 && r[BB_RING - 1].a == BB_RING + 299);          /* старые вытеснены, порядок от старой к новой */
    for (unsigned i = 1; i < n; i++) if (r[i].seq != r[i - 1].seq + 1 || r[i].a != r[i - 1].a + 1) { CHECK(0); break; }
    bb_stats_t st; bb_get_stats(&st);
    CHECK(st.total == BB_RING + 300 && st.lost >= 300);
    bb_clear();
}

static void test_gate(void) {
    bb_gate_t g = { 0, 0, 0 };
    uint32_t sk;
    int pass = 0;
    for (int i = 0; i < 10; i++) pass += bb_gate_pass(&g, 5000 + i, 3, &sk);   /* в одной секунде - три */
    CHECK(pass == 3 && g.skipped == 7);
    CHECK(bb_gate_pass(&g, 6100, 3, &sk) == 1 && sk == 7);                    /* новое окно: сколько отброшено - сообщается */
    CHECK(bb_gate_pass(&g, 6101, 3, &sk) == 1 && sk == 0);
    CHECK(bb_gate_pass(&g, 100, 3, &sk) == 1);                                /* часы пошли назад: окно сбрасывается, а не зависает */
}

static void test_file(void) {
    static bb_rec_t r[BB_RING], back[BB_RING];
    bb_clear();
    for (uint32_t i = 0; i < 40; i++) { fake_ms = 777 + i; bb_event(BB_LOCK, i % 3, 5000 + i, 2, 1, 3, 0, "display"); }
    unsigned n = bb_snapshot(r, BB_RING);
    CHECK(n == 40);
    static uint8_t buf[sizeof(bb_file_hdr_t) + BB_RING * BB_REC_SIZE];
    unsigned len = bb_file_build(buf, r, n, 9, BB_SAVE_SEVERE, 123456, 40, 0);
    CHECK(len == sizeof(bb_file_hdr_t) + 40 * BB_REC_SIZE && len == 48 + 40 * 48);
    bb_file_hdr_t h;
    int rc = bb_file_parse(buf, len, &h, back, BB_RING);
    CHECK(rc == 40 && h.boot_id == 9 && h.reason == BB_SAVE_SEVERE && h.saved_ms == 123456 && h.total == 40 && h.count == 40);
    CHECK(memcmp(back, r, 40 * sizeof r[0]) == 0);
    /* мало места - самые новые записи */
    rc = bb_file_parse(buf, len, &h, back, 5);
    CHECK(rc == 5 && back[4].a == 5039 && back[0].a == 5035);
    /* только проверка */
    CHECK(bb_file_parse(buf, len, 0, 0, 0) == 0);
    /* пустой файл: ноль записей - допустимо */
    unsigned l0 = bb_file_build(buf, r, 0, 1, BB_SAVE_BOOT, 1, 0, 0);
    CHECK(l0 == 48 && bb_file_parse(buf, l0, &h, back, BB_RING) == 0);
    len = bb_file_build(buf, r, n, 9, BB_SAVE_SEVERE, 123456, 40, 0);
    /* повреждения */
    CHECK(bb_file_parse(buf, 20, &h, back, BB_RING) == BB_FILE_SHORT);
    CHECK(bb_file_parse(buf, 0, &h, back, BB_RING) == BB_FILE_SHORT);
    static uint8_t bad[sizeof buf];
    memcpy(bad, buf, len); bad[0] = 'X';
    CHECK(bb_file_parse(bad, len, &h, back, BB_RING) == BB_FILE_MAGIC_BAD);
    memcpy(bad, buf, len); bad[4] ^= 1;
    CHECK(bb_file_parse(bad, len, &h, back, BB_RING) == BB_FILE_VERSION_BAD);
    memcpy(bad, buf, len); bad[8] = 47;
    CHECK(bb_file_parse(bad, len, &h, back, BB_RING) == BB_FILE_SIZE_BAD);
    CHECK(bb_file_parse(buf, len - 1, &h, back, BB_RING) == BB_FILE_SIZE_BAD);          /* обрезанный файл (обрыв записи) */
    CHECK(bb_file_parse(buf, len + 48, &h, back, BB_RING) == BB_FILE_SIZE_BAD);
    memcpy(bad, buf, len); bad[12] = 0xFF; bad[13] = 0xFF;                               /* count больше кольца */
    CHECK(bb_file_parse(bad, len, &h, back, BB_RING) == BB_FILE_SIZE_BAD);
    memcpy(bad, buf, len); bad[48 + 17 * 48 + 12] ^= 0x40;                               /* бит в записи */
    CHECK(bb_file_parse(bad, len, &h, back, BB_RING) == BB_FILE_CRC_BAD);
    /* полное кольцо помещается в файл */
    for (uint32_t i = 0; i < BB_RING + 5; i++) bb_event(BB_NOTE, 0, i, 0, 0, 0, 0, "n");
    n = bb_snapshot(r, BB_RING);
    len = bb_file_build(buf, r, n, 1, BB_SAVE_PERIODIC, 1, bb_total(), 5);
    CHECK(n == BB_RING && len == 48 + BB_RING * 48 && bb_file_parse(buf, len, &h, back, BB_RING) == BB_RING && h.lost == 5);
    bb_clear();
}

static void test_text(void) {
    char l[300];
    bb_rec_t r; memset(&r, 0, sizeof r);
    r.seq = 12; r.t_ms = 62345; r.cpu = 1; r.sev = BB_SEV_WARN;
    r.kind = BB_FRAME; r.a = 48200; r.b = 41230; r.c = 100u << 16 | 20; r.d = 3100u << 16 | 44900; r.flags = 4; strcpy(r.name, "сборка");
    bb_format(&r, l, sizeof l);
    CHECK(strstr(l, "#12") && strstr(l, "62,345 с") && strstr(l, "кадр дисплея 48,2 мс") && strstr(l, "точек 41230") && strstr(l, "сборка 4,4") == 0 && strstr(l, "сборка 44,9"));
    CHECK(strstr(l, "[заметно]") && strstr(l, "ещё 4 таких не записано"));
    r.kind = BB_LOCK; r.a = 12300; r.b = 3; r.c = 2; r.d = 1; r.flags = 0; r.sev = 0; strcpy(r.name, "display");
    bb_format(&r, l, sizeof l);
    CHECK(strstr(l, "очередь готовых ядра 1") && strstr(l, "держало ядро 1") && strstr(l, "12,3 мс") && strstr(l, "display"));
    r.b = 0xFFFF; r.c = 7; bb_format(&r, l, sizeof l); CHECK(strstr(l, "мьютекс ядра, владелец - поток #7"));
    r.b = 1; bb_format(&r, l, sizeof l); CHECK(strstr(l, "таблица потоков"));
    r.kind = BB_CPUWAIT; r.a = 87; r.b = 2; r.c = 3; bb_format(&r, l, sizeof l); CHECK(strstr(l, "ждал процессор 87 мс") && strstr(l, "critical"));
    r.kind = BB_GAP; r.a = 21000; r.b = BB_GAP_MACHINE; r.sev = BB_SEV_SEVERE; strcpy(r.name, "machine");
    bb_format(&r, l, sizeof l); CHECK(strstr(l, "ТЯЖЁЛО") && strstr(l, "пауза 21000 мс") && strstr(l, "тиков таймера не было"));
    r.kind = BB_WATCHDOG; r.a = 2500; r.b = BB_WD_WARN; strcpy(r.name, "no feed"); bb_format(&r, l, sizeof l);
    CHECK(strstr(l, "предупреждение") && strstr(l, "2500 мс") && strstr(l, "причина: no feed"));
    r.kind = BB_MIND; r.a = 19; r.b = 150; r.c = 101; strcpy(r.name, "lever"); bb_format(&r, l, sizeof l);
    CHECK(strstr(l, "Разум: lever [19] -> 150 (было 100)"));
    /* усечение по размеру буфера: не выходит за границы и завершается нулём */
    char tiny[12]; unsigned k = bb_format(&r, tiny, sizeof tiny); CHECK(k == 11 && tiny[11] == 0);
    CHECK(bb_format(&r, tiny, 0) == 0);
    /* имя длиннее 15 байт усекается по границе буквы UTF-8: оборванного байта в конце нет */
    bb_clear();
    bb_event(BB_NOTE, 0, 0, 0, 0, 0, 0, "кольцо очищено");
    { bb_rec_t t1[1]; CHECK(bb_snapshot(t1, 1) == 1); CHECK(!strcmp(t1[0].name, "кольцо о"));
      bb_clear(); bb_event(BB_NOTE, 0, 0, 0, 0, 0, 0, "abcdefghijklmnяя");          /* 15-й байт - половина буквы: режется целиком */
      CHECK(bb_snapshot(t1, 1) == 1 && !strcmp(t1[0].name, "abcdefghijklmn")); }
    /* сводка и хвост */
    bb_clear();
    fake_ms = 9; bb_event(BB_FRAME, 0, 30000, 1, 0, 0, 0, "gfx"); bb_event(BB_GAP, 2, 900, BB_GAP_CORE, 0, 0, 0, "core");
    char sum[512]; bb_summary(sum, sizeof sum);
    CHECK(strstr(sum, "событий 2") && strstr(sum, "кадры 1") && strstr(sum, "паузы 1") && strstr(sum, "кадр 30,0 мс") && strstr(sum, "пауза 900 мс") && strstr(sum, "тяжёлых 1"));
    CHECK(!strcmp(bb_kind_name(BB_LOCK), "замок") && !strcmp(bb_kind_name(99), "?"));
    bb_clear();
}

static char tail_out[4096];
static void tail_fn(const char* s) { strncat(tail_out, s, sizeof tail_out - strlen(tail_out) - 1); }
static void test_tail(void) {
    bb_clear();
    for (uint32_t i = 0; i < 100; i++) bb_event(BB_NOTE, 0, i, 0, 0, 0, 0, "n");
    tail_out[0] = 0; bb_dump_tail(5, tail_fn);
    int lines = 0; for (const char* p = tail_out; *p; p++) lines += *p == '\n';
    CHECK(lines == 6 && strstr(tail_out, "последние события") && strstr(tail_out, "n 99") && !strstr(tail_out, "n 94"));
    tail_out[0] = 0; bb_dump_tail(1000, tail_fn);                          /* верхняя граница - 64 записи */
    lines = 0; for (const char* p = tail_out; *p; p++) lines += *p == '\n';
    CHECK(lines == 65);
    bb_clear();
}

#ifdef __SANITIZE_THREAD__
static void* writer(void* arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (uint32_t i = 0; i < 20000; i++) bb_event(BB_CPUWAIT, i % 5 == 0 ? BB_SEV_SEVERE : 0, i, id, 0, 0, 0, "w");
    return 0;
}
static void* reader(void* arg) {
    (void)arg;
    static bb_rec_t r[BB_RING];
    for (int k = 0; k < 200; k++) {
        unsigned n = bb_snapshot(r, BB_RING);
        for (unsigned i = 1; i < n; i++) if (r[i].seq <= r[i - 1].seq) { CHECK(0); break; }
    }
    return 0;
}
static void test_threads(void) {
    bb_clear();
    pthread_t t[5];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, writer, (void*)(uintptr_t)i);
    pthread_create(&t[4], 0, reader, 0);
    for (int i = 0; i < 5; i++) pthread_join(t[i], 0);
    bb_stats_t st; bb_get_stats(&st);
    CHECK(st.total == 80000 && st.kind_n[BB_CPUWAIT] == 80000);                         /* ни одно событие не потеряно в счётчиках */
    static bb_rec_t r[BB_RING];
    unsigned n = bb_snapshot(r, BB_RING);
    CHECK(n == BB_RING);                                                                /* после остановки писателей кольцо цело */
    for (unsigned i = 1; i < n; i++) if (r[i].seq != r[i - 1].seq + 1) { CHECK(0); break; }
}
#endif

int main(int argc, char** argv) {
    (void)argc; (void)argv;
#ifdef __SANITIZE_THREAD__
    if (argc > 1) { bb_set_hooks(hook_ms, hook_cpu); test_threads(); printf(fails ? "BBRING THREAD TESTS FAILED\n" : "bbring: concurrent writers OK\n"); return fails != 0; }
#endif
    test_ring(); test_overflow(); test_gate(); test_file(); test_text(); test_tail();
    printf(fails ? "BBRING TESTS FAILED\n" : "bbring: ring, overflow, gate, file build/parse, text OK\n");
    return fails != 0;
}
