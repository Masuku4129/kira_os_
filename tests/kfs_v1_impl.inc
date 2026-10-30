#include "kfs.h"
#include "store.h"      /* crc32 */

/* Одна файловая система - один блокирующий мьютекс: операции идут с диском и длятся долго, поэтому спин-замок
   не годится (ожидающий крутился бы с выключенными прерываниями, а поток выше приоритетом мог бы навсегда вытеснить
   владельца). В хост-тестах и в инструменте kfstool потоков нет, мьютекс не нужен. */
#if defined(KIRA_HOST_TEST) || defined(KFS_NO_THREADS)
#define KFS_LOCK()   ((void)0)
#define KFS_UNLOCK() ((void)0)
#else
#include "kmutex.h"
static kmutex_t kfs_mx = KMUTEX_INIT;
#define KFS_LOCK()   kmutex_lock(&kfs_mx)
#define KFS_UNLOCK() kmutex_unlock(&kfs_mx)
#endif

#define MAGIC 0x3130534652494B4BULL     /* "KKIRFS01" */
#define VERSION 1
#define SPB (KFS_BLOCK / BLK_SECTOR)     /* секторов в блоке */
#define MAX_INODES 1024
#define MAX_BLOCKS 32768
#define NDIRECT 12
#define PTRS (KFS_BLOCK / 4)
#define ROOT 1
#define DIRENT_SIZE 64
#define MAX_BLK_LIST (NDIRECT + PTRS)

typedef struct {
    uint32_t type, flags;
    uint64_t size;
    uint32_t direct[NDIRECT];
    uint32_t indirect;
    uint32_t gen;
    uint32_t reserved[13];
    uint32_t crc;                        /* по первым 124 байтам */
} inode_t;
_Static_assert(sizeof(inode_t) == 128, "inode must be 128 bytes");

typedef struct {
    uint64_t magic;
    uint32_t version, block_size, total_blocks, inode_count, inode_start, data_start;
    uint8_t pad[512 - 8 - 6 * 4 - 4];
    uint32_t crc;                        /* по первым 508 байтам */
} super_t;
_Static_assert(sizeof(super_t) == 512, "superblock must be one sector");

typedef struct { uint32_t ino; char name[DIRENT_SIZE - 4]; } dirent_t;

static struct {
    blk_dev_t* dev;
    uint64_t base;
    int mounted;
    uint32_t total_blocks, inode_count, inode_start, data_start;
    uint32_t free_blocks, hint, repaired;
    inode_t ino[MAX_INODES];
    uint8_t used[MAX_BLOCKS / 8];
    uint8_t visited[MAX_INODES];
    uint32_t parent[MAX_INODES];
    uint8_t blk[KFS_BLOCK];              /* рабочий блок */
    uint32_t list[MAX_BLK_LIST];         /* рабочий список блоков */
    uint32_t list2[MAX_BLK_LIST];
    uint32_t list3[MAX_BLK_LIST];        /* список блоков каталога при обходе */
    uint32_t newlist[MAX_BLK_LIST];      /* блоки записываемого файла */
} fs;

/* --- сектора --- */

static int rd(uint64_t sec, uint32_t n, void* b) { return fs.dev->read(fs.dev, fs.base + sec, n, b) == 0 ? 0 : KFS_ERR_IO; }
static int wr(uint64_t sec, uint32_t n, const void* b) { return fs.dev->write(fs.dev, fs.base + sec, n, b) == 0 ? 0 : KFS_ERR_IO; }
static int flush(void) { return fs.dev->flush ? (fs.dev->flush(fs.dev) == 0 ? 0 : KFS_ERR_IO) : 0; }
static int rd_block(uint32_t b, void* buf) { return rd((uint64_t)b * SPB, SPB, buf); }
static int wr_block(uint32_t b, const void* buf) { return wr((uint64_t)b * SPB, SPB, buf); }

static void zero(void* p, size_t n) { uint8_t* q = (uint8_t*)p; while (n--) *q++ = 0; }
static void copy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* --- карта блоков (только в памяти) --- */

static int is_used(uint32_t b) { return fs.used[b >> 3] & (1u << (b & 7)); }
static void set_used(uint32_t b) { if (!is_used(b)) { fs.used[b >> 3] |= (uint8_t)(1u << (b & 7)); fs.free_blocks--; } }
static void set_free(uint32_t b) { if (b >= fs.data_start && b < fs.total_blocks && is_used(b)) { fs.used[b >> 3] &= (uint8_t)~(1u << (b & 7)); fs.free_blocks++; } }

