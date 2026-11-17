/* KiraFS: потеря питания при диске с кэшем записи (под ASan/UBSan).
   В отличие от tests/kfs_test.c (запись просто обрывается), здесь диск ведёт себя как настоящий накопитель:
   записи лежат в энергозависимом кэше до flush; при потере питания на носитель попадает ЛЮБОЙ набор секторов,
   записанных после последнего flush, в любом порядке (для одного сектора - любая из его версий).
   Питание может пропасть и на записи сектора, и на сбросе кэша (flush).
   Случайные последовательности операций (запись/замена файла, mkdir, unlink, clone) с обрывом на случайной записи.
   Инварианты:
     1. kfs_mount после обрыва успешен; повторное монтирование ничего не чинит и не меняет счётчиков;
     2. затронутый путь - либо в старом, либо в новом состоянии (операции атомарны), все остальные - как были;
     3. операция, вернувшая успех, оставила кэш пустым (данные уже на носителе - иначе откат после «успеха»);
     4. после обрыва нет утечек: если стереть все файлы и каталоги, свободных блоков и инодов столько же, сколько на чистом томе;
     5. система живёт дальше: следующие операции работают.
   Использование: kfs_crash_test ЗЁРЕН ПОСЛЕДОВАТЕЛЬНОСТЕЙ [ОПЕРАЦИЙ] */
#include "kfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECT (16 + 16384)
#define PEND_MAX 40000
static uint8_t disk[SECT * 512];
static struct { uint64_t lba; uint8_t d[512]; } pend[PEND_MAX];
static int npend;
static long crash_after = -1;          /* сколько секторов ещё дойдёт до кэша; -1 - питание не пропадёт */
static int tripped;                    /* питание пропало */

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static int fails;
#define BAD(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; if (fails > 8) exit(1); } while (0)

static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b) {
    (void)d; if (lba + n > SECT || tripped) return -1;
    memcpy(b, disk + lba * 512, (size_t)n * 512);
    for (int i = 0; i < npend; i++)                 /* кэш накопителя отдаёт последнюю записанную версию */
        if (pend[i].lba >= lba && pend[i].lba < lba + n) memcpy((uint8_t*)b + (pend[i].lba - lba) * 512, pend[i].d, 512);
    return 0;
}
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b) {
    (void)d; if (lba + n > SECT) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (tripped) return -1;
        if (crash_after == 0) { tripped = 1; return -1; }
        if (crash_after > 0) crash_after--;
        if (npend >= PEND_MAX) { printf("FAIL: кэш записи переполнен\n"); exit(2); }
        pend[npend].lba = lba + i; memcpy(pend[npend].d, (const uint8_t*)b + (size_t)i * 512, 512); npend++;
    }
    return 0;
}
static int fl(blk_dev_t* d) {
    (void)d; if (tripped) return -1;
    if (crash_after == 0) { tripped = 1; return -1; }       /* питание пропало во время сброса кэша: на носителе случайная часть */
    if (crash_after > 0) crash_after--;
    for (int i = 0; i < npend; i++) memcpy(disk + pend[i].lba * 512, pend[i].d, 512);
    npend = 0; return 0;
}
static blk_dev_t dev = {"ram", SECT, rd, wr, fl, 0};

