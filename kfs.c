#include "kfs.h"
#include "store.h"      /* crc32 */

/* KiraFS v2. Границы определяет диск, а не код:
   - размер тома - весь диск (до 2^32 блоков по 4 КБ = 16 ТБ: номер блока - 32 бита); вырос диск (образ увеличили,
     раздел расширили) - при монтировании растёт и том;
   - файлы любого размера: 12 прямых блоков, косвенный, дважды и трижды косвенный (до ~4 ТБ);
   - каталоги без предела числа записей (растут теми же косвенными блоками);
   - таблица инодов - до 32 участков: кончаются иноды - система сама добавляет участок из свободного места;
   - в памяти: карта блоков (1 бит на блок) и тип инода (1 байт на инод) - всё по размеру тома; сами иноды - в кэше.
   Тома v1 (до 128 МБ, файлы до 4 МБ) монтируются как есть и при первом монтировании получают суперблок v2
   (раскладка та же; меняется один сектор).
   Расчёт на обрыв питания без журнала - как в v1:
   - изменение метаданных - запись ОДНОГО сектора; данные и новые косвенные блоки пишутся и сбрасываются ДО того,
     как инод переключают на них: файл после сбоя - либо целиком старый, либо целиком новый;
   - карта блоков на диске не хранится: при монтировании она строится по инодам, заодно подбираются потерянные
     иноды и блоки, убираются записи каталогов с битыми ссылками. */

#if defined(KIRA_HOST_TEST) || defined(KFS_NO_THREADS)
#include <stdlib.h>
#define KFS_LOCK()   ((void)0)
#define KFS_UNLOCK() ((void)0)
static void* kalloc(size_t n) { return calloc(1, n ? n : 1); }
static void  kfree_(void* p) { free(p); }
#else
#include "kmutex.h"
#include "heap.h"
#include "sched.h"
static kmutex_t kfs_mx = KMUTEX_INIT;
/* KFS transactions may call public kfs_* operations recursively. Keep one owner/depth
   record under a short spinlock; never hold the metadata spinlock across disk I/O or sleep. */
static spinlock_t kfs_owner_guard = SPINLOCK_INIT;
static void* kfs_owner;
static unsigned kfs_lock_depth;
static void kfs_lock(void) {
    void* self = sched_current();
    if (!self) { kmutex_lock(&kfs_mx); return; }
    uint64_t f = spin_lock_irqsave(&kfs_owner_guard);
    if (kfs_owner == self) {
        ++kfs_lock_depth;
        spin_unlock_irqrestore(&kfs_owner_guard, f);
        return;
    }
    spin_unlock_irqrestore(&kfs_owner_guard, f);
    kmutex_lock(&kfs_mx);
    f = spin_lock_irqsave(&kfs_owner_guard);
    kfs_owner = self;
    kfs_lock_depth = 1;
    spin_unlock_irqrestore(&kfs_owner_guard, f);
}
static void kfs_unlock(void) {
    void* self = sched_current();
    if (!self) { kmutex_unlock(&kfs_mx); return; }
    uint64_t f = spin_lock_irqsave(&kfs_owner_guard);
    if (kfs_owner != self || kfs_lock_depth == 0) {
        spin_unlock_irqrestore(&kfs_owner_guard, f);
        kernel_panic("KiraFS transaction lock ownership corruption");
    }
    int release = --kfs_lock_depth == 0;
    if (release) kfs_owner = 0;
    spin_unlock_irqrestore(&kfs_owner_guard, f);
    if (release) kmutex_unlock(&kfs_mx);
}
#define KFS_LOCK()   kfs_lock()
#define KFS_UNLOCK() kfs_unlock()
static void* kalloc(size_t n) { uint8_t* p = (uint8_t*)kmalloc(n ? n : 1); if (p) for (size_t i = 0; i < n; i++) p[i] = 0; return p; }
static void  kfree_(void* p) { if (p) kfree(p); }
#endif

void kfs_transaction_begin(void) { KFS_LOCK(); }
void kfs_transaction_end(void) { KFS_UNLOCK(); }

#define MAGIC 0x3130534652494B4BULL     /* "KKIRFS01" */
#define VERSION 2
#define SPB (KFS_BLOCK / BLK_SECTOR)     /* секторов в блоке */
#define NDIRECT 12
#define PTRS (KFS_BLOCK / 4)
#define IPB (KFS_BLOCK / 128)            /* инодов в блоке */
#define ROOT 1
#define DIRENT_SIZE 64
#define NEXT 32                          /* участков таблицы инодов */
#define ICACHE 64
static volatile uint32_t run_max = 256u;  /* блоков в одном запросе к диску; меняет Разум (рычаг), 16..256 */
#define RUN_MAX run_max

typedef struct {
    uint32_t type, flags;
    uint64_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;
    uint32_t gen;
    uint32_t dind, tind;                 /* v2: дважды и трижды косвенный (в v1 - нули) */
    uint32_t reserved[11];
    uint32_t crc;                        /* по первым 124 байтам */
} inode_t;
_Static_assert(sizeof(inode_t) == 128, "inode must be 128 bytes");

typedef struct { uint32_t start, count; } iext_t;   /* первый блок участка и число инодов в нём */

typedef struct {
    uint64_t magic;
    uint32_t version, block_size, total_blocks, inode_count, inode_start, data_start;
    uint32_t next;                       /* v2: участков таблицы инодов; участок 0 = (inode_start, его иноды) */
    iext_t   ext[NEXT];
    uint8_t  pad[512 - 32 - 4 - NEXT * 8 - 4];
    uint32_t crc;                        /* по первым 508 байтам */
} super_t;
_Static_assert(sizeof(super_t) == 512, "superblock must be one sector");

typedef struct { uint32_t ino; char name[DIRENT_SIZE - 4]; } dirent_t;

static struct {
    blk_dev_t* dev;
    uint64_t base;
    int mounted;
    uint32_t total_blocks, inode_count, data_start, next;
    iext_t ext[NEXT];
    uint32_t free_blocks, free_inodes, hint, ihint, repaired;
    uint8_t* used;                       /* карта блоков: total_blocks бит */
    uint8_t* itype;                      /* тип каждого инода: 0, KFS_FILE, KFS_DIR */
    struct { uint32_t n, age; inode_t in; } ic[ICACHE];
    uint32_t clock;
    uint32_t pc_no; uint32_t pc[PTRS];   /* кэш одного блока указателей */
    uint8_t blk[KFS_BLOCK];              /* рабочий блок */
    uint32_t lvl[4][PTRS];               /* блоки указателей при обходе и построении дерева */
} fs;

/* --- сектора --- */