static uint32_t alloc_block(void) {
    for (uint32_t n = 0; n < fs.total_blocks - fs.data_start; n++) {
        uint32_t b = fs.data_start + (fs.hint - fs.data_start + n) % (fs.total_blocks - fs.data_start);
        if (!is_used(b)) { set_used(b); fs.hint = b + 1; if (fs.hint >= fs.total_blocks) fs.hint = fs.data_start; return b; }
    }
    return 0;
}

/* --- иноды --- */

static uint32_t inode_crc(const inode_t* i) { return crc32(i, sizeof(inode_t) - 4); }

static int write_inode(uint32_t n) {
    fs.ino[n].crc = inode_crc(&fs.ino[n]);
    uint32_t group = n & ~3u;                                   /* 4 инода = 1 сектор: пишем сектор целиком */
    return wr((uint64_t)fs.inode_start * SPB + group / 4, 1, &fs.ino[group]);
}

static uint32_t alloc_inode(void) {
    for (uint32_t i = 2; i < fs.inode_count; i++) if (fs.ino[i].type == 0) return i;
    return 0;
}

/* --- блоки файла --- */

static uint32_t data_blocks(const inode_t* i) { return (uint32_t)((i->size + KFS_BLOCK - 1) / KFS_BLOCK); }

/* Список блоков с данными инода в out; индексный блок кладётся в *ind (0, если его нет). */
static int block_list(const inode_t* in, uint32_t* out, uint32_t* ind) {
    uint32_t n = data_blocks(in);
    if (n > MAX_BLK_LIST) return KFS_ERR_CORRUPT;
    *ind = 0;
    for (uint32_t i = 0; i < n && i < NDIRECT; i++) out[i] = in->direct[i];
    if (n > NDIRECT) {
        if (!in->indirect) return KFS_ERR_CORRUPT;
        *ind = in->indirect;
        int r = rd_block(in->indirect, fs.blk);
        if (r) return r;
        const uint32_t* p = (const uint32_t*)fs.blk;
        for (uint32_t i = NDIRECT; i < n; i++) out[i] = p[i - NDIRECT];
    }
    return n;
}

static int valid_data_block(uint32_t b) { return b >= fs.data_start && b < fs.total_blocks; }

static void free_inode_blocks(const inode_t* in) {
    uint32_t ind;
    int n = block_list(in, fs.list2, &ind);
    if (n < 0) return;                                          /* повреждённый список: блоки вернёт следующий fsck */
    for (int i = 0; i < n; i++) set_free(fs.list2[i]);
    if (ind) set_free(ind);
}

/* --- каталоги: файл из записей по 64 байта; запись пишется одним сектором --- */

typedef struct { uint32_t block; uint32_t sector; uint32_t slot; } dloc_t;

/* Обход записей каталога dir. f вызывается для каждой ЗАНЯТОЙ записи; ненулевой результат - стоп.
   free_loc (если не 0) получает первое свободное место. */
typedef int (*dir_fn)(void* ctx, dirent_t* e, const dloc_t* loc);

