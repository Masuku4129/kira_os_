// FAT12/16/32 против эталона: образы создаёт mkfs.fat (dosfstools), файлы кладёт mtools (mcopy), наш драйвер читает;
// затем наш драйвер пишет (длинные и русские имена, большие файлы, замена, каталоги, удаление, рост каталога),
// а fsck.fat проверяет целостность и mtools читает обратно. Плюс разделы (MBR, GPT с CRC, суперфлоппи) и порча.
// Пути к инструментам: MKFS_FAT, FSCK_FAT, MTOOLS (каталог с mcopy/mdir) - из окружения или PATH.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fat.h"
#include "part.h"
#include "vfs.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static const char* mkfs, *fsck, *mt;
static char dirt[] = "/tmp/kira_fat_XXXXXX";

/* ---- блочное устройство в памяти ---- */
typedef struct { uint8_t* m; uint64_t n; int fail_writes; } memdev_t;
static int mrd(blk_dev_t* d, uint64_t lba, uint32_t c, void* b) { memdev_t* m = d->priv; if (lba + c > m->n) return -1; memcpy(b, m->m + lba * 512, (size_t)c * 512); return 0; }
static int mwr(blk_dev_t* d, uint64_t lba, uint32_t c, const void* b) { memdev_t* m = d->priv; if (m->fail_writes || lba + c > m->n) return -1; memcpy(m->m + lba * 512, b, (size_t)c * 512); return 0; }
static blk_dev_t mkdev(memdev_t* m) { blk_dev_t d; memset(&d, 0, sizeof d); strcpy(d.name, "mem"); d.sectors = m->n; d.read = mrd; d.write = mwr; d.priv = m; return d; }

static uint8_t* load(const char* path, uint64_t* len) {
    FILE* f = fopen(path, "rb"); CHECK(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)n); CHECK(fread(b, 1, (size_t)n, f) == (size_t)n); fclose(f);
    *len = (uint64_t)n; return b;
}
static void save(const char* path, const uint8_t* b, uint64_t n) { FILE* f = fopen(path, "wb"); CHECK(f); CHECK(fwrite(b, 1, n, f) == n); fclose(f); }
static void pattern(uint8_t* b, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t)(seed >> 16); } }

typedef struct { int n; char names[300][256]; uint32_t types[300]; uint64_t sizes[300]; } listing_t;
static int lcb(void* c, const char* name, const kfs_stat_t* st) { listing_t* l = c; if (l->n < 300) { strcpy(l->names[l->n], name); l->types[l->n] = st->type; l->sizes[l->n] = st->size; l->n++; } return 0; }
static int has(listing_t* l, const char* n, uint32_t type) { for (int i = 0; i < l->n; i++) if (!strcmp(l->names[i], n) && l->types[i] == type) return 1; return 0; }

static void check_read(fat_vol_t* v, const char* path, const uint8_t* want, size_t n) {
    kfs_stat_t st; CHECK(fat_stat(v, path, &st) == 0 && st.type == KFS_FILE && st.size == n);
    uint8_t* got = malloc(n + 10);
    CHECK(fat_read(v, path, 0, got, n + 10) == (int64_t)n && !memcmp(got, want, n));
    if (n > 5000) {                                                    /* чтение с середины, через границы кластеров */
        CHECK(fat_read(v, path, 4093, got, 3000) == 3000 && !memcmp(got, want + 4093, 3000));
        CHECK(fat_read(v, path, n - 7, got, 100) == 7 && !memcmp(got, want + n - 7, 7));
    }
    free(got);
}