static int rd(uint64_t sec, uint32_t n, void* b) { return fs.dev->read(fs.dev, fs.base + sec, n, b) == 0 ? 0 : KFS_ERR_IO; }
static int wr(uint64_t sec, uint32_t n, const void* b) { return fs.dev->write(fs.dev, fs.base + sec, n, b) == 0 ? 0 : KFS_ERR_IO; }
static int flush(void) { return fs.dev->flush ? (fs.dev->flush(fs.dev) == 0 ? 0 : KFS_ERR_IO) : 0; }
static int rd_block(uint32_t b, void* buf) { return rd((uint64_t)b * SPB, SPB, buf); }
static int wr_block(uint32_t b, const void* buf) {
    if (b == fs.pc_no) fs.pc_no = 0;                              /* кэш указателей больше не верен */
    return wr((uint64_t)b * SPB, SPB, buf);
}

static void zero(void* p, size_t n) { uint8_t* q = (uint8_t*)p; while (n--) *q++ = 0; }
static void copy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* --- карта блоков (в памяти) --- */

static int in_itable(uint32_t b) {
    for (uint32_t e = 0; e < fs.next; e++) if (b >= fs.ext[e].start && b < fs.ext[e].start + fs.ext[e].count / IPB) return 1;
    return 0;
}
static int valid_data_block(uint32_t b) { return b >= fs.data_start && b < fs.total_blocks && !in_itable(b); }
static int is_used(uint32_t b) { return fs.used[b >> 3] & (1u << (b & 7)); }
static void set_used(uint32_t b) { if (!is_used(b)) { fs.used[b >> 3] |= (uint8_t)(1u << (b & 7)); fs.free_blocks--; } }
static void set_free(uint32_t b) { if (valid_data_block(b) && is_used(b)) { fs.used[b >> 3] &= (uint8_t)~(1u << (b & 7)); fs.free_blocks++; } }

static uint32_t alloc_block(void) {
    if (!fs.free_blocks) return 0;
    uint32_t span = fs.total_blocks - fs.data_start;
    for (uint32_t n = 0; n < span; ) {
        uint32_t b = fs.data_start + (fs.hint - fs.data_start + n) % span;
        if (!(b & 7) && fs.used[b >> 3] == 0xFF && b + 8 <= fs.total_blocks) { n += 8; continue; }   /* занятый байт целиком */
        if (!is_used(b)) { set_used(b); fs.hint = b + 1 >= fs.total_blocks ? fs.data_start : b + 1; return b; }
        n++;
    }
    return 0;
}

/* Найти count свободных блоков подряд (для участка таблицы инодов). 0 - нет. */
static uint32_t find_run(uint32_t count) {
    uint32_t run = 0;
    for (uint32_t b = fs.data_start; b < fs.total_blocks; b++) {
        if (is_used(b)) { run = 0; continue; }
        if (++run == count) return b + 1 - count;
    }
    return 0;
}

/* --- иноды: расположение и кэш --- */

static int inode_sector(uint32_t n, uint64_t* sec, uint32_t* slot) {
    uint32_t k = n;
    for (uint32_t e = 0; e < fs.next; e++) {
        if (k < fs.ext[e].count) { *sec = (uint64_t)fs.ext[e].start * SPB + k / 4; *slot = k % 4; return 0; }
        k -= fs.ext[e].count;
    }
    return KFS_ERR_CORRUPT;
}

static uint32_t inode_crc(const inode_t* i) { return crc32(i, sizeof(inode_t) - 4); }

static void icache_drop(void) { for (int i = 0; i < ICACHE; i++) fs.ic[i].n = 0; }

/* Инод n в кэше (0 - ошибка чтения). Указатель живёт, пока не прочитано ещё ICACHE других инодов. */
static inode_t* iget(uint32_t n) {
    int victim = 0;
    for (int i = 0; i < ICACHE; i++) {
        if (fs.ic[i].n == n && n) { fs.ic[i].age = ++fs.clock; return &fs.ic[i].in; }
        if (fs.ic[i].age < fs.ic[victim].age) victim = i;
    }
    uint64_t sec; uint32_t slot;
    if (n >= fs.inode_count || inode_sector(n, &sec, &slot)) return 0;
    inode_t s[4];
    if (rd(sec, 1, s)) return 0;
    fs.ic[victim].n = n; fs.ic[victim].age = ++fs.clock; fs.ic[victim].in = s[slot];
    return &fs.ic[victim].in;
}

/* Записать инод n (его копию в кэше) - сектор целиком: чтение, замена, запись. */
static int write_inode(uint32_t n) {
    inode_t* in = iget(n);
    if (!in) return KFS_ERR_IO;
    in->crc = inode_crc(in);
    uint64_t sec; uint32_t slot;
    int r = inode_sector(n, &sec, &slot); if (r) return r;
    inode_t s[4];
    r = rd(sec, 1, s); if (r) return r;
    s[slot] = *in;
    r = wr(sec, 1, s); if (r) return r;
    fs.itype[n] = (uint8_t)in->type;
    return 0;
}

static int grow_inodes(void);

static uint32_t alloc_inode(void) {
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t k = 0; k < fs.inode_count; k++) {
            uint32_t i = 2 + (fs.ihint - 2 + k) % (fs.inode_count - 2);
            if (i >= 2 && fs.itype[i] == 0) { fs.ihint = i + 1 >= fs.inode_count ? 2 : i + 1; return i; }
        }
        if (pass == 0 && grow_inodes() != 0) break;               /* иноды кончились: таблица растёт */
    }
    return 0;
}

/* --- блоки файла: адресация --- */

static uint64_t data_blocks(const inode_t* i) { return (i->size + KFS_BLOCK - 1) / KFS_BLOCK; }
#define L1 ((uint64_t)PTRS)
#define L2 ((uint64_t)PTRS * PTRS)
#define L3 ((uint64_t)PTRS * PTRS * PTRS)
#define MAX_FILE_BLOCKS (NDIRECT + L1 + L2 + L3)

static int read_ptr(uint32_t blk, uint32_t i, uint32_t* out) {
    if (!valid_data_block(blk)) return KFS_ERR_CORRUPT;
    if (fs.pc_no != blk) { int r = rd_block(blk, fs.pc); if (r) { fs.pc_no = 0; return r; } fs.pc_no = blk; }
    *out = fs.pc[i];
    return 0;
}

/* Номер блока с данными idx файла. */
static int bmap(const inode_t* in, uint64_t idx, uint32_t* out) {
    int r;
    uint32_t p, q;
    if (idx < NDIRECT) { *out = in->direct[idx]; return 0; }
    idx -= NDIRECT;
    if (idx < L1) return read_ptr(in->indirect, (uint32_t)idx, out);
    idx -= L1;
    if (idx < L2) { if ((r = read_ptr(in->dind, (uint32_t)(idx / L1), &p))) return r; return read_ptr(p, (uint32_t)(idx % L1), out); }
    idx -= L2;
    if (idx < L3) {
        if ((r = read_ptr(in->tind, (uint32_t)(idx / L2), &p))) return r;
        if ((r = read_ptr(p, (uint32_t)((idx / L1) % L1), &q))) return r;
        return read_ptr(q, (uint32_t)(idx % L1), out);
    }
    return KFS_ERR_FBIG;
}

