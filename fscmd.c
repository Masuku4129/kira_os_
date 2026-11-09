#include "fscmd.h"
#include "kfs.h"
#include "blk.h"
#include "uart.h"
#include "vfs.h"
#include "heap.h"
#include "experience.h"

static void fail(const char* what, int e) {
    uart_print(what); uart_print(": "); uart_print(kfs_strerror(e)); uart_print("\n");
}

static int need_fs(void) {
    if (vfs_any()) return 1;
    uart_print("файловая система не подключена (диск без KiraFS: 'format yes' создаст её, стерев область за хранилищем опыта)\n");
    return 0;
}

/* Путь из аргумента: без ведущего '/' добавляется корень. Результат во внутреннем буфере. */
static char pathbuf[KFS_NAME_MAX * 4 + 4];
static const char* mkpath(const char* s, unsigned n) {
    unsigned o = 0;
    if (n == 0 || s[0] != '/') pathbuf[o++] = '/';
    for (unsigned i = 0; i < n && o < sizeof pathbuf - 1; i++) pathbuf[o++] = s[i];
    pathbuf[o] = 0;
    return pathbuf;
}
static unsigned word_len(const char* s) { unsigned n = 0; while (s[n] && s[n] != ' ') n++; return n; }
static const char* skip_sp(const char* s) { while (*s == ' ') s++; return s; }

static int ls_cb(void* c, const char* name, const kfs_stat_t* st) {
    (*(unsigned*)c)++;
    if (st->type == KFS_DIR) { uart_print("  <dir>      "); }
    else {
        char b[16]; unsigned n = 0; uint64_t v = st->size;
        do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 15);
        for (unsigned i = n; i < 11; i++) uart_putchar(' ');
        uart_print("  ");
        while (n) uart_putchar(b[--n]);
        uart_print(" ");
        (void)0;
    }
    uart_print(" "); uart_print(name); uart_print("\n");
    return 0;
}

void fscmd_ls(const char* arg) {
    if (!need_fs()) return;
    arg = skip_sp(arg);
    const char* p = mkpath(arg, word_len(arg));
    unsigned n = 0;
    int r = vfs_list(p, ls_cb, &n);
    if (r) fail(p, r); else if (!n) uart_print("  (пусто)\n");
}

void fscmd_cat(const char* arg) {
    if (!need_fs()) return;
    arg = skip_sp(arg);
    if (!arg[0]) { uart_print("использование: cat путь\n"); return; }
    const char* p = mkpath(arg, word_len(arg));
    kfs_stat_t st;
    int r = vfs_stat(p, &st);
    if (r) { fail(p, r); return; }
    if (st.type != KFS_FILE) { fail(p, KFS_ERR_ISDIR); return; }
    static uint8_t chunk[256];
    uint64_t off = 0, limit = 16384;
    while (off < st.size && off < limit) {
        int64_t n = vfs_read(p, off, chunk, sizeof chunk - 1);
        if (n < 0) { fail(p, (int)n); return; }
        if (n == 0) break;
        for (int64_t i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            if (c == '\n' || c >= 0x20) uart_putchar((char)c); else uart_putchar('.');   /* управляющие коды не должны ломать экран */
        }
        off += (uint64_t)n;
    }
    if (st.size > limit) uart_print("\n...(показаны первые 16 КБ)");
    if (st.size == 0 || off == 0) uart_print("(пустой файл)");
    uart_print("\n");
}

void fscmd_write(const char* arg) {
    if (!need_fs()) return;
    arg = skip_sp(arg);
    unsigned wl = word_len(arg);
    if (!wl) { uart_print("использование: write путь текст\n"); return; }
    const char* text = skip_sp(arg + wl);
    char data[96]; unsigned n = 0;
    while (text[n] && n < sizeof data - 1) { data[n] = text[n]; n++; }
    data[n++] = '\n';
    const char* p = mkpath(arg, wl);
    int r = vfs_write(p, data, n);
    if (r) fail(p, r);
    else { uart_print(p); uart_print(": записано "); uart_print_dec(n); uart_print(" байт\n"); }
}

void fscmd_rm(const char* arg) {
    if (!need_fs()) return;
    arg = skip_sp(arg);
    if (!arg[0]) { uart_print("использование: rm путь\n"); return; }
    const char* p = mkpath(arg, word_len(arg));
    int r = vfs_unlink(p);
    if (r) fail(p, r);
}

