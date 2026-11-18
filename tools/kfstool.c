/* kfstool - инструмент сборки на хосте: кладёт файлы в образ диска с KiraFS (тот же kfs.c, что и в ядре) и составляет
   манифест доверенных программ. Образ должен быть выключен (не подключён к QEMU).
     kfstool образ init                 создать KiraFS, если её нет (существующая не трогается)
     kfstool образ put /путь файл       записать файл (недостающие каталоги создаются)
     kfstool образ ls [/путь]           список
     kfstool образ cat /путь            содержимое в stdout
     kfstool manifest файл...           manifest.h для ядра: имя (без каталога и .elf) и SHA-256 каждого файла
     kfstool образ putz /путь файл.zst  записать сжатый кадр Zstd: Кира видит файл распакованным, в блоках лежит кадр (до 8 МБ)
     kfstool образ seal                 печать системы: /etc/system.cbor со SHA-256 всех файлов /bin и /etc и корнем Меркла (sysseal.h) */
#include "kfs.h"
#include "immune.h"
#include "sysseal.h"
#include "appman.c"     /* формат манифеста приложений подключается исходником: список файлов в десятке скриптов сборки kfstool не меняется */
#include "zstd.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int img_fd;
static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b) { (void)d; return pread(img_fd, b, (size_t)n * 512, (off_t)(lba * 512)) == (ssize_t)(n * 512) ? 0 : -1; }
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b) { (void)d; return pwrite(img_fd, b, (size_t)n * 512, (off_t)(lba * 512)) == (ssize_t)(n * 512) ? 0 : -1; }
static int fl(blk_dev_t* d) { (void)d; return fsync(img_fd); }
static blk_dev_t dev;

static void die(const char* what, int e) { fprintf(stderr, "kfstool: %s: %s\n", what, kfs_strerror(e)); exit(1); }

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { perror(path); exit(1); }
    uint8_t* b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { perror(path); exit(1); }
    fclose(f); *n = (size_t)sz;
    return b;
}

static int open_image(const char* path, int create_fs) {
    img_fd = open(path, O_RDWR);
    if (img_fd < 0) { perror(path); exit(1); }
    off_t size = lseek(img_fd, 0, SEEK_END);
    strcpy(dev.name, "image"); dev.sectors = (uint64_t)size / 512; dev.read = rd; dev.write = wr; dev.flush = fl;
    int r = kfs_mount(&dev, KFS_BASE_LBA);
    if (r == KFS_ERR_NOFS && create_fs) { r = kfs_format(&dev, KFS_BASE_LBA); if (r == 0) printf("kfstool: создана KiraFS на %s\n", path); }
    if (r) die(path, r);
    return 0;
}

static void mkdirs(const char* path) {
    char tmp[256]; size_t n = strlen(path);
    if (n >= sizeof tmp) { fprintf(stderr, "kfstool: слишком длинный путь\n"); exit(1); }
    strcpy(tmp, path);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        kfs_stat_t st;
        if (kfs_stat(tmp, &st) != 0) { int r = kfs_mkdir(tmp); if (r) die(tmp, r); }
        tmp[i] = '/';
    }
}