/* Обход всех блоков файла: данных (meta = 0) и указателей (meta = 1). cb != 0 - стоп с этим кодом. */
typedef int (*blk_cb)(void* ctx, uint32_t b, int meta);

static int walk_tree(uint32_t b, int depth, uint64_t* left, blk_cb cb, void* ctx) {
    if (!valid_data_block(b)) return KFS_ERR_CORRUPT;
    int r = cb(ctx, b, 1); if (r) return r;
    uint32_t* p = fs.lvl[depth];
    if ((r = rd_block(b, p))) return r;
    for (uint32_t i = 0; i < PTRS && *left; i++) {
        if (depth == 1) { if (!valid_data_block(p[i])) return KFS_ERR_CORRUPT; if ((r = cb(ctx, p[i], 0))) return r; (*left)--; }
        else if ((r = walk_tree(p[i], depth - 1, left, cb, ctx))) return r;
    }
    return 0;
}

static int walk_blocks(const inode_t* in, blk_cb cb, void* ctx) {
    uint64_t n = data_blocks(in);
    if (n > MAX_FILE_BLOCKS) return KFS_ERR_CORRUPT;
    int r;
    for (uint64_t i = 0; i < n && i < NDIRECT; i++) {
        if (!valid_data_block(in->direct[i])) return KFS_ERR_CORRUPT;
        if ((r = cb(ctx, in->direct[i], 0))) return r;
    }
    uint64_t left = n > NDIRECT ? n - NDIRECT : 0;
    if (left && (r = walk_tree(in->indirect, 1, &left, cb, ctx))) return r;
    if (left && (r = walk_tree(in->dind, 2, &left, cb, ctx))) return r;
    if (left && (r = walk_tree(in->tind, 3, &left, cb, ctx))) return r;
    return left ? KFS_ERR_CORRUPT : 0;
}

static int free_cb(void* c, uint32_t b, int meta) { (void)c; (void)meta; set_free(b); return 0; }
static void free_inode_blocks(const inode_t* in) { walk_blocks(in, free_cb, 0); }   /* повреждённое дерево - вернёт fsck */

/* Сколько блоков указателей нужно файлу из nb блоков. */
static uint64_t meta_blocks(uint64_t nb) {
    uint64_t m = 0;
    if (nb <= NDIRECT) return 0;
    nb -= NDIRECT;
    uint64_t a = nb < L1 ? nb : L1; m += 1; nb -= a;
    if (!nb) return m;
    a = nb < L2 ? nb : L2; m += 1 + (a + L1 - 1) / L1; nb -= a;
    if (!nb) return m;
    m += 1 + (nb + L2 - 1) / L2 + (nb + L1 - 1) / L1;
    return m;
}

/* Построить дерево указателей глубины depth над list[0..count). Номер вершины в *top; новые блоки - в meta. */
typedef struct { uint32_t* list; uint64_t n, cap; } mlist_t;

static int build_tree(int depth, const uint32_t* list, uint64_t count, uint32_t* top, mlist_t* meta) {
    uint32_t b = alloc_block();
    if (!b) return KFS_ERR_NOSPC;
    meta->list[meta->n++] = b;
    uint64_t per = depth == 1 ? 1 : depth == 2 ? L1 : L2;
    uint32_t kids = (uint32_t)((count + per - 1) / per);
    uint32_t* tmp = fs.lvl[depth];                               /* дети (глубина меньше - свой буфер) строятся раньше вершины */
    zero(tmp, KFS_BLOCK);
    for (uint32_t i = 0; i < kids; i++) {
        if (depth == 1) tmp[i] = list[i];
        else {
            uint64_t c = count - (uint64_t)i * per; if (c > per) c = per;
            int r = build_tree(depth - 1, list + (uint64_t)i * per, c, &tmp[i], meta);
            if (r) return r;
        }
    }
    int r = wr_block(b, tmp);
    if (r) return r;
    *top = b;
    return 0;
}

/* Записать в inode новый список блоков (дерево указателей строится и пишется здесь же). */
static int attach_list(inode_t* in, const uint32_t* list, uint64_t nb, mlist_t* meta) {
    for (uint64_t i = 0; i < nb && i < NDIRECT; i++) in->direct[i] = list[i];
    uint64_t at = NDIRECT; int r;
    if (nb > at) { uint64_t c = nb - at < L1 ? nb - at : L1; if ((r = build_tree(1, list + at, c, &in->indirect, meta))) return r; at += c; }
    if (nb > at) { uint64_t c = nb - at < L2 ? nb - at : L2; if ((r = build_tree(2, list + at, c, &in->dind, meta))) return r; at += c; }
    if (nb > at) { uint64_t c = nb - at; if ((r = build_tree(3, list + at, c, &in->tind, meta))) return r; }
    return 0;
}

/* Поставить блок b номером idx в существующий файл (рост каталога). Новые блоки указателей - в meta,
   изменения существующих - на месте (запись за пределом размера файла безвредна: она игнорируется). */
static int set_ptr_in(uint32_t* slot, int depth, uint64_t idx, uint32_t b, mlist_t* meta) {
    uint32_t* p = fs.lvl[depth];                                 /* у каждой глубины свой буфер: стек ядра не трогаем */
    if (!*slot) {
        uint32_t m = alloc_block();
        if (!m) return KFS_ERR_NOSPC;
        meta->list[meta->n++] = m;
        zero(p, KFS_BLOCK);
        *slot = m;
    } else {
        if (!valid_data_block(*slot)) return KFS_ERR_CORRUPT;
        int r = rd_block(*slot, p); if (r) return r;
    }
    uint64_t per = depth == 1 ? 1 : depth == 2 ? L1 : L2;
    uint32_t i = (uint32_t)(idx / per);
    if (depth == 1) p[i] = b;
    else { int r = set_ptr_in(&p[i], depth - 1, idx % per, b, meta); if (r) return r; }
    return wr_block(*slot, p);
}

static int set_ptr(inode_t* in, uint64_t idx, uint32_t b, mlist_t* meta) {
    if (idx < NDIRECT) { in->direct[idx] = b; return 0; }
    idx -= NDIRECT;
    if (idx < L1) return set_ptr_in(&in->indirect, 1, idx, b, meta);
    idx -= L1;
    if (idx < L2) return set_ptr_in(&in->dind, 2, idx, b, meta);
    idx -= L2;
    if (idx < L3) return set_ptr_in(&in->tind, 3, idx, b, meta);
    return KFS_ERR_FBIG;
}

/* --- каталоги: файл из записей по 64 байта; запись пишется одним сектором --- */

typedef struct { uint32_t block; uint32_t sector; uint32_t slot; } dloc_t;
typedef int (*dir_fn)(void* ctx, dirent_t* e, const dloc_t* loc);