static int dir_walk(uint32_t dir, dir_fn f, void* ctx, dloc_t* free_loc, int* have_free) {
    const inode_t* d = &fs.ino[dir];
    if (have_free) *have_free = 0;
    uint32_t ind, nb = d->size / KFS_BLOCK;
    uint32_t* blocks = fs.list3;
    if (nb > MAX_BLK_LIST) return KFS_ERR_CORRUPT;
    int n = block_list(d, blocks, &ind);
    if (n < 0) return n;
    for (uint32_t bi = 0; bi < nb; bi++) {
        for (uint32_t s = 0; s < SPB; s++) {
            uint8_t sec[BLK_SECTOR];
            int r = rd((uint64_t)blocks[bi] * SPB + s, 1, sec);
            if (r) return r;
            dirent_t* es = (dirent_t*)sec;
            for (uint32_t k = 0; k < BLK_SECTOR / DIRENT_SIZE; k++) {
                dloc_t loc = { blocks[bi], s, k };
                if (es[k].ino == 0) {
                    if (free_loc && have_free && !*have_free) { *free_loc = loc; *have_free = 1; }
                    continue;
                }
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
/* 1 - найдено (ino, loc заполнены), 0 - нет, <0 - ошибка */
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
    /* места нет: новый блок каталога. Сначала сам блок, потом переключаем иноду каталога. */
    inode_t* d = &fs.ino[dir];
    uint32_t nb = d->size / KFS_BLOCK;
    if (nb >= NDIRECT) return KFS_ERR_NOSPC;                    /* каталог ограничен прямыми блоками: 12 x 64 записи */
    uint32_t b = alloc_block();
    if (!b) return KFS_ERR_NOSPC;
    zero(fs.blk, KFS_BLOCK);
    dirent_t* e = (dirent_t*)fs.blk;
    e[0].ino = child;
    copy(e[0].name, name, slen(name));
    r = wr_block(b, fs.blk);
    if (!r) r = flush();
    if (r) { set_free(b); return r; }
    inode_t save = *d;
    d->direct[nb] = b; d->size += KFS_BLOCK; d->gen++;
    r = write_inode(dir);
    if (!r) r = flush();
    if (r) { *d = save; set_free(b); }
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

/* Берёт следующий компонент пути. 0 - конец пути, 1 - компонент в name, <0 - ошибка. */
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

/* Разбирает путь: *parent - каталог, в котором лежит последний компонент; leaf - его имя ("" для "/").
   Для существующего пути *ino - его инод, иначе 0. */
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
        if (fs.ino[cur].type != KFS_DIR) return KFS_ERR_NOTDIR;
        *parent = cur;
        copy(leaf, name, slen(name) + 1);
        uint32_t child = 0;
        int f = dir_find(cur, name, &child, 0);
        if (f < 0) return f;
        const char* q = p; char peek[KFS_NAME_MAX + 1];
        int more = next_comp(&q, peek);
        if (more < 0) return more;
        if (!f) {
            if (more) return KFS_ERR_NOENT;                     /* промежуточного каталога нет */
            *ino = 0; return 0;
        }
        *ino = child;
        if (!more) return 0;
        cur = child;
    }
}

/* --- монтирование и проверка --- */

static int load_inodes(void) {
    for (uint32_t i = 0; i < fs.inode_count; i += 128) {         /* по 32 КБ */
        uint32_t n = fs.inode_count - i < 128 ? fs.inode_count - i : 128;
        int r = rd((uint64_t)fs.inode_start * SPB + i / 4, n / 4, &fs.ino[i]);
        if (r) return r;
    }
    return 0;
}

static int check_blocks(const inode_t* in, int is_dir) {
    if (in->type == KFS_DIR && (in->size % KFS_BLOCK)) return 0;
    if (is_dir && in->size / KFS_BLOCK > NDIRECT) return 0;
    uint32_t n = data_blocks(in);
    if (in->size > KFS_MAX_FILE) return 0;
    uint32_t ind;
    int c = block_list(in, fs.list, &ind);
    if (c < 0) return c == KFS_ERR_IO ? c : 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!valid_data_block(fs.list[i])) return 0;
        for (uint32_t k = 0; k < i; k++) if (fs.list[k] == fs.list[i]) return 0;
    }
    if (ind) {
        if (!valid_data_block(ind)) return 0;
        for (uint32_t i = 0; i < n; i++) if (fs.list[i] == ind) return 0;
    }
    return 1;
}

static int clear_inode(uint32_t n) {
    zero(&fs.ino[n], sizeof(inode_t));
    fs.repaired++;
    return write_inode(n);
}

typedef struct { uint32_t dir; int err; } scan_t;

static int scan_cb(void* c, dirent_t* e, const dloc_t* loc) {
    scan_t* s = (scan_t*)c;
    uint32_t n = e->ino;
    int bad = n >= fs.inode_count || n < 2 || fs.ino[n].type == 0 || fs.visited[n];
    if (!bad) { fs.visited[n] = 1; fs.parent[n] = s->dir; return 0; }
    s->err = write_dirent(loc, 0, 0);                           /* битая или повторная ссылка */
    fs.repaired++;
    return s->err;
}