void fscmd_mkdir(const char* arg) {
    if (!need_fs()) return;
    arg = skip_sp(arg);
    if (!arg[0]) { uart_print("использование: mkdir путь\n"); return; }
    const char* p = mkpath(arg, word_len(arg));
    int r = vfs_mkdir(p);
    if (r) fail(p, r);
}

void fscmd_df(const char* arg) {
    (void)arg;
    if (!need_fs()) return;
    static vfs_info_t m[VFS_MAX];
    int nm = vfs_mounts(m, VFS_MAX);
    for (int k = 0; k < nm; k++) {
        uart_print("/"); uart_print(m[k].name); uart_print(": FAT"); uart_print_dec((uint64_t)m[k].fat_type);
        if (m[k].label[0]) { uart_print(" «"); uart_print(m[k].label); uart_print("»"); }
        uart_print(", "); uart_print(m[k].desc); uart_print(", всего "); uart_print_dec(m[k].total_kb / 1024);
        uart_print(" МБ, свободно "); uart_print_dec(m[k].free_kb / 1024); uart_print(" МБ\n");
    }
    if (!kfs_mounted()) return;
    kfs_info_t i; kfs_info(&i);
    uart_print("KiraFS: блоков "); uart_print_dec(i.total_blocks - 0);
    uart_print(" по 4 КБ, свободно "); uart_print_dec(i.free_blocks);
    uart_print(" (" ); uart_print_dec((uint64_t)i.free_blocks * 4); uart_print(" КБ); инодов "); uart_print_dec(i.inodes);
    uart_print(", свободно "); uart_print_dec(i.free_inodes);
    if (i.repaired) { uart_print("; при загрузке исправлено: "); uart_print_dec(i.repaired); }
    uart_print("\n");
}

/* format                 - список дисков и что на них
   format ИМЯ yes         - KiraFS на пустом диске или на диске Киры (он становится системным)
   format ИМЯ стереть     - то же для диска с чужими данными (Windows, Linux, флешка): всё на нём пропадёт
   format yes             - заново на текущем системном диске */
void fscmd_format(const char* arg) {
    arg = skip_sp(arg);
    unsigned a = word_len(arg);
    const char* w2 = skip_sp(arg + a);
    unsigned b = word_len(w2);
    blk_dev_t* d = 0; const char* word = arg; unsigned wl = a;
    if (a && b) {
        char id[12]; unsigned k = 0; while (k < a && k < sizeof id - 1) { id[k] = arg[k]; k++; } id[k] = 0;
        d = blk_find(id);
        if (!d) { uart_print("нет диска "); uart_print(id); uart_print(" (список: disk)\n"); return; }
        word = w2; wl = b;
    } else d = blk_primary();
    int yes = wl == 3 && word[0] == 'y' && word[1] == 'e' && word[2] == 's';
    static const char ru[] = "стереть", en[] = "wipe";
    int wipe = (wl == sizeof ru - 1 || wl == sizeof en - 1);
    for (unsigned i = 0; wipe && i < wl; i++) if (word[i] != (wl == sizeof ru - 1 ? ru[i] : en[i])) wipe = 0;
    if (!d || (!yes && !wipe)) {
        uart_print("format создаёт KiraFS и УДАЛЯЕТ всё на диске. Диски:\n");
        blk_print();
        uart_print("  format ИМЯ yes       - пустой диск или диск Киры (станет системным)\n");
        uart_print("  format ИМЯ стереть   - диск с данными другой системы: они пропадут\n");
        return;
    }
    const blk_info_t* inf = 0;
    for (int i = 0; i < blk_count(); i++) if (blk_info(i)->dev == d) inf = blk_info(i);
    if (inf && inf->foreign && !wipe) {
        uart_print("на "); uart_print(inf->id); uart_print(" есть данные другой системы (таблица разделов или файловая система).\n");
        uart_print("Если их не жалко: format "); uart_print(inf->id); uart_print(" стереть\n");
        return;
    }
    int r = kfs_format(d, KFS_BASE_LBA);
    if (r) { fail("format", r); return; }
    if (d != blk_primary()) { blk_set_primary(d); experience_attach(); }
    uart_print("KiraFS создана и подключена");
    if (inf) { uart_print(" на "); uart_print(inf->id); }
    uart_print(".\n");
}