static int dir_walk(uint32_t dir, dir_fn f, void* ctx, dloc_t* free_loc, int* have_free) {
    inode_t* dp = iget(dir);
    if (!dp) return KFS_ERR_IO;
    inode_t d = *dp;                                             /* копия: обратные вызовы читают другие иноды */
    if (have_free) *have_free = 0;
    uint64_t nb = d.size / KFS_BLOCK;
    for (uint64_t bi = 0; bi < nb; bi++) {
        uint32_t blk;
        int r = bmap(&d, bi, &blk); if (r) return r;
        if (!valid_data_block(blk)) return KFS_ERR_CORRUPT;
        for (uint32_t s = 0; s < SPB; s++) {
            uint8_t sec[BLK_SECTOR];
            r = rd((uint64_t)blk * SPB + s, 1, sec);
            if (r) return r;
            dirent_t* es = (dirent_t*)sec;
            for (uint32_t k = 0; k < BLK_SECTOR / DIRENT_SIZE; k++) {
                dloc_t loc = { blk, s, k };
                if (es[k].ino == 0) {
                    if (free_loc && have_free && !*have_free) { *free_loc = loc; *have_free = 1; }
                    continue;
                }
                es[k].name[DIRENT_SIZE - 5] = 0;
                if (f) { int q = f(ctx, &es[k], &loc); if (q) return q; }
            }
        }
    }
    return 0;
}

typedef struct { const char* name; uint32_t ino; dloc_t loc; } find_t;
static int find_cb(void* c, dirent_t* e, const dloc_t* loc) {
    find_t* f = (find_t*)c;
    if (seq(e->name, f->name)) { f->ino = e->ino; f->loc = *loc; return 1; }
    return 0;
}
static int dir_find(uint32_t dir, const char* name, uint32_t* ino, dloc_t* loc) {
    find_t f = { name, 0, { 0, 0, 0 } };
    int r = dir_walk(dir, find_cb, &f, 0, 0);
    if (r < 0) return r;
    if (r == 1) { *ino = f.ino; if (loc) *loc = f.loc; return 1; }
    return 0;
}

static int write_dirent(const dloc_t* loc, uint32_t ino, const char* name) {
    uint8_t sec[BLK_SECTOR];
    uint64_t lba = (uint64_t)loc->block * SPB + loc->sector;
    int r = rd(lba, 1, sec);
    if (r) return r;
    dirent_t* e = &((dirent_t*)sec)[loc->slot];
    zero(e, sizeof *e);
    e->ino = ino;
    if (name) { size_t n = slen(name); copy(e->name, name, n); }
    return wr(lba, 1, sec);
}

static int dir_add(uint32_t dir, const char* name, uint32_t child) {
    dloc_t loc; int have;
    int r = dir_walk(dir, 0, 0, &loc, &have);
    if (r < 0) return r;
    if (have) { r = write_dirent(&loc, child, name); if (r) return r; return flush(); }
    /* места нет: новый блок каталога. Сначала блок и указатели, потом переключаем иноду каталога. */
    inode_t* dp = iget(dir);
    if (!dp) return KFS_ERR_IO;
    inode_t d = *dp;
    uint64_t idx = d.size / KFS_BLOCK;
    if (fs.free_blocks < 4) return KFS_ERR_NOSPC;
    uint32_t b = alloc_block();
    if (!b) return KFS_ERR_NOSPC;
    zero(fs.blk, KFS_BLOCK);
    dirent_t* e = (dirent_t*)fs.blk;
    e[0].ino = child;
    copy(e[0].name, name, slen(name));
    r = wr_block(b, fs.blk);
    uint32_t ml[4]; mlist_t meta = { ml, 0, 4 };
    if (!r) r = set_ptr(&d, idx, b, &meta);
    if (!r) r = flush();
    if (!r) {
        d.size += KFS_BLOCK; d.gen++;
        dp = iget(dir);
        if (!dp) r = KFS_ERR_IO;
        else { inode_t save = *dp; *dp = d; r = write_inode(dir); if (!r) r = flush(); if (r) *dp = save; }
    }
    if (r) { set_free(b); for (uint64_t i = 0; i < meta.n; i++) set_free(ml[i]); }
    return r;
}

static int dir_remove(uint32_t dir, const char* name) {
    uint32_t ino; dloc_t loc;
    int r = dir_find(dir, name, &ino, &loc);
    if (r < 0) return r;
    if (r == 0) return KFS_ERR_NOENT;
    r = write_dirent(&loc, 0, 0);
    if (r) return r;
    return flush();
}

typedef struct { int empty; } empty_t;
static int nonempty_cb(void* c, dirent_t* e, const dloc_t* l) { (void)e; (void)l; ((empty_t*)c)->empty = 0; return 1; }

/* --- пути --- */

static int next_comp(const char** p, char* name) {
    const char* s = *p;
    while (*s == '/') s++;
    if (!*s) { *p = s; return 0; }
    size_t n = 0;
    while (s[n] && s[n] != '/') n++;
    if (n > KFS_NAME_MAX) return KFS_ERR_NAMETOOLONG;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return KFS_ERR_INVAL;
    for (size_t i = 0; i < n; i++) if ((uint8_t)s[i] < 0x20) return KFS_ERR_INVAL;
    copy(name, s, n); name[n] = 0;
    *p = s + n;
    return 1;
}

static int resolve(const char* path, uint32_t* parent, char* leaf, uint32_t* ino) {
    if (!fs.mounted) return KFS_ERR_NOFS;
    if (!path || path[0] != '/') return KFS_ERR_INVAL;
    uint32_t cur = ROOT;
    *parent = ROOT; leaf[0] = 0; *ino = ROOT;
    const char* p = path;
    char name[KFS_NAME_MAX + 1];
    for (;;) {
        int r = next_comp(&p, name);
        if (r < 0) return r;
        if (r == 0) return 0;
        if (fs.itype[cur] != KFS_DIR) return KFS_ERR_NOTDIR;
        *parent = cur;
        copy(leaf, name, slen(name) + 1);
        uint32_t child = 0;
        int f = dir_find(cur, name, &child, 0);
        if (f < 0) return f;
        const char* q = p; char peek[KFS_NAME_MAX + 1];
        int more = next_comp(&q, peek);
        if (more < 0) return more;
        if (!f) {
            if (more) return KFS_ERR_NOENT;
            *ino = 0; return 0;
        }
        if (child >= fs.inode_count || !fs.itype[child]) return KFS_ERR_CORRUPT;
        *ino = child;
        if (!more) return 0;
        cur = child;
    }
}

/* --- суперблок --- */