static int mount_scan(void) {
    int r = load_inodes();
    if (r) return r;
    /* 1. иноды: тип и CRC, затем корректность списка блоков */
    for (uint32_t i = 1; i < fs.inode_count; i++) {
        inode_t* in = &fs.ino[i];
        if (in->type == 0) continue;
        int ok = (in->type == KFS_FILE || in->type == KFS_DIR) && in->crc == inode_crc(in) && check_blocks(in, in->type == KFS_DIR);
        if (!ok) { r = clear_inode(i); if (r) return r; }
    }
    if (fs.ino[ROOT].type != KFS_DIR) return KFS_ERR_CORRUPT;
    /* 2. достижимость от корня; записи с битыми ссылками удаляются */
    for (uint32_t i = 0; i < fs.inode_count; i++) { fs.visited[i] = 0; fs.parent[i] = 0; }
    fs.visited[ROOT] = 1;
    for (uint32_t pass = 0; pass < 64; pass++) {                 /* каталоги обходятся волнами по глубине */
        int progressed = 0;
        for (uint32_t i = 1; i < fs.inode_count; i++) {
            if (!fs.visited[i] || fs.ino[i].type != KFS_DIR || fs.visited[i] == 2) continue;
            fs.visited[i] = 2;                                   /* 2 - содержимое просмотрено */
            scan_t s = { i, 0 };
            r = dir_walk(i, scan_cb, &s, 0, 0);
            if (r < 0) return r;
            progressed = 1;
        }
        if (!progressed) break;
    }
    /* 3. занятые блоки; перекрёстные ссылки - повод убрать инод целиком */
    for (uint32_t b = 0; b < fs.data_start; b++) fs.used[b >> 3] |= (uint8_t)(1u << (b & 7));
    fs.free_blocks = fs.total_blocks - fs.data_start;
    for (uint32_t i = 1; i < fs.inode_count; i++) {
        inode_t* in = &fs.ino[i];
        if (in->type == 0) continue;
        if (!fs.visited[i]) { r = clear_inode(i); if (r) return r; continue; }    /* потеряшка: недоделанная операция */
        uint32_t ind;
        int n = block_list(in, fs.list, &ind);
        if (n < 0) return n;
        int clash = 0;
        for (int k = 0; k < n; k++) if (is_used(fs.list[k])) clash = 1;
        if (ind && is_used(ind)) clash = 1;
        if (clash) {
            r = clear_inode(i);
            if (r) return r;
            fs.visited[i] = 0;
            continue;
        }
        for (int k = 0; k < n; k++) set_used(fs.list[k]);
        if (ind) set_used(ind);
    }
    fs.hint = fs.data_start;
    if (fs.repaired) { r = flush(); if (r) return r; }
    return 0;
}

static int read_super(blk_dev_t* dev, uint64_t base, super_t* sb) {
    if (dev->read(dev, base, 1, sb) != 0) return KFS_ERR_IO;
    if (sb->magic != MAGIC) return KFS_ERR_NOFS;
    if (sb->crc != crc32(sb, sizeof(super_t) - 4)) return KFS_ERR_CORRUPT;
    if (sb->version != VERSION || sb->block_size != KFS_BLOCK) return KFS_ERR_NOFS;
    if (sb->total_blocks < 8 || sb->total_blocks > MAX_BLOCKS || sb->inode_count < 32 || sb->inode_count > MAX_INODES ||
        (sb->inode_count & 31) || sb->inode_start != 1 || sb->data_start != 1 + sb->inode_count * 128 / KFS_BLOCK ||
        sb->data_start >= sb->total_blocks)
        return KFS_ERR_CORRUPT;
    if (base + (uint64_t)sb->total_blocks * SPB > dev->sectors) return KFS_ERR_CORRUPT;
    return 0;
}

static int kfs_mount_impl(blk_dev_t* dev, uint64_t base) {
    fs.mounted = 0;
    if (!dev) return KFS_ERR_NOFS;
    super_t sb;
    int r = read_super(dev, base, &sb);
    if (r) return r;
    fs.dev = dev; fs.base = base;
    fs.total_blocks = sb.total_blocks; fs.inode_count = sb.inode_count;
    fs.inode_start = sb.inode_start; fs.data_start = sb.data_start;
    fs.repaired = 0;
    zero(fs.used, sizeof fs.used);
    r = mount_scan();
    if (r) return r;
    fs.mounted = 1;
    return 0;
}