/* Потеря питания: из кэша на носитель попадает случайное подмножество в случайном порядке. */
static void power_loss(void) {
    int idx[PEND_MAX]; int k = 0;
    int mode = (int)(rnd() % 4);                    /* 0 - ничего, 1 - всё, 2/3 - случайно */
    for (int i = 0; i < npend; i++) if (mode == 1 || (mode >= 2 && (rnd() & 1))) idx[k++] = i;
    for (int i = k - 1; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (int i = 0; i < k; i++) memcpy(disk + pend[idx[i]].lba * 512, pend[idx[i]].d, 512);
    npend = 0; tripped = 0; crash_after = -1;
}

/* ---- модель ---- */
enum { NONE = 0, FILE_ = 1, DIR_ = 2 };
typedef struct { int kind; int seed; size_t size; } ent_t;
#define NSLOT 14
static const char* const path[NSLOT] = {"/f0", "/f1", "/f2", "/f3", "/f4", "/f5", "/d0", "/d1",
                                        "/d0/g0", "/d0/g1", "/d0/g2", "/d1/g0", "/d1/g1", "/d1/g2"};
static int parent(int s) { return s < 8 ? -1 : (s < 11 ? 6 : 7); }
typedef struct { ent_t e[NSLOT]; } model_t;
static uint8_t buf[400000];
static uint8_t pat(int seed, size_t i) { return (uint8_t)(seed * 31 + i * 7 + (i >> 8)); }
static const size_t sizes[] = {0, 1, 100, 4095, 4096, 5000, 20000, 60000, 100000, 300000};

static int matches(int s, const ent_t* e) {
    kfs_stat_t st; int r = kfs_stat(path[s], &st);
    if (e->kind == NONE) return r == KFS_ERR_NOENT;
    if (r) return 0;
    if (e->kind == DIR_) return st.type == KFS_DIR;
    if (st.type != KFS_FILE || st.size != e->size) return 0;
    if (kfs_read(path[s], 0, buf, e->size + 8) != (int64_t)e->size) return 0;
    for (size_t i = 0; i < e->size; i++) if (buf[i] != pat(e->seed, i)) return 0;
    return 1;
}

typedef struct { int op, slot, src; size_t size; int seed; } op_t;      /* 0 put, 1 mkdir, 2 unlink, 3 clone */

static int gen(const model_t* m, op_t* o) {
    for (int tries = 0; tries < 50; tries++) {
        int s = (int)(rnd() % NSLOT), p = parent(s);
        if (p >= 0 && m->e[p].kind != DIR_) continue;
        o->slot = s; o->src = -1; o->seed = (int)(rnd() % 200) + 1; o->size = sizes[rnd() % 10];
        int r = (int)(rnd() % 10);
        if (s == 6 || s == 7) {                                         /* каталоги */
            if (m->e[s].kind == NONE) { o->op = 1; return 1; }
            int empty = 1; for (int c = 8; c < NSLOT; c++) if (parent(c) == s && m->e[c].kind != NONE) empty = 0;
            if (empty && r < 5) { o->op = 2; return 1; }
            continue;
        }
        if (m->e[s].kind == DIR_) continue;
        if (r < 5) { o->op = 0; return 1; }
        if (r < 7) { if (m->e[s].kind == FILE_) { o->op = 2; return 1; } continue; }
        int src = (int)(rnd() % NSLOT);
        if (m->e[s].kind == NONE && src != s && m->e[src].kind == FILE_) { o->op = 3; o->src = src; return 1; }
    }
    return 0;
}
static int do_op(const op_t* o) {
    switch (o->op) {
    case 0: for (size_t i = 0; i < o->size; i++) buf[i] = pat(o->seed, i); return kfs_write(path[o->slot], buf, o->size);
    case 1: return kfs_mkdir(path[o->slot]);
    case 2: return kfs_unlink(path[o->slot]);
    default: return kfs_clone(path[o->src], path[o->slot]);
    }
}
static void apply(model_t* m, const op_t* o) {
    ent_t* e = &m->e[o->slot];
    switch (o->op) {
    case 0: e->kind = FILE_; e->seed = o->seed; e->size = o->size; break;
    case 1: e->kind = DIR_; break;
    case 2: e->kind = NONE; break;
    default: *e = m->e[o->src]; break;
    }
}

static uint32_t base_free, base_ino;
/* Стереть всё и сверить, что утечек нет. */
static void wipe_check(const model_t* m, const char* where) {
    for (int s = 8; s < NSLOT; s++) if (m->e[s].kind == FILE_) { if (kfs_unlink(path[s])) BAD("%s: не удалось стереть %s", where, path[s]); }
    for (int s = 0; s < 6; s++) if (m->e[s].kind == FILE_) { if (kfs_unlink(path[s])) BAD("%s: не удалось стереть %s", where, path[s]); }
    for (int s = 6; s < 8; s++) if (m->e[s].kind == DIR_) { if (kfs_unlink(path[s])) BAD("%s: не удалось стереть %s", where, path[s]); }
    kfs_info_t i; kfs_info(&i);
    if (i.free_blocks != base_free || i.free_inodes != base_ino)
        BAD("%s: утечка после стирания: блоков %u (чистый том %u), инодов %u (%u)", where, i.free_blocks, base_free, i.free_inodes, base_ino);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "kfs_crash_test ЗЁРЕН ПОСЛЕДОВАТЕЛЬНОСТЕЙ [ОПЕРАЦИЙ]\n"); return 2; }
    uint64_t seed0 = strtoull(argv[1], NULL, 0); long nseq = atol(argv[2]); int nops = argc > 3 ? atoi(argv[3]) : 40;
    /* опорный чистый том: после создания и стирания одного файла (корневой каталог получает блок навсегда) */
    memset(disk, 0, sizeof disk); npend = 0;
    if (kfs_format(&dev, 16)) { printf("FAIL: format\n"); return 1; }
    if (kfs_write("/w", "x", 1) || kfs_unlink("/w")) { printf("FAIL: опорный том\n"); return 1; }
    { kfs_info_t i; kfs_info(&i); base_free = i.free_blocks; base_ino = i.free_inodes; }
    long crashes = 0, tripped_n = 0, ops_done = 0, ok_ops = 0;
    for (long q = 0; q < nseq; q++) {
        rs = 0x9e3779b97f4a7c15ull ^ (seed0 * 1000003ull + (uint64_t)q + 1); if (!rs) rs = 1;
        memset(disk, 0, sizeof disk); npend = 0; tripped = 0; crash_after = -1;
        if (kfs_format(&dev, 16)) { printf("FAIL: format\n"); return 1; }
        if (kfs_write("/w", "x", 1) || kfs_unlink("/w")) { printf("FAIL: опорный том\n"); return 1; }
        model_t m; memset(&m, 0, sizeof m);
        for (int n = 0; n < nops; n++) {
            op_t o; if (!gen(&m, &o)) continue;
            model_t nm = m; apply(&nm, &o);
            int arm = (rnd() % 3) != 0;
            crash_after = arm ? (long)(rnd() % 4 == 0 ? rnd() % 700 : rnd() % 60) : -1;
            int r = do_op(&o);
            ops_done++;
            if (!arm || (!tripped && crash_after != 0 && r == 0)) {
                if (r == 0) { ok_ops++; if (npend) BAD("seq %ld op %d: операция вернула успех, а в кэше записи %d секторов без flush", q, n, npend); }
                if (!tripped) {
                    if (r != 0 && r != KFS_ERR_NOSPC && r != KFS_ERR_EXIST) BAD("seq %ld op %d: неожиданная ошибка %s", q, n, kfs_strerror(r));
                    crash_after = -1;
                    if (r == 0) { m = nm; continue; }
                    continue;
                }
            }
            if (!tripped) { crash_after = -1; if (r == 0) m = nm; continue; }
            /* питание пропало внутри операции (или ровно на её записи) */
            tripped_n++; crashes++;
            power_loss();
            if (kfs_mount(&dev, 16)) { BAD("seq %ld op %d: монтирование после обрыва: %d", q, n, kfs_mount(&dev, 16)); goto next_seq; }
            kfs_info_t i1; kfs_info(&i1);
            int old_ok = matches(o.slot, &m.e[o.slot]), new_ok = matches(o.slot, &nm.e[o.slot]);
            if (!old_ok && !new_ok) BAD("seq %ld op %d (op=%d %s): путь не в старом и не в новом состоянии", q, n, o.op, path[o.slot]);
            for (int s = 0; s < NSLOT; s++) if (s != o.slot && !matches(s, &m.e[s])) BAD("seq %ld op %d (op=%d %s): затронут посторонний путь %s", q, n, o.op, path[o.slot], path[s]);
            if (new_ok && !old_ok) m = nm;
            if (kfs_mount(&dev, 16)) { BAD("seq %ld op %d: повторное монтирование", q, n); goto next_seq; }
            kfs_info_t i2; kfs_info(&i2);
            if (i2.repaired != 0 || i1.free_blocks != i2.free_blocks || i1.free_inodes != i2.free_inodes)
                BAD("seq %ld op %d: повторное монтирование изменило состояние (repaired %u, блоки %u->%u, иноды %u->%u)", q, n, i2.repaired, i1.free_blocks, i2.free_blocks, i1.free_inodes, i2.free_inodes);
        }
        /* итог последовательности: стирание и проверка утечек (на копии не нужно: том дальше не используется) */
        { char w[48]; snprintf(w, sizeof w, "seq %ld", q); npend = 0; crash_after = -1; tripped = 0; wipe_check(&m, w); }
        next_seq:;
    }
    printf("kfs_crash_test: последовательностей %ld, операций %ld (успешных %ld), обрывов питания %ld, нарушений %d\n", nseq, ops_done, ok_ops, crashes, fails);
    if (!crashes) { printf("FAIL: обрывов не было - тест ничего не проверяет\n"); return 1; }
    return fails ? 1 : 0;
}