static void super_fill(super_t* sb) {
    zero(sb, sizeof *sb);
    sb->magic = MAGIC; sb->version = VERSION; sb->block_size = KFS_BLOCK; sb->total_blocks = fs.total_blocks;
    sb->inode_count = fs.inode_count; sb->inode_start = fs.ext[0].start; sb->data_start = fs.data_start;
    sb->next = fs.next;
    for (uint32_t e = 0; e < fs.next; e++) sb->ext[e] = fs.ext[e];
    sb->crc = crc32(sb, sizeof *sb - 4);
}

static int super_write(void) {
    super_t sb; super_fill(&sb);
    int r = wr(0, 1, &sb);
    if (!r) r = flush();
    return r;
}

static int read_super(blk_dev_t* dev, uint64_t base, super_t* sb) {
    if (dev->read(dev, base, 1, sb) != 0) return KFS_ERR_IO;
    if (sb->magic != MAGIC) return KFS_ERR_NOFS;
    if (sb->crc != crc32(sb, sizeof(super_t) - 4)) return KFS_ERR_CORRUPT;
    if ((sb->version != 1 && sb->version != 2) || sb->block_size != KFS_BLOCK) return KFS_ERR_NOFS;
    if (sb->version == 1) { sb->next = 1; sb->ext[0].start = sb->inode_start; sb->ext[0].count = sb->inode_count; }
    if (sb->total_blocks < 8 || sb->next < 1 || sb->next > NEXT || sb->inode_start != 1 || sb->ext[0].start != 1 ||
        sb->ext[0].count < 32 || (sb->ext[0].count % 32) || sb->data_start != 1 + sb->ext[0].count / IPB ||
        sb->data_start >= sb->total_blocks)
        return KFS_ERR_CORRUPT;
    uint64_t sum = 0;
    for (uint32_t e = 0; e < sb->next; e++) {
        const iext_t* x = &sb->ext[e];
        if (!x->count || (x->count % IPB) || x->start < (e ? sb->data_start : 1) ||
            (uint64_t)x->start + x->count / IPB > sb->total_blocks) return KFS_ERR_CORRUPT;
        for (uint32_t k = 0; k < e; k++) {                         /* участки не пересекаются */
            const iext_t* y = &sb->ext[k];
            if (x->start < y->start + y->count / IPB && y->start < x->start + x->count / IPB) return KFS_ERR_CORRUPT;
        }
        sum += x->count;
    }
    if (sum != sb->inode_count || sum > 0x7FFFFFFFu) return KFS_ERR_CORRUPT;
    if (base + (uint64_t)sb->total_blocks * SPB > dev->sectors) return KFS_ERR_CORRUPT;
    return 0;
}

/* --- монтирование и проверка --- */

static int clear_inode(uint32_t n) {
    inode_t* in = iget(n);
    if (!in) return KFS_ERR_IO;
    zero(in, sizeof *in);
    fs.repaired++;
    return write_inode(n);
}

typedef struct { uint8_t* visited; uint32_t dir; int err; } scan_t;

static int scan_cb(void* c, dirent_t* e, const dloc_t* loc) {
    scan_t* s = (scan_t*)c;
    uint32_t n = e->ino;
    int bad = n >= fs.inode_count || n < 2 || fs.itype[n] == 0 || s->visited[n];
    if (!bad) { s->visited[n] = 1; return 0; }
    s->err = write_dirent(loc, 0, 0);                           /* битая или повторная ссылка */
    fs.repaired++;
    return s->err;
}

static int check_cb(void* c, uint32_t b, int meta) { (void)c; (void)b; (void)meta; return 0; }   /* диапазон проверяет обход */

typedef struct { uint64_t marked; int clash; } mark_t;
static int mark_cb(void* c, uint32_t b, int meta) {
    (void)meta;
    mark_t* m = (mark_t*)c;
    if (is_used(b)) { m->clash = 1; return 1; }
    set_used(b); m->marked++;
    return 0;
}
typedef struct { uint64_t left; } unmark_t;
static int unmark_cb(void* c, uint32_t b, int meta) {
    (void)meta;
    unmark_t* u = (unmark_t*)c;
    if (!u->left) return 1;
    if (is_used(b)) { fs.used[b >> 3] &= (uint8_t)~(1u << (b & 7)); fs.free_blocks++; }
    u->left--;
    return 0;
}

static int inode_ok(const inode_t* in) {
    if (in->type != KFS_FILE && in->type != KFS_DIR) return 0;
    if (in->crc != inode_crc(in)) return 0;
    if (in->type == KFS_DIR && (in->size % KFS_BLOCK)) return 0;
    if (data_blocks(in) > MAX_FILE_BLOCKS) return 0;
    return 1;
}

static int mount_scan(void) {
    /* 1. иноды: тип, CRC, дерево блоков - участками по 256 */
    inode_t* chunk = (inode_t*)kalloc(256 * sizeof(inode_t));
    if (!chunk) return KFS_ERR_NOSPC;
    int r = 0;
    uint32_t base_n = 0;
    for (uint32_t e = 0; e < fs.next && !r; e++) {
        for (uint32_t k = 0; k < fs.ext[e].count && !r; k += 256) {
            uint32_t cnt = fs.ext[e].count - k < 256 ? fs.ext[e].count - k : 256;
            r = rd((uint64_t)fs.ext[e].start * SPB + k / 4, cnt / 4, chunk);
            for (uint32_t i = 0; i < cnt && !r; i++) {
                uint32_t n = base_n + k + i;
                if (n == 0 || chunk[i].type == 0) { fs.itype[n] = 0; continue; }
                int ok = inode_ok(&chunk[i]);
                if (ok) { int w = walk_blocks(&chunk[i], check_cb, 0); if (w == KFS_ERR_IO) { r = w; break; } ok = w == 0; }
                if (ok) fs.itype[n] = (uint8_t)chunk[i].type;
                else { fs.itype[n] = 0; r = clear_inode(n); }
            }
        }
        base_n += fs.ext[e].count;
    }
    kfree_(chunk);
    if (r) return r;
    if (fs.itype[ROOT] != KFS_DIR) return KFS_ERR_CORRUPT;
    /* 2. достижимость от корня; записи с битыми ссылками удаляются. Каталоги - волнами, пока есть новые. */
    uint8_t* visited = (uint8_t*)kalloc(fs.inode_count);
    if (!visited) return KFS_ERR_NOSPC;
    visited[ROOT] = 1;
    for (int progressed = 1; progressed && !r; ) {
        progressed = 0;
        for (uint32_t i = 1; i < fs.inode_count && !r; i++) {
            if (visited[i] != 1 || fs.itype[i] != KFS_DIR) continue;
            visited[i] = 2;                                     /* содержимое просмотрено */
            scan_t s = { visited, i, 0 };
            int w = dir_walk(i, scan_cb, &s, 0, 0);
            if (w < 0) r = w;
            progressed = 1;
        }
    }
    /* 3. занятые блоки; перекрёстные ссылки - повод убрать инод целиком */
    for (uint32_t i = 1; i < fs.inode_count && !r; i++) {
        if (!fs.itype[i]) continue;
        if (!visited[i]) { fs.itype[i] = 0; r = clear_inode(i); continue; }   /* потеряшка: недоделанная операция */
        inode_t* ip = iget(i);
        if (!ip) { r = KFS_ERR_IO; break; }
        inode_t in = *ip;
        mark_t m = { 0, 0 };
        int w = walk_blocks(&in, mark_cb, &m);
        if (w == KFS_ERR_IO) { r = w; break; }
        if (m.clash || w) {
            unmark_t u = { m.marked };
            walk_blocks(&in, unmark_cb, &u);                     /* снять отмеченное этим инодом (до столкновения) */
            fs.itype[i] = 0;
            r = clear_inode(i);
        }
    }
    kfree_(visited);
    if (r) return r;
    fs.free_inodes = 0;
    for (uint32_t i = 2; i < fs.inode_count; i++) if (!fs.itype[i]) fs.free_inodes++;
    fs.hint = fs.data_start; fs.ihint = 2;
    if (fs.repaired) { r = flush(); if (r) return r; }
    return 0;
}