static int kfs_format_impl(blk_dev_t* dev, uint64_t base) {
    if (!dev || base >= dev->sectors) return KFS_ERR_INVAL;
    uint64_t blocks = (dev->sectors - base) / SPB;
    if (blocks > MAX_BLOCKS) blocks = MAX_BLOCKS;
    if (blocks < 16) return KFS_ERR_NOSPC;
    uint32_t inodes = (uint32_t)(blocks / 4) & ~31u;
    if (inodes > MAX_INODES) inodes = MAX_INODES;
    if (inodes < 32) inodes = 32;
    fs.dev = dev; fs.base = base; fs.mounted = 0;
    uint32_t itab = inodes * 128 / KFS_BLOCK;
    super_t sb;
    zero(&sb, sizeof sb);
    sb.magic = MAGIC; sb.version = VERSION; sb.block_size = KFS_BLOCK; sb.total_blocks = (uint32_t)blocks;
    sb.inode_count = inodes; sb.inode_start = 1; sb.data_start = 1 + itab;
    sb.crc = crc32(&sb, sizeof sb - 4);
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
    st->type = fs.ino[ino].type; st->size = fs.ino[ino].size; st->ino = ino;
    return 0;
}

static int64_t kfs_read_impl(const char* path, uint64_t off, void* buf, size_t len) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!ino) return KFS_ERR_NOENT;
    inode_t* in = &fs.ino[ino];
    if (in->type != KFS_FILE) return KFS_ERR_ISDIR;
    if (off >= in->size) return 0;
    if (len > in->size - off) len = (size_t)(in->size - off);
    uint32_t ind;
    int n = block_list(in, fs.list, &ind);
    if (n < 0) return n;
    uint8_t* out = (uint8_t*)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bi = (uint32_t)(pos / KFS_BLOCK), bo = (uint32_t)(pos % KFS_BLOCK);
        size_t chunk = KFS_BLOCK - bo; if (chunk > len - done) chunk = len - done;
        if (bo == 0 && chunk == KFS_BLOCK) r = rd_block(fs.list[bi], out + done);
        else { r = rd_block(fs.list[bi], fs.blk); if (!r) copy(out + done, fs.blk + bo, chunk); }
        if (r) return r;
        done += chunk;
    }
    return (int64_t)done;
}

static int kfs_write_impl(const char* path, const void* data, size_t len) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!leaf[0]) return KFS_ERR_ISDIR;
    if (fs.ino[parent].type != KFS_DIR) return KFS_ERR_NOTDIR;
    if (ino && fs.ino[ino].type != KFS_FILE) return KFS_ERR_ISDIR;
    if (len > KFS_MAX_FILE) return KFS_ERR_FBIG;
    uint32_t nb = (uint32_t)((len + KFS_BLOCK - 1) / KFS_BLOCK);
    uint32_t slot = ino;
    if (!slot) { slot = alloc_inode(); if (!slot) return KFS_ERR_NOSPC; }
    uint32_t need = nb + (nb > NDIRECT ? 1 : 0);
    if (need > fs.free_blocks) return KFS_ERR_NOSPC;

    inode_t old = fs.ino[slot];
    uint32_t* newlist = fs.newlist; uint32_t nind = 0;
    for (uint32_t i = 0; i < nb; i++) {
        newlist[i] = alloc_block();
        if (!newlist[i]) { for (uint32_t k = 0; k < i; k++) set_free(newlist[k]); return KFS_ERR_NOSPC; }
    }
    if (nb > NDIRECT) {
        nind = alloc_block();
        if (!nind) { for (uint32_t k = 0; k < nb; k++) set_free(newlist[k]); return KFS_ERR_NOSPC; }
    }
    const uint8_t* src = (const uint8_t*)data;
    for (uint32_t i = 0; i < nb && !r; i++) {
        size_t chunk = len - (size_t)i * KFS_BLOCK; if (chunk > KFS_BLOCK) chunk = KFS_BLOCK;
        if (chunk == KFS_BLOCK) r = wr_block(newlist[i], src + (size_t)i * KFS_BLOCK);
        else { zero(fs.blk, KFS_BLOCK); copy(fs.blk, src + (size_t)i * KFS_BLOCK, chunk); r = wr_block(newlist[i], fs.blk); }
    }
    if (!r && nind) {
        zero(fs.blk, KFS_BLOCK);
        uint32_t* p = (uint32_t*)fs.blk;
        for (uint32_t i = NDIRECT; i < nb; i++) p[i - NDIRECT] = newlist[i];
        r = wr_block(nind, fs.blk);
    }
    if (!r) r = flush();                                         /* данные на диске до переключения инода */
    if (r) {
        for (uint32_t k = 0; k < nb; k++) set_free(newlist[k]);
        if (nind) set_free(nind);
        return r;
    }
    /* список старых блоков запоминаем до записи инода; в карте они остаются занятыми до успешного переключения */
    uint32_t oldn = 0, oldind = 0;
    if (ino) { int c = block_list(&old, fs.list2, &oldind); oldn = c < 0 ? 0 : (uint32_t)c; }
    inode_t* in = &fs.ino[slot];
    zero(in, sizeof *in);
    in->type = KFS_FILE; in->size = len; in->gen = old.gen + 1;
    for (uint32_t i = 0; i < nb && i < NDIRECT; i++) in->direct[i] = newlist[i];
    in->indirect = nind;
    r = write_inode(slot);
    if (!r) r = flush();
    if (r) {                                                     /* инод не переключён: возвращаем всё как было */
        *in = old;
        for (uint32_t k = 0; k < nb; k++) set_free(newlist[k]);
        if (nind) set_free(nind);
        return r;
    }
    if (ino) {
        for (uint32_t k = 0; k < oldn; k++) set_free(fs.list2[k]);
        if (oldind) set_free(oldind);
        return 0;
    }
    r = dir_add(parent, leaf, slot);
    if (r) {                                                     /* без записи в каталоге файл не существует */
        for (uint32_t k = 0; k < nb; k++) set_free(newlist[k]);
        if (nind) set_free(nind);
        zero(in, sizeof *in);
        write_inode(slot); flush();
        return r;
    }
    return 0;
}