/* Печать: список файлов собирается отдельно от чтения (внутри kfs_list другие kfs_* звать нельзя). */
static seal_entry seal[SEAL_MAX_FILES]; static size_t nseal; static char seal_dir[SEAL_NAME_MAX];
static char seal_dirs[256][SEAL_NAME_MAX]; static size_t nseal_dirs;
static int seal_cb(void* c, const char* name, const kfs_stat_t* st) {
    (void)c;
    char path[SEAL_NAME_MAX];
    if (snprintf(path, SEAL_NAME_MAX, "%s/%s", seal_dir, name) >= SEAL_NAME_MAX) { fprintf(stderr, "kfstool: длинный путь %s\n", name); exit(1); }
    if (st->type == KFS_DIR) {
        if (nseal_dirs >= 256) { fprintf(stderr, "kfstool: каталогов больше 256\n"); exit(1); }
        strcpy(seal_dirs[nseal_dirs++], path);
        return 0;
    }
    if (nseal >= SEAL_MAX_FILES) { fprintf(stderr, "kfstool: файлов больше %d\n", SEAL_MAX_FILES); exit(1); }
    strcpy(seal[nseal].path, path);
    seal[nseal++].size = st->size;
    return 0;
}
static int seal_cmp(const void* a, const void* b) { return strcmp(((const seal_entry*)a)->path, ((const seal_entry*)b)->path); }
static int do_seal(void) {
    strcpy(seal_dirs[0], "/bin"); strcpy(seal_dirs[1], "/etc"); nseal_dirs = 2;
    for (size_t d = 0; d < nseal_dirs; d++) {              /* подкаталоги добавляются в конец списка по ходу обхода */
        strcpy(seal_dir, seal_dirs[d]);
        int r = kfs_list(seal_dirs[d], seal_cb, 0);
        if (r && r != KFS_ERR_NOENT) die(seal_dirs[d], r);
    }
    for (size_t i = 0; i < nseal; i++) if (!strcmp(seal[i].path, SEAL_PATH)) { seal[i] = seal[--nseal]; break; }   /* сама печать в неё не входит */
    qsort(seal, nseal, sizeof seal[0], seal_cmp);
    for (size_t i = 0; i < nseal; i++) {
        uint8_t* b = malloc((size_t)seal[i].size + 1);
        int64_t got = kfs_read(seal[i].path, 0, b, (size_t)seal[i].size);
        if (got != (int64_t)seal[i].size) die(seal[i].path, got < 0 ? (int)got : KFS_ERR_IO);
        sha256(b, (size_t)got, seal[i].hash);
        free(b);
    }
    static uint8_t m[SEAL_MAX_FILES * (SEAL_NAME_MAX + 48) + 64];
    size_t n = seal_build(seal, nseal, m, sizeof m);
    if (!n) { fprintf(stderr, "kfstool: печать не собралась\n"); return 1; }
    int r = kfs_write(SEAL_PATH, m, n);
    if (r < 0) die(SEAL_PATH, r);
    printf("kfstool: печать системы: %zu файлов, %zu байт -> %s\n", nseal, n, SEAL_PATH);
    return 0;
}