/* Таблица инодов растёт: участок вдвое больше нынешнего числа инодов (не больше 1/16 тома), из свободного места. */
static int grow_inodes(void) {
    if (fs.next >= NEXT) return KFS_ERR_NOSPC;
    uint64_t want = fs.inode_count;
    uint64_t cap = (uint64_t)fs.total_blocks / 16 * IPB;          /* таблица не съедает больше 1/16 тома */
    if (want > cap) want = cap;
    want &= ~(uint64_t)(IPB - 1);
    if (want < IPB) want = IPB;
    if (fs.inode_count + want > 0x7FFFFFFFu) return KFS_ERR_NOSPC;
    uint32_t nblk = (uint32_t)(want / IPB);
    while (nblk && (nblk + 2 > fs.free_blocks || !find_run(nblk))) nblk /= 2;    /* сколько найдётся подряд */
    if (!nblk) return KFS_ERR_NOSPC;
    uint32_t start = find_run(nblk);
    uint8_t* nt = (uint8_t*)kalloc((size_t)fs.inode_count + (size_t)nblk * IPB);
    if (!nt) return KFS_ERR_NOSPC;
    zero(fs.blk, KFS_BLOCK);
    for (uint32_t b = 0; b < nblk; b++) { int r = wr_block(start + b, fs.blk); if (r) { kfree_(nt); return r; } }
    int r = flush();
    if (r) { kfree_(nt); return r; }
    for (uint32_t b = 0; b < nblk; b++) set_used(start + b);
    fs.ext[fs.next].start = start; fs.ext[fs.next].count = nblk * IPB; fs.next++;
    uint32_t old = fs.inode_count;
    fs.inode_count += nblk * IPB;
    r = super_write();                                           /* участок существует с этого сектора */
    if (r) {
        fs.next--; fs.inode_count = old;
        for (uint32_t b = 0; b < nblk; b++) { fs.used[(start + b) >> 3] &= (uint8_t)~(1u << ((start + b) & 7)); fs.free_blocks++; }
        kfree_(nt); return r;
    }
    copy(nt, fs.itype, old);
    kfree_(fs.itype); fs.itype = nt;
    fs.free_inodes += nblk * IPB;
    return 0;
}

/* Диск больше тома - том растёт до размера диска (карта блоков в памяти увеличивается). */
static int grow_volume(void) {
    uint64_t dev_blocks = (fs.dev->sectors - fs.base) / SPB;
    if (dev_blocks > 0xFFFFFFFFull) dev_blocks = 0xFFFFFFFFull;
    if (dev_blocks <= fs.total_blocks) return 0;
    uint8_t* nu = (uint8_t*)kalloc((size_t)((dev_blocks + 7) / 8));
    if (!nu) return 0;                                           /* мало памяти: том остаётся прежним */
    copy(nu, fs.used, (fs.total_blocks + 7) / 8);
    uint32_t old = fs.total_blocks;
    fs.total_blocks = (uint32_t)dev_blocks;
    int r = super_write();
    if (r) { fs.total_blocks = old; kfree_(nu); return r; }
    kfree_(fs.used); fs.used = nu;
    fs.free_blocks += fs.total_blocks - old;
    return 0;
}

static void release_mem(void) {
    kfree_(fs.used); fs.used = 0;
    kfree_(fs.itype); fs.itype = 0;
}

static int kfs_mount_impl(blk_dev_t* dev, uint64_t base) {
    fs.mounted = 0;
    release_mem();
    if (!dev) return KFS_ERR_NOFS;
    super_t sb;
    int r = read_super(dev, base, &sb);
    if (r) return r;
    fs.dev = dev; fs.base = base;
    fs.total_blocks = sb.total_blocks; fs.inode_count = sb.inode_count; fs.data_start = sb.data_start;
    fs.next = sb.next;
    for (uint32_t e = 0; e < NEXT; e++) fs.ext[e] = e < sb.next ? sb.ext[e] : (iext_t){ 0, 0 };
    fs.repaired = 0; fs.pc_no = 0;
    icache_drop();
    fs.used = (uint8_t*)kalloc((size_t)((fs.total_blocks + 7) / 8));
    fs.itype = (uint8_t*)kalloc(fs.inode_count);
    if (!fs.used || !fs.itype) { release_mem(); return KFS_ERR_NOSPC; }
    for (uint32_t b = 0; b < fs.data_start; b++) fs.used[b >> 3] |= (uint8_t)(1u << (b & 7));
    for (uint32_t e = 1; e < fs.next; e++)
        for (uint32_t b = fs.ext[e].start; b < fs.ext[e].start + fs.ext[e].count / IPB; b++) fs.used[b >> 3] |= (uint8_t)(1u << (b & 7));
    fs.free_blocks = 0;
    for (uint32_t b = 0; b < fs.total_blocks; b++) if (!is_used(b)) fs.free_blocks++;
    r = mount_scan();
    if (r) { release_mem(); return r; }
    if (sb.version == 1 && (r = super_write())) { release_mem(); return r; }   /* v1 -> v2: один сектор */
    if ((r = grow_volume())) { release_mem(); return r; }
    fs.mounted = 1;
    if (fs.free_inodes < fs.inode_count / 8 + 16) grow_inodes();    /* запас инодов заранее, если есть место */
    return 0;
}