void fs_init(void) {
    blk_dev_t* d = blk_primary();
    if (!d) return;                              /* без диска ФС нет; сообщение уже дал experience_init */
    int r = kfs_mount(d, KFS_BASE_LBA);
    if (r == 0) {
        kfs_info_t i; kfs_info(&i);
        uart_print("[KIRA FS] KiraFS mounted: "); uart_print_dec((uint64_t)i.free_blocks * 4); uart_print(" KB free");
        if (i.repaired) { uart_print(", repaired after unclean shutdown: "); uart_print_dec(i.repaired); }
        uart_print(".\n");
    } else if (r == KFS_ERR_NOFS) {
        uart_print("[KIRA FS] No KiraFS on disk. Type 'format yes' to create one.\n");
    } else {
        uart_print("[KIRA FS] Mount failed: "); uart_print(kfs_strerror(r)); uart_print(" (data untouched; see 'format').\n");
    }
}

/* cp откуда куда: копирование между любыми томами (флешка <-> системный диск). Файл читается целиком в память. */
void fscmd_cp(const char* arg) {
    if (!need_fs()) return;
    arg = skip_sp(arg);
    unsigned a = word_len(arg);
    const char* b = skip_sp(arg + a);
    unsigned bl = word_len(b);
    if (!a || !bl) { uart_print("использование: cp откуда куда (например: cp /usb0/фото.bmp /data/фото.bmp)\n"); return; }
    static char src[KFS_NAME_MAX * 4 + 4];
    const char* sp = mkpath(arg, a);
    unsigned k = 0; while (sp[k] && k < sizeof src - 1) { src[k] = sp[k]; k++; } src[k] = 0;
    const char* dp = mkpath(b, bl);
    kfs_stat_t st;
    int r = vfs_stat(src, &st);
    if (r) { fail(src, r); return; }
    if (st.type != KFS_FILE) { fail(src, KFS_ERR_ISDIR); return; }
    if (st.size != (size_t)st.size) { fail(src, KFS_ERR_FBIG); return; }
    uint8_t* buf = (uint8_t*)kmalloc(st.size ? (size_t)st.size : 1);
    if (!buf) { uart_print("cp: не хватает памяти\n"); return; }
    int64_t got = vfs_read(src, 0, buf, (size_t)st.size);
    if (got != (int64_t)st.size) { fail(src, got < 0 ? (int)got : KFS_ERR_IO); kfree(buf); return; }
    r = vfs_write(dp, buf, (size_t)st.size);
    kfree(buf);
    if (r) { fail(dp, r); return; }
    uart_print(src); uart_print(" -> "); uart_print(dp); uart_print(": "); uart_print_dec(st.size); uart_print(" байт\n");
}

/* remount usbN rw|ro: флешки подключаются только для чтения; запись разрешает человек этой командой. */
void fscmd_remount(const char* arg) {
    arg = skip_sp(arg);
    if (arg[0] == '/') arg++;
    int ok = arg[0] == 'u' && arg[1] == 's' && arg[2] == 'b' && arg[3] >= '0' && arg[3] <= '3' && arg[4] == ' ';
    const char* m = ok ? skip_sp(arg + 4) : 0;
    int rw = m && m[0] == 'r' && m[1] == 'w' && !m[2];
    int ro = m && m[0] == 'r' && m[1] == 'o' && !m[2];
    if (!ok || (!rw && !ro)) { uart_print("использование: remount usb0 rw (разрешить запись) или remount usb0 ro (только чтение)\n"); return; }
    int n = arg[3] - '0';
    int r = vfs_set_read_only(n, ro);
    if (r) { fail(arg, r); return; }
    uart_print("/usb"); uart_putchar((char)('0' + n));
    uart_print(rw ? ": запись разрешена (перед извлечением: umount usb0)\n" : ": только чтение\n");
}

/* umount usbN: дописать всё и отключить том - флешку можно вынимать. */
void fscmd_umount(const char* arg) {
    arg = skip_sp(arg);
    if (arg[0] == '/') arg++;
    if (!(arg[0] == 'u' && arg[1] == 's' && arg[2] == 'b' && arg[3] >= '0' && arg[3] <= '3' && (!arg[4] || arg[4] == ' '))) {
        uart_print("использование: umount usb0 (тома: df)\n"); return;
    }
    int n = arg[3] - '0';
    int r = vfs_flush(n);
    if (r) { fail(arg, r); return; }
    vfs_unmount(n);
    uart_print("/usb"); uart_putchar((char)('0' + n)); uart_print(": записано и отключено - можно вынимать\n");
}