static int ls_cb(void* c, const char* name, const kfs_stat_t* st) {
    (void)c; printf("%s %10llu %s\n", st->type == KFS_DIR ? "d" : "-", (unsigned long long)st->size, name); return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "manifest")) {
        printf("/* Создано tools/kfstool manifest. Не редактировать: эталон доверенных программ для иммунной системы. */\n");
        printf("#include \"trust.h\"\n");
        printf("static const trust_entry_t PROG_MANIFEST[] = {\n");
        for (int i = 2; i < argc; i++) {
            size_t n; uint8_t* b = slurp(argv[i], &n);
            uint8_t d[32]; sha256(b, n, d);
            const char* base = strrchr(argv[i], '/'); base = base ? base + 1 : argv[i];
            char name[64]; snprintf(name, sizeof name, "%s", base);
            char* dot = strrchr(name, '.'); if (dot && !strcmp(dot, ".elf")) *dot = 0;
            printf("    { \"%s\", {", name);
            for (int k = 0; k < 32; k++) printf("%s0x%02x", k ? "," : "", d[k]);
            printf("} },\n");
            free(b);
        }
        printf("};\n#define PROG_MANIFEST_COUNT %d\n", argc - 2);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "appman")) {      /* kfstool appman СЕРИЙНЫЙ_НОМЕР ВЫХОД.manifest ELF...: бинарный манифест приложений (appman.h) под подпись издателя */
        unsigned n = (unsigned)(argc - 4);
        if (n > APPMAN_MAX) { fprintf(stderr, "kfstool: слишком много программ для манифеста (%u > %u)\n", n, APPMAN_MAX); return 1; }
        trust_entry_t* e = calloc(n, sizeof *e);
        for (unsigned i = 0; i < n; i++) {
            size_t sz; uint8_t* b = slurp(argv[4 + i], &sz);
            const char* base = strrchr(argv[4 + i], '/'); base = base ? base + 1 : argv[4 + i];
            char* name = calloc(1, strlen(base) + 1); strcpy(name, base);
            char* dot = strrchr(name, '.'); if (dot && !strcmp(dot, ".elf")) *dot = 0;
            e[i].name = name;
            sha256(b, sz, (uint8_t*)e[i].sha);
            free(b);
        }
        static uint8_t out[APPMAN_MAX_LEN];
        size_t len = appman_build((uint32_t)strtoul(argv[2], 0, 10), e, n, out, sizeof out);
        if (!len) { fprintf(stderr, "kfstool: манифест приложений не собрался (имя длиннее 23 символов или повтор)\n"); return 1; }
        FILE* f = fopen(argv[3], "wb");
        if (!f || fwrite(out, 1, len, f) != len || fclose(f)) { perror(argv[3]); return 1; }
        printf("kfstool: манифест приложений: %u программ, %zu байт -> %s (подписать: tools/kdrvsign.py --domain apps)\n", n, len, argv[3]);
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "использование: kfstool образ init|put|ls|cat ...  |  kfstool manifest файл...  |  kfstool appman N ВЫХОД ELF...\n"); return 2; }
    const char* cmd = argv[2];
    if (!strcmp(cmd, "init")) { open_image(argv[1], 1); printf("kfstool: KiraFS готова\n"); return 0; }
    open_image(argv[1], 0);
    if (!strcmp(cmd, "put") && argc == 5) {
        size_t n; uint8_t* b = slurp(argv[4], &n);
        mkdirs(argv[3]);
        int r = kfs_write(argv[3], b, n);
        if (r) die(argv[3], r);
        printf("kfstool: %s <- %s (%zu байт)\n", argv[3], argv[4], n);
        return 0;
    }
    if (!strcmp(cmd, "putz") && argc == 5) {
        size_t n; uint8_t* b = slurp(argv[4], &n);
        int64_t us = zstd_frame_content_size(b, n);
        if (us <= 0) { fprintf(stderr, "kfstool: %s: в кадре Zstd нет размера содержимого (сожмите zstd/python-zstandard с записью размера)\n", argv[4]); return 1; }
        mkdirs(argv[3]);
        int r = kfs_write_packed(argv[3], b, n, (uint64_t)us);
        if (r) die(argv[3], r);
        printf("kfstool: %s <- %s (сжато %zu байт, содержимое %lld)\n", argv[3], argv[4], n, (long long)us);
        return 0;
    }
    if (!strcmp(cmd, "seal")) { open_image(argv[1], 0); return do_seal(); }
    if (!strcmp(cmd, "ls")) { int r = kfs_list(argc > 3 ? argv[3] : "/", ls_cb, 0); if (r) die("ls", r); return 0; }
    if (!strcmp(cmd, "cat") && argc == 4) {
        kfs_stat_t st; int r = kfs_stat(argv[3], &st); if (r) die(argv[3], r);
        uint8_t* b = malloc((size_t)st.size + 1);
        int64_t got = kfs_read(argv[3], 0, b, (size_t)st.size);
        if (got < 0) die(argv[3], (int)got);
        fwrite(b, 1, (size_t)got, stdout);
        return 0;
    }
    if (!strcmp(cmd, "rm") && argc == 4) { int r = kfs_unlink(argv[3]); if (r && r != KFS_ERR_NOENT) die(argv[3], r); return 0; }   /* нет файла - не ошибка */
    fprintf(stderr, "kfstool: неизвестная команда\n");
    return 2;
}