static void one_fs(const char* tag, const char* mkfs_args, unsigned kb, int expect_type, int partitioned) {
    char img[256], src[256], out[256];
    snprintf(img, sizeof img, "%s/%s.img", dirt, tag);
    unlink(img);
    char kbs[16]; snprintf(kbs, sizeof kbs, "%u", kb);
    { char cmd[512]; snprintf(cmd, sizeof cmd, "%s %s -n KIRATEST -C %s %s >/dev/null", mkfs, mkfs_args, img, kbs); unlink(img); CHECK(system(cmd) == 0); }
    /* эталон кладёт файлы */
    static uint8_t big[700000], small[] = "Привет из mtools!\n";
    pattern(big, sizeof big, 7);
    snprintf(src, sizeof src, "%s/big.bin", dirt); save(src, big, sizeof big);
    snprintf(out, sizeof out, "%s/hello.txt", dirt); save(out, small, sizeof small - 1);
    setenv("MTOOLS_SKIP_CHECK", "1", 1); setenv("LC_ALL", "C.UTF-8", 1);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s/mmd -i %s ::Docs ::Docs/Глубже >/dev/null", mt, img); CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "%s/mcopy -i %s %s ::hello.txt", mt, img, out); CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "%s/mcopy -i %s %s '::Длинное имя файла с пробелами.txt'", mt, img, out); CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "%s/mcopy -i %s %s ::Docs/Глубже/big.bin", mt, img, src); CHECK(system(cmd) == 0);
    for (int i = 0; i < 40; i++) {                                       /* каталог на несколько кластеров/секторов */
        snprintf(cmd, sizeof cmd, "%s/mcopy -i %s %s ::Docs/file_number_%02d_long_name.txt", mt, img, out, i); CHECK(system(cmd) == 0);
    }
    uint64_t len; uint8_t* raw = load(img, &len);
    uint8_t* disk = raw; uint64_t dlen = len, base = 0;
    if (partitioned) {                                                   /* MBR: раздел с 2048 сектора */
        dlen = len + 2048 * 512; disk = calloc(1, dlen); memcpy(disk + 2048 * 512, raw, len);
        uint8_t* p = disk + 446; p[0] = 0x80; p[4] = expect_type == 32 ? 0x0C : 0x0E;
        uint32_t st = 2048, cn = (uint32_t)(len / 512); memcpy(p + 8, &st, 4); memcpy(p + 12, &cn, 4);
        disk[510] = 0x55; disk[511] = 0xAA;
        base = 2048;
    }
    memdev_t md = { disk, dlen / 512, 0 }; blk_dev_t dev = mkdev(&md);
    part_t parts[PART_MAX];
    CHECK(part_scan(&dev, disk, parts, PART_MAX) == 1 && parts[0].start == base);
    fat_vol_t* v = calloc(1, sizeof *v);
    CHECK(fat_mount(v, &dev, parts[0].start, parts[0].count) == 0);
    CHECK(v->type == expect_type && !strcmp(v->label, "KIRATEST"));

    /* ---- чтение того, что записал эталон ---- */
    listing_t* l = calloc(1, sizeof *l);
    CHECK(fat_list(v, "/", lcb, l) == 0);
    if (getenv("FAT_DEBUG")) for (int i = 0; i < l->n; i++) printf("  [%s] t%u s%llu\n", l->names[i], l->types[i], (unsigned long long)l->sizes[i]);
    CHECK(has(l, "hello.txt", KFS_FILE) && has(l, "Длинное имя файла с пробелами.txt", KFS_FILE) && has(l, "Docs", KFS_DIR));
    check_read(v, "/hello.txt", small, sizeof small - 1);
    check_read(v, "/HELLO.TXT", small, sizeof small - 1);                 /* регистр не важен */
    check_read(v, "/длинное ИМЯ файла с пробелами.txt", small, sizeof small - 1);
    check_read(v, "/Docs/Глубже/big.bin", big, sizeof big);
    l->n = 0; CHECK(fat_list(v, "/Docs", lcb, l) == 0 && l->n == 41 && has(l, "file_number_39_long_name.txt", KFS_FILE));
    kfs_stat_t st;
    CHECK(fat_stat(v, "/nope", &st) == KFS_ERR_NOENT && fat_stat(v, "/hello.txt/x", &st) == KFS_ERR_NOTDIR);
    uint64_t tk, fk; fat_info(v, &tk, &fk); CHECK(tk > 0 && fk < tk);

    /* ---- наша запись ---- */
    static uint8_t big2[1200000]; pattern(big2, sizeof big2, 99);
    const char* text = "Записано Кирой.\n";
    CHECK(fat_write(v, "/kira.txt", text, strlen(text)) == 0);                       /* 8.3 строчными - без LFN */
    CHECK(fat_write(v, "/Mixed Case Name.TXT", text, strlen(text)) == 0);
    CHECK(fat_write(v, "/Отчёт о работе системы.txt", text, strlen(text)) == 0);
    CHECK(fat_mkdir(v, "/Новая папка") == 0 && fat_mkdir(v, "/Новая папка") == KFS_ERR_EXIST);
    CHECK(fat_mkdir(v, "/Новая папка/вложенная") == 0);
    int64_t free_before = 0; { uint64_t a, b; fat_info(v, &a, &b); free_before = (int64_t)b; }
    if (expect_type != 12) CHECK(fat_write(v, "/Новая папка/вложенная/большой.bin", big2, sizeof big2) == 0);
    else CHECK(fat_write(v, "/Новая папка/вложенная/большой.bin", big2, 300000) == 0);
    CHECK(fat_write(v, "/hello.txt", "replaced", 8) == 0);                           /* замена: старые кластеры освобождены */
    CHECK(fat_write(v, "/empty.dat", "", 0) == 0);
    for (int i = 0; i < 70; i++) { char p[128]; snprintf(p, sizeof p, "/Новая папка/n%03d_со_длинным_именем.txt", i); CHECK(fat_write(v, p, p, strlen(p)) == 0); }
    CHECK(fat_unlink(v, "/Docs/file_number_05_long_name.txt") == 0);
    CHECK(fat_unlink(v, "/Docs") == KFS_ERR_NOTEMPTY);
    CHECK(fat_unlink(v, "/Docs/Глубже/big.bin") == 0 && fat_unlink(v, "/Docs/Глубже") == 0);
    CHECK(fat_write(v, "/a:b", "x", 1) == KFS_ERR_INVAL && fat_write(v, "/Docs", "x", 1) == KFS_ERR_ISDIR);
    CHECK(fat_write(v, "/нет/файл", "x", 1) == KFS_ERR_NOENT);
    check_read(v, "/hello.txt", (const uint8_t*)"replaced", 8);
    check_read(v, "/Отчёт о работе системы.txt", (const uint8_t*)text, strlen(text));
    check_read(v, "/Новая папка/вложенная/большой.bin", big2, expect_type != 12 ? sizeof big2 : 300000);
    l->n = 0; CHECK(fat_list(v, "/Новая папка", lcb, l) == 0 && l->n == 71);
    CHECK(fat_flush(v) == 0);
    {                                                                      /* учёт свободного места сходится с пересчётом */
        uint64_t a, b, a2, b2; fat_info(v, &a, &b);
        fat_vol_t* w = calloc(1, sizeof *w); CHECK(fat_mount(w, &dev, parts[0].start, parts[0].count) == 0);
        w->free_known = 0; fat_info(w, &a2, &b2);
        CHECK(a == a2 && b == b2 && (int64_t)b != free_before);
        free(w);
    }
    if (partitioned) memcpy(raw, disk + 2048 * 512, len);
    save(img, raw, len);

    /* ---- эталон проверяет: целостность и содержимое ---- */
    snprintf(cmd, sizeof cmd, "%s -n %s >%s/fsck.txt 2>&1", fsck, img, dirt);
    int fr = system(cmd);
    if (fr != 0) { snprintf(cmd, sizeof cmd, "cat %s/fsck.txt", dirt); system(cmd); }
    CHECK(fr == 0);
    snprintf(out, sizeof out, "%s/back.bin", dirt);
    snprintf(cmd, sizeof cmd, "%s/mcopy -n -i %s '::Новая папка/вложенная/большой.bin' %s", mt, img, out); CHECK(system(cmd) == 0);
    { uint64_t bl; uint8_t* b = load(out, &bl); CHECK(bl == (expect_type != 12 ? sizeof big2 : 300000) && !memcmp(b, big2, bl)); free(b); }
    snprintf(cmd, sizeof cmd, "%s/mcopy -n -i %s '::Отчёт о работе системы.txt' %s", mt, img, out); CHECK(system(cmd) == 0);
    { uint64_t bl; uint8_t* b = load(out, &bl); CHECK(bl == strlen(text) && !memcmp(b, text, bl)); free(b); }
    snprintf(cmd, sizeof cmd, "%s/mdir -i %s ::/ > %s/dir.txt", mt, img, dirt); CHECK(system(cmd) == 0);
    { uint64_t bl; uint8_t* b = load(strcat(strcpy(out, dirt), "/dir.txt"), &bl); b = realloc(b, bl + 1); b[bl] = 0;
      CHECK(strstr((char*)b, "kira     txt") || strstr((char*)b, "kira.txt"));
      CHECK(strstr((char*)b, "Mixed Case Name.TXT") && strstr((char*)b, "Новая папка")); free(b); }

    /* ---- сбой записи: устройство «выдернули» - ошибка, без падения ---- */
    md.fail_writes = 1;
    CHECK(fat_write(v, "/fail.txt", text, strlen(text)) == KFS_ERR_IO);
    md.fail_writes = 0;

    /* ---- порча: случайные байты в служебных областях не роняют драйвер ---- */
    srand(11);
    for (int t = 0; t < 300; t++) {
        uint8_t* cp = malloc(dlen); memcpy(cp, disk, dlen);
        if (!partitioned) memcpy(cp, raw, len);
        memdev_t m2 = { cp, dlen / 512, 0 }; blk_dev_t d2 = mkdev(&m2);
        uint64_t area = (uint64_t)(v->data_start + v->spc * 8) * 512;
        for (int k = 0; k < 40; k++) cp[base * 512 + (uint64_t)rand() % area] = (uint8_t)rand();
        fat_vol_t* w = calloc(1, sizeof *w);
        if (fat_mount(w, &d2, base, dlen / 512 - base) == 0) {
            listing_t* l2 = calloc(1, sizeof *l2);
            fat_list(w, "/", lcb, l2); fat_list(w, "/Docs", lcb, l2);
            uint8_t tmp[4096]; fat_read(w, "/Новая папка/вложенная/большой.bin", 1000, tmp, sizeof tmp);
            fat_write(w, "/x.txt", "x", 1); fat_unlink(w, "/hello.txt");
            uint64_t a, b; fat_info(w, &a, &b);
            free(l2);
        }
        free(w); free(cp);
    }
    if (expect_type == 32) {                                               /* зацикленная цепочка каталога */
        uint8_t* cp = malloc(dlen); memcpy(cp, disk, dlen); if (!partitioned) memcpy(cp, raw, len);
        uint32_t rc = v->root_clus; uint64_t fo = (base + v->rsvd) * 512 + (uint64_t)rc * 4;
        uint32_t self = rc; memcpy(cp + fo, &self, 4);
        memdev_t m2 = { cp, dlen / 512, 0 }; blk_dev_t d2 = mkdev(&m2);
        fat_vol_t* w = calloc(1, sizeof *w); CHECK(fat_mount(w, &d2, base, dlen / 512 - base) == 0);
        listing_t* l2 = calloc(1, sizeof *l2);
        int r1 = fat_list(w, "/", lcb, l2); kfs_stat_t s2; int r2 = fat_stat(w, "/нет такого", &s2);
        CHECK((r1 == 0 || r1 == KFS_ERR_CORRUPT) && (r2 == KFS_ERR_CORRUPT || r2 == KFS_ERR_NOENT));
        free(l2); free(w); free(cp);
    }
    printf("  fat%d%s: read (mkfs.fat + mcopy), write/replace/mkdir/unlink (fsck.fat clean, mtools reads back), write failure, corruption fuzz\n", expect_type, partitioned ? " in MBR partition" : "");
    free(v); free(l); if (partitioned) free(disk); free(raw);
}