static int kfs_format_impl(blk_dev_t* dev, uint64_t base) {
    if (!dev || base >= dev->sectors) return KFS_ERR_INVAL;
    uint64_t blocks = (dev->sectors - base) / SPB;
    if (blocks > 0xFFFFFFFFull) blocks = 0xFFFFFFFFull;
    if (blocks < 16) return KFS_ERR_NOSPC;
    /* иноды: по одному на 32 КБ тома, от 64 до 65536 для начала - дальше таблица растёт сама */
    uint64_t inodes = blocks / 8;
    if (inodes > 65536) inodes = 65536;
    if (inodes < 64) inodes = 64;
    inodes &= ~(uint64_t)31;
    while (1 + inodes / IPB + 8 > blocks && inodes > 32) inodes -= 32;
    fs.mounted = 0;
    release_mem();
    fs.dev = dev; fs.base = base; fs.pc_no = 0;
    icache_drop();
    uint32_t itab = (uint32_t)(inodes / IPB);
    super_t sb;
    fs.total_blocks = (uint32_t)blocks; fs.inode_count = (uint32_t)inodes; fs.data_start = 1 + itab;
    fs.next = 1; fs.ext[0].start = 1; fs.ext[0].count = (uint32_t)inodes;
    super_fill(&sb);
    zero(fs.blk, KFS_BLOCK);
    for (uint32_t b = 1; b < 1 + itab; b++) { int r = wr_block(b, fs.blk); if (r) return r; }
    int r = flush();
    if (r) return r;
    /* корень: инод 1 - каталог. Суперблок пишется последним: до этого момента диск не считается KiraFS. */
    inode_t root; zero(&root, sizeof root);
    root.type = KFS_DIR; root.gen = 1;
    root.crc = inode_crc(&root);
    uint8_t sec[BLK_SECTOR]; zero(sec, sizeof sec);
    copy(sec + sizeof(inode_t), &root, sizeof root);              /* инод 1 - вторая запись сектора */
    r = wr((uint64_t)1 * SPB, 1, sec);
    if (r) return r;
    r = flush();
    if (r) return r;
    zero(fs.blk, KFS_BLOCK); copy(fs.blk, &sb, sizeof sb);
    r = wr(0, 1, fs.blk);
    if (r) return r;
    r = flush();
    if (r) return r;
    return kfs_mount_impl(dev, base);
}

int kfs_mounted(void) { return fs.mounted; }

/* --- операции --- */

static int kfs_stat_impl(const char* path, kfs_stat_t* st) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!ino) return KFS_ERR_NOENT;
    inode_t* in = iget(ino);
    if (!in) return KFS_ERR_IO;
    st->type = in->type; st->size = in->size; st->ino = ino;
    return 0;
}

static int64_t kfs_read_impl(const char* path, uint64_t off, void* buf, size_t len) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!ino) return KFS_ERR_NOENT;
    inode_t* ip = iget(ino);
    if (!ip) return KFS_ERR_IO;
    inode_t in = *ip;
    if (in.type != KFS_FILE) return KFS_ERR_ISDIR;
    if (off >= in.size) return 0;
    if (len > in.size - off) len = (size_t)(in.size - off);
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t bi = pos / KFS_BLOCK; uint32_t bo = (uint32_t)(pos % KFS_BLOCK);
        uint32_t b;
        if ((r = bmap(&in, bi, &b))) return r;
        if (!valid_data_block(b)) return KFS_ERR_CORRUPT;
        if (bo == 0 && len - done >= KFS_BLOCK) {                  /* целые блоки подряд - одним запросом */
            uint32_t run = 1;
            size_t whole = (len - done) / KFS_BLOCK;
            while (run < whole && run < RUN_MAX) {
                uint32_t nb2;
                if (bmap(&in, bi + run, &nb2) || nb2 != b + run || !valid_data_block(nb2)) break;
                run++;
            }
            if ((r = rd((uint64_t)b * SPB, run * SPB, out + done))) return r;
            done += (size_t)run * KFS_BLOCK;
        } else {
            size_t chunk = KFS_BLOCK - bo; if (chunk > len - done) chunk = len - done;
            if ((r = rd_block(b, fs.blk))) return r;
            copy(out + done, fs.blk + bo, chunk);
            done += chunk;
        }
    }
    return (int64_t)done;
}

static int kfs_write_impl(const char* path, const void* data, size_t len) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!leaf[0]) return KFS_ERR_ISDIR;
    if (fs.itype[parent] != KFS_DIR) return KFS_ERR_NOTDIR;
    if (ino && fs.itype[ino] != KFS_FILE) return KFS_ERR_ISDIR;
    uint64_t nb = ((uint64_t)len + KFS_BLOCK - 1) / KFS_BLOCK;
    if (nb > MAX_FILE_BLOCKS) return KFS_ERR_FBIG;
    uint64_t nmeta = meta_blocks(nb);
    if (nb + nmeta > fs.free_blocks) return KFS_ERR_NOSPC;
    uint32_t slot = ino;
    if (!slot) { slot = alloc_inode(); if (!slot) return KFS_ERR_NOSPC; }
    if (nb + nmeta > fs.free_blocks) return KFS_ERR_NOSPC;      /* рост таблицы инодов мог занять место */

    uint32_t* list = (uint32_t*)kalloc((size_t)(nb ? nb : 1) * 4);
    uint32_t* mlist = (uint32_t*)kalloc((size_t)(nmeta + 1) * 4);
    if (!list || !mlist) { kfree_(list); kfree_(mlist); return KFS_ERR_NOSPC; }
    mlist_t meta = { mlist, 0, nmeta + 1 };
    uint64_t got = 0;
    for (; got < nb; got++) { list[got] = alloc_block(); if (!list[got]) { r = KFS_ERR_NOSPC; break; } }
    /* данные: подряд идущие целые блоки - одним запросом */
    const uint8_t* src = (const uint8_t*)data;
    for (uint64_t i = 0; i < nb && !r; ) {
        size_t off = (size_t)(i * KFS_BLOCK);
        size_t chunk = len - off;
        if (chunk >= KFS_BLOCK) {
            uint32_t run = 1;
            while (i + run < nb && run < RUN_MAX && list[i + run] == list[i] + run && len - off >= (size_t)(run + 1) * KFS_BLOCK) run++;
            r = wr((uint64_t)list[i] * SPB, run * SPB, src + off);
            i += run;
        } else {
            zero(fs.blk, KFS_BLOCK); copy(fs.blk, src + off, chunk);
            r = wr_block(list[i], fs.blk);
            i++;
        }
    }
    inode_t nin; zero(&nin, sizeof nin);
    if (!r) r = attach_list(&nin, list, nb, &meta);
    if (!r) r = flush();                                         /* данные и указатели на диске до переключения инода */
    if (r) {
        for (uint64_t k = 0; k < got; k++) set_free(list[k]);
        for (uint64_t k = 0; k < meta.n; k++) set_free(mlist[k]);
        kfree_(list); kfree_(mlist);
        return r;
    }
    inode_t* ip = iget(slot);
    if (!ip) r = KFS_ERR_IO;
    inode_t old = ip ? *ip : nin;
    if (!r) {
        nin.type = KFS_FILE; nin.size = len; nin.gen = (ino ? old.gen : 0) + 1;
        *ip = nin;
        r = write_inode(slot);
        if (!r) r = flush();
        if (r) { ip = iget(slot); if (ip) *ip = old; }
    }
    if (r) {                                                     /* инод не переключён: возвращаем всё как было */
        for (uint64_t k = 0; k < nb; k++) set_free(list[k]);
        for (uint64_t k = 0; k < meta.n; k++) set_free(mlist[k]);
        kfree_(list); kfree_(mlist);
        return r;
    }
    kfree_(list); kfree_(mlist);
    if (ino) { free_inode_blocks(&old); return 0; }
    fs.free_inodes--;
    r = dir_add(parent, leaf, slot);
    if (r) {                                                     /* без записи в каталоге файл не существует */
        ip = iget(slot);
        if (ip) {
            inode_t gone = *ip;
            zero(ip, sizeof *ip);
            write_inode(slot); flush();
            free_inode_blocks(&gone);
        }
        fs.itype[slot] = 0; fs.free_inodes++;
        return r;
    }
    return 0;
}