static int kfs_mkdir_impl(const char* path) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!leaf[0] || ino) return KFS_ERR_EXIST;
    if (fs.ino[parent].type != KFS_DIR) return KFS_ERR_NOTDIR;
    uint32_t slot = alloc_inode();
    if (!slot) return KFS_ERR_NOSPC;
    inode_t* in = &fs.ino[slot];
    zero(in, sizeof *in);
    in->type = KFS_DIR; in->gen = 1;
    r = write_inode(slot);
    if (!r) r = flush();
    if (!r) r = dir_add(parent, leaf, slot);
    if (r) { zero(in, sizeof *in); write_inode(slot); flush(); }
    return r;
}

static int kfs_unlink_impl(const char* path) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!leaf[0]) return KFS_ERR_INVAL;                          /* корень не удаляется */
    if (!ino) return KFS_ERR_NOENT;
    if (fs.ino[ino].type == KFS_DIR) {
        empty_t e = { 1 };
        r = dir_walk(ino, nonempty_cb, &e, 0, 0);
        if (r < 0) return r;
        if (!e.empty) return KFS_ERR_NOTEMPTY;
    }
    r = dir_remove(parent, leaf);                                /* сначала убираем имя, потом сам инод */
    if (r) return r;
    inode_t old = fs.ino[ino];
    zero(&fs.ino[ino], sizeof(inode_t));
    r = write_inode(ino);
    if (!r) r = flush();
    free_inode_blocks(&old);
    return r;
}

typedef struct { kfs_list_cb cb; void* ctx; } list_t;
static int list_cb(void* c, dirent_t* e, const dloc_t* l) {
    (void)l;
    list_t* li = (list_t*)c;
    if (e->ino >= fs.inode_count || fs.ino[e->ino].type == 0) return 0;
    kfs_stat_t st = { fs.ino[e->ino].type, fs.ino[e->ino].size, e->ino };
    return li->cb(li->ctx, e->name, &st);
}

static int kfs_list_impl(const char* path, kfs_list_cb cb, void* ctx) {
    uint32_t parent, ino; char leaf[KFS_NAME_MAX + 1];
    int r = resolve(path, &parent, leaf, &ino);
    if (r) return r;
    if (!ino) return KFS_ERR_NOENT;
    if (fs.ino[ino].type != KFS_DIR) return KFS_ERR_NOTDIR;
    list_t l = { cb, ctx };
    r = dir_walk(ino, list_cb, &l, 0, 0);
    return r < 0 ? r : 0;
}

static void kfs_info_impl(kfs_info_t* o) {
    o->total_blocks = fs.total_blocks; o->free_blocks = fs.free_blocks; o->inodes = fs.inode_count;
    uint32_t used = 0;
    for (uint32_t i = 1; i < fs.inode_count; i++) if (fs.ino[i].type) used++;
    o->free_inodes = fs.inode_count - 1 - used; o->repaired = fs.repaired;
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
    default: return "ошибка";
    }
}

/* --- публичный интерфейс: каждая операция под мьютексом --- */

int kfs_detect(blk_dev_t* dev, uint64_t base) {
    union { super_t sb; uint8_t raw[BLK_SECTOR]; } u;
    _Static_assert(sizeof(super_t) <= BLK_SECTOR, "superblock fits a sector");
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