static uint32_t test_crc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static void gpt_seal(uint8_t* h, const uint8_t* entries, uint32_t bytes) {
    uint32_t ec = test_crc32(entries, bytes); memcpy(h + 88, &ec, 4);
    memset(h + 16, 0, 4); uint32_t hc = test_crc32(h, 92); memcpy(h + 16, &hc, 4);
}
static void gpt_test(void) {
    static uint8_t d[512 * 200];
    memset(d, 0, sizeof d);
    uint8_t* mbr = d + 446; mbr[4] = 0xEE; uint32_t one = 1, cnt = 199;
    memcpy(mbr + 8, &one, 4); memcpy(mbr + 12, &cnt, 4); d[510] = 0x55; d[511] = 0xAA;
    uint8_t* h = d + 512; memcpy(h, "EFI PART", 8);
    uint32_t hs = 92, n = 4, esz = 128; memcpy(h + 12, &hs, 4);
    uint64_t current = 1, backup = 199, first_usable = 34, last_usable = 180, ents = 2;
    memcpy(h + 24, &current, 8); memcpy(h + 32, &backup, 8);
    memcpy(h + 40, &first_usable, 8); memcpy(h + 48, &last_usable, 8);
    memcpy(h + 72, &ents, 8); memcpy(h + 80, &n, 4); memcpy(h + 84, &esz, 4);
    static const uint8_t bd[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
    uint8_t* e = d + 1024 + 128; memcpy(e, bd, 16);
    uint64_t f = 40, l = 150; memcpy(e + 32, &f, 8); memcpy(e + 40, &l, 8);
    gpt_seal(h, d + 1024, 512);

    memdev_t m = { d, 200, 0 }; blk_dev_t dev = mkdev(&m); part_t pt[PART_MAX];
    CHECK(part_scan(&dev, d, pt, PART_MAX) == 1 && pt[0].start == 40 && pt[0].count == 111 && pt[0].gpt);

    e[32] ^= 1; CHECK(part_scan(&dev, d, pt, PART_MAX) == 0); e[32] ^= 1;
    h[20] ^= 1; CHECK(part_scan(&dev, d, pt, PART_MAX) == 0); h[20] ^= 1;

    f = 20; memcpy(e + 32, &f, 8); gpt_seal(h, d + 1024, 512);
    CHECK(part_scan(&dev, d, pt, PART_MAX) == 0);
    f = 40; memcpy(e + 32, &f, 8); gpt_seal(h, d + 1024, 512);

    uint64_t bad_lba = UINT64_MAX - 1; memcpy(h + 72, &bad_lba, 8); gpt_seal(h, d + 1024, 512);
    CHECK(part_scan(&dev, d, pt, PART_MAX) == 0);
    memcpy(h + 72, &ents, 8); gpt_seal(h, d + 1024, 512);
    CHECK(part_scan(&dev, d, pt, PART_MAX) == 1);
    printf("  partitions: GPT header + entry CRC, usable-range and LBA bounds passed\n");
}

int main(void) {
    mkfs = getenv("MKFS_FAT") ? getenv("MKFS_FAT") : "mkfs.fat";
    fsck = getenv("FSCK_FAT") ? getenv("FSCK_FAT") : "fsck.fat";
    mt = getenv("MTOOLS") ? getenv("MTOOLS") : "/usr/bin";
    CHECK(mkdtemp(dirt));
    uint8_t s[11], nt;
    CHECK(fat_short_name("readme.txt", s, &nt) == 1 && !memcmp(s, "README  TXT", 11) && nt == 0x18);
    CHECK(fat_short_name("Readme.txt", s, &nt) == 0 && fat_short_name("a.b.c", s, &nt) == 0 && fat_short_name("toolongname.txt", s, &nt) == 0);
    CHECK(fat_lfn_checksum((const uint8_t*)"README  TXT") == fat_lfn_checksum((const uint8_t*)"README  TXT"));
    gpt_test();
    one_fs("f12", "", 1440, 12, 0);
    one_fs("f16", "-F 16", 32768, 16, 1);
    one_fs("f32", "-F 32", 70000, 32, 1);
    one_fs("f32s", "-F 32 -s 1", 40000, 32, 0);
    /* ---- единое дерево: KiraFS в корне + флешка /usb0 ---- */
    {
        memdev_t km = { calloc(1, 8u << 20), (8u << 20) / 512, 0 }; blk_dev_t kd = mkdev(&km);
        CHECK(kfs_format(&kd, KFS_BASE_LBA) == 0 && kfs_write("/a.txt", "system", 6) == 0 && kfs_mkdir("/data") == 0);
        char img[256], cmd2[512]; snprintf(img, sizeof img, "%s/v.img", dirt);
        snprintf(cmd2, sizeof cmd2, "%s -F 32 -n FLASH -C %s 40000 >/dev/null", mkfs, img); CHECK(system(cmd2) == 0);
        uint64_t len; uint8_t* raw = load(img, &len);
        memdev_t fm = { raw, len / 512, 0 }; blk_dev_t fd = mkdev(&fm);
        fat_vol_t* fv = calloc(1, sizeof *fv); CHECK(fat_mount(fv, &fd, 0, len / 512) == 0);
        CHECK(vfs_mount_fat(fv, "test flash") == 0);
        listing_t* l = calloc(1, sizeof *l);
        CHECK(vfs_list("/", lcb, l) == 0 && has(l, "a.txt", KFS_FILE) && has(l, "usb0", KFS_DIR) && has(l, "data", KFS_DIR));
        CHECK(vfs_write("/usb0/Фото с телефона.txt", "flash data", 10) == 0);
        char b[32]; CHECK(vfs_read("/usb0/Фото с телефона.txt", 0, b, 32) == 10 && !memcmp(b, "flash data", 10));
        CHECK(vfs_write("/data/copy.txt", b, 10) == 0 && kfs_read("/data/copy.txt", 0, b, 32) == 10);   /* «cp» флешка -> диск */
        kfs_stat_t st; CHECK(vfs_stat("/usb0", &st) == 0 && st.type == KFS_DIR && vfs_stat("/usb1", &st) == KFS_ERR_NOFS);
        CHECK(vfs_unlink("/usb0") == KFS_ERR_INVAL);
        vfs_info_t mi[VFS_MAX]; CHECK(vfs_mounts(mi, VFS_MAX) == 1 && !strcmp(mi[0].name, "usb0") && !strcmp(mi[0].label, "FLASH") && mi[0].fat_type == 32);
        uint32_t g = vfs_generation();
        CHECK(vfs_flush(0) == 0); vfs_unmount(0);
        CHECK(vfs_generation() != g && vfs_read("/usb0/Фото с телефона.txt", 0, b, 32) == KFS_ERR_NOFS);
        l->n = 0; CHECK(vfs_list("/", lcb, l) == 0 && !has(l, "usb0", KFS_DIR));

        memset(fv, 0, sizeof *fv); CHECK(fat_mount(fv, &fd, 0, len / 512) == 0);
        CHECK(vfs_mount_fat_ro(fv, "read-only test") == 0);
        CHECK(vfs_mounts(mi, VFS_MAX) == 1 && mi[0].read_only);
        uint8_t* before = malloc(len); CHECK(before); memcpy(before, raw, len);
        CHECK(vfs_read("/usb0/Фото с телефона.txt", 0, b, 32) == 10 && !memcmp(b, "flash data", 10));
        CHECK(vfs_write("/usb0/no-write.txt", "x", 1) == KFS_ERR_RO);
        CHECK(vfs_mkdir("/usb0/no-dir") == KFS_ERR_RO);
        CHECK(vfs_unlink("/usb0/Фото с телефона.txt") == KFS_ERR_RO);
        CHECK(vfs_flush(0) == 0 && !memcmp(before, raw, len));
        vfs_unmount(0); free(before);
        save(img, raw, len);
        snprintf(cmd2, sizeof cmd2, "%s -n %s >/dev/null 2>&1", fsck, img); CHECK(system(cmd2) == 0);
        free(l); free(fv); free(raw); free(km.m);
        printf("  vfs: KiraFS root, writable test volume, USB read-only policy, unmount, fsck clean\n");
    }
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dirt); system(cmd);
    printf("fat: FAT12/16/32 cross-checked with dosfstools and mtools passed\n");
    return 0;
}