static int kfs_mkdir_impl(const char* path) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!leaf[0] || ino) return KFS_ERR_EXIST;
    if (fs.itype[parent] != KFS_DIR) return KFS_ERR_NOTDIR;
    uint32_t slot = alloc_inode();
    if (!slot) return KFS_ERR_NOSPC;
    inode_t* in = iget(slot);
    if (!in) return KFS_ERR_IO;
    zero(in, sizeof *in);
    in->type = KFS_DIR; in->gen = 1;
    r = write_inode(slot);
    if (!r) r = flush();
    if (!r) { fs.free_inodes--; r = dir_add(parent, leaf, slot); if (r) fs.free_inodes++; }
    if (r) { in = iget(slot); if (in) { zero(in, sizeof *in); write_inode(slot); flush(); } fs.itype[slot] = 0; }
    return r;
}

static int kfs_unlink_impl(const char* path) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!leaf[0]) return KFS_ERR_INVAL;                          /* корень не удаляется */
    if (!ino) return KFS_ERR_NOENT;
    if (fs.itype[ino] == KFS_DIR) {
        empty_t e = { 1 };
        r = dir_walk(ino, nonempty_cb, &e, 0, 0);
        if (r < 0) return r;
        if (!e.empty) return KFS_ERR_NOTEMPTY;
    }
    r = dir_remove(parent, leaf);                                /* сначала убираем имя, потом сам инод */
    if (r) return r;
    inode_t* ip = iget(ino);
    if (!ip) return KFS_ERR_IO;
    inode_t old = *ip;
    zero(ip, sizeof *ip);
    r = write_inode(ino);
    if (!r) r = flush();
    fs.itype[ino] = 0; fs.free_inodes++;
    free_inode_blocks(&old);
    return r;
}

typedef struct { kfs_list_cb cb; void* ctx; } list_t;
static int list_cb(void* c, dirent_t* e, const dloc_t* l) {
    (void)l;
    list_t* li = (list_t*)c;
    if (e->ino >= fs.inode_count || fs.itype[e->ino] == 0) return 0;
    inode_t* in = iget(e->ino);
    if (!in) return KFS_ERR_IO;
    kfs_stat_t st = { in->type, in->size, e->ino };
    return li->cb(li->ctx, e->name, &st);
}

static int kfs_list_impl(const char* path, kfs_list_cb cb, void* ctx) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!ino) return KFS_ERR_NOENT;
    if (fs.itype[ino] != KFS_DIR) return KFS_ERR_NOTDIR;
    list_t l = { cb, ctx };
    r = dir_walk(ino, list_cb, &l, 0, 0);
    return r < 0 ? r : 0;
}

static void kfs_info_impl(kfs_info_t* o) {
    o->total_blocks = fs.total_blocks; o->free_blocks = fs.free_blocks; o->inodes = fs.inode_count;
    o->free_inodes = fs.free_inodes; o->repaired = fs.repaired;
}

const char* kfs_strerror(int e) {
    switch (e) {
    case KFS_OK: return "ok";
    case KFS_ERR_IO: return "ошибка диска";
    case KFS_ERR_NOENT: return "нет такого файла или каталога";
    case KFS_ERR_EXIST: return "уже существует";
    case KFS_ERR_NOTDIR: return "не каталог";
    case KFS_ERR_ISDIR: return "это каталог";
    case KFS_ERR_NOSPC: return "нет места";
    case KFS_ERR_INVAL: return "неверный путь";
    case KFS_ERR_NOTEMPTY: return "каталог не пуст";
    case KFS_ERR_FBIG: return "файл слишком велик";
    case KFS_ERR_CORRUPT: return "файловая система повреждена";
    case KFS_ERR_NAMETOOLONG: return "имя слишком длинное";
    case KFS_ERR_NOFS: return "на диске нет файловой системы Киры";
    case KFS_ERR_RO: return "том подключён только для чтения";
    default: return "ошибка";
    }
}

/* --- публичный интерфейс: каждая операция под мьютексом --- */

int kfs_detect(blk_dev_t* dev, uint64_t base) {
    union { super_t sb; uint8_t raw[BLK_SECTOR]; } u;
    if (base + 1 > dev->sectors) return KFS_ERR_NOFS;
    return read_super(dev, base, &u.sb);
}

int kfs_mount(blk_dev_t* dev, uint64_t base) { KFS_LOCK(); int r = kfs_mount_impl(dev, base); KFS_UNLOCK(); return r; }
int kfs_format(blk_dev_t* dev, uint64_t base) { KFS_LOCK(); int r = kfs_format_impl(dev, base); KFS_UNLOCK(); return r; }
int kfs_stat(const char* path, kfs_stat_t* st) { KFS_LOCK(); int r = kfs_stat_impl(path, st); KFS_UNLOCK(); return r; }
int64_t kfs_read(const char* path, uint64_t off, void* buf, size_t len) { KFS_LOCK(); int64_t r = kfs_read_impl(path, off, buf, len); KFS_UNLOCK(); return r; }
int kfs_write(const char* path, const void* data, size_t len) { KFS_LOCK(); int r = kfs_write_impl(path, data, len); KFS_UNLOCK(); return r; }
int kfs_mkdir(const char* path) { KFS_LOCK(); int r = kfs_mkdir_impl(path); KFS_UNLOCK(); return r; }
int kfs_unlink(const char* path) { KFS_LOCK(); int r = kfs_unlink_impl(path); KFS_UNLOCK(); return r; }
int kfs_list(const char* path, kfs_list_cb cb, void* ctx) { KFS_LOCK(); int r = kfs_list_impl(path, cb, ctx); KFS_UNLOCK(); return r; }
void kfs_info(kfs_info_t* o) { KFS_LOCK(); kfs_info_impl(o); KFS_UNLOCK(); }

void kfs_set_run_max(uint32_t blocks) { run_max = blocks < 16 ? 16 : blocks > 256 ? 256 : blocks; }
