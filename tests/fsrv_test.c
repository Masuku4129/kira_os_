// Песочницы программ: имена, права, квота, размеры, изоляция между программами (на KiraFS в памяти).
#include "fsrv.h"
#include "caps.h"
#include "kfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

#define SECT (16 + 4096)
static uint8_t disk[SECT * 512];
static int rd(blk_dev_t* d, uint64_t l, uint32_t n, void* b) { (void)d; if (l + n > SECT) return -1; memcpy(b, disk + l * 512, n * 512); return 0; }
static int wr(blk_dev_t* d, uint64_t l, uint32_t n, const void* b) { (void)d; if (l + n > SECT) return -1; memcpy(disk + l * 512, b, n * 512); return 0; }
static int fl(blk_dev_t* d) { (void)d; return 0; }
static blk_dev_t dev = { "ram", SECT, rd, wr, fl, 0 };

static fsrv_req_t R;
static uint8_t Rbuf[9u << 20];
static int64_t call(const char* prog, int op, const char* name, const void* data, uint64_t len, uint64_t off) {
    memset(&R, 0, sizeof R); R.data = Rbuf;
    if (op == FS_OP_LIST && !len) len = 65536;
    strncpy(R.prog, prog, sizeof R.prog - 1); R.op = (uint32_t)op;
    if (name) strncpy(R.name, name, sizeof R.name - 1);
    R.off = off; R.len = len;
    if (data && len && len <= sizeof Rbuf) memcpy(R.data, data, len);
    fsrv_execute(&R);
    return R.result;
}

int main(void) {
    CHECK(fsrv_prog_ok("paint") && fsrv_prog_ok("atk-mem") && fsrv_prog_ok("a_1") && !fsrv_prog_ok("") && !fsrv_prog_ok("Paint") && !fsrv_prog_ok("a/b") && !fsrv_prog_ok("0123456789abcdef") && !fsrv_prog_ok(".."));
    CHECK(fsrv_name_ok("picture.kpnt") && fsrv_name_ok("a") && fsrv_name_ok("A-b_c.1") && !fsrv_name_ok("") && !fsrv_name_ok(".hidden") && !fsrv_name_ok("..") && !fsrv_name_ok(".") && !fsrv_name_ok("a/b") && !fsrv_name_ok("/etc") && !fsrv_name_ok("a b") && !fsrv_name_ok("a\n") && !fsrv_name_ok("-x"));
    char longn[64]; memset(longn, 'a', 41); longn[41] = 0; CHECK(!fsrv_name_ok(longn)); longn[40] = 0; CHECK(fsrv_name_ok(longn));

    CHECK(call("paint", FS_OP_READ, "x", 0, 10, 0) == KFS_ERR_NOFS);                   /* без ФС */
    CHECK(kfs_format(&dev, 16) == 0);
    kfs_mkdir("/bin"); kfs_write("/bin/secret", "TOP", 3);

    /* запись, чтение, размер, смещение */
    uint8_t buf[70000]; for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7);
    CHECK(call("paint", FS_OP_WRITE, "pic", buf, 1000, 0) == 0);
    CHECK(call("paint", FS_OP_STAT, "pic", 0, 0, 0) == 1000);
    CHECK(call("paint", FS_OP_READ, "pic", 0, 4096, 0) == 1000 && memcmp(R.data, buf, 1000) == 0);
    CHECK(call("paint", FS_OP_READ, "pic", 0, 10, 995) == 5 && memcmp(R.data, buf + 995, 5) == 0);
    CHECK(call("paint", FS_OP_READ, "pic", 0, 10, 1000) == 0);
    CHECK(call("paint", FS_OP_READ, "pic", 0, 0, 0) == FS_E_ARG && call("paint", FS_OP_READ, "pic", 0, fsrv_max_data() + 1, 0) == FS_E_ARG);
    CHECK(call("paint", FS_OP_WRITE, "pic", Rbuf, fsrv_max_data() + 1, 0) == FS_E_ARG);
    CHECK(call("paint", FS_OP_WRITE, "pic", buf, 600, 0) == 0 && call("paint", FS_OP_STAT, "pic", 0, 0, 0) == 600);   /* атомарная замена меньшим */
    CHECK(call("paint", FS_OP_WRITE, "big", buf, sizeof buf, 0) == 0 && call("paint", FS_OP_STAT, "big", 0, 0, 0) == (int64_t)sizeof buf);   /* больше прежних 64 КБ */
    CHECK(call("paint", FS_OP_WRITE, "empty", buf, 0, 0) == 0 && call("paint", FS_OP_STAT, "empty", 0, 0, 0) == 0);
    CHECK(call("paint", FS_OP_READ, "nothere", 0, 5, 0) == KFS_ERR_NOENT && call("paint", FS_OP_STAT, "nothere", 0, 0, 0) == KFS_ERR_NOENT);

    /* изоляция: чужая песочница, системные файлы и выход вверх недостижимы по построению */
    CHECK(call("plasma", FS_OP_STAT, "pic", 0, 0, 0) == KFS_ERR_NOENT);                                  /* у plasma своя пустая песочница */
    CHECK(call("plasma", FS_OP_WRITE, "pic", "mine", 4, 0) == 0);
    CHECK(call("paint", FS_OP_STAT, "pic", 0, 0, 0) == 600 && call("plasma", FS_OP_STAT, "pic", 0, 0, 0) == 4);
    const char* evil[] = { "../paint/pic", "/bin/secret", "..", ".", "a/b", "/", "../../bin/secret", ".x", "", "pic/../../bin/secret" };
    for (unsigned i = 0; i < sizeof evil / sizeof *evil; i++) {
        CHECK(call("plasma", FS_OP_READ, evil[i], 0, 10, 0) == FS_E_DENIED);
        CHECK(call("plasma", FS_OP_WRITE, evil[i], "x", 1, 0) == FS_E_DENIED);
        CHECK(call("plasma", FS_OP_REMOVE, evil[i], 0, 0, 0) == FS_E_DENIED);
    }
    CHECK(call("../bin", FS_OP_STAT, "secret", 0, 0, 0) == FS_E_DENIED && call("", FS_OP_LIST, "", 0, 100, 0) == FS_E_DENIED);   /* имя программы тоже проверяется */
    CHECK(kfs_read("/bin/secret", 0, buf, 3) == 3);                                                      /* системный файл не тронут */

    /* список */
    CHECK(call("paint", FS_OP_LIST, "", 0, 0, 0) > 0);
    CHECK(strstr((char*)R.data, "pic\n") && strstr((char*)R.data, "big\n") && strstr((char*)R.data, "empty\n"));
    CHECK(call("plasma", FS_OP_LIST, "", 0, 0, 0) == 4 && memcmp(R.data, "pic\n", 4) == 0);
    CHECK(call("nobody", FS_OP_LIST, "", 0, 0, 0) == 0);                                                  /* песочница создаётся пустой */

    /* удаление */
    CHECK(call("paint", FS_OP_REMOVE, "pic", 0, 0, 0) == 0 && call("paint", FS_OP_STAT, "pic", 0, 0, 0) == KFS_ERR_NOENT);
    CHECK(call("paint", FS_OP_REMOVE, "pic", 0, 0, 0) == KFS_ERR_NOENT);
    CHECK(call("paint", FS_OP_STAT, "..", 0, 0, 0) == FS_E_DENIED);

    /* квота */
    for (int i = 0; i < (int)fsrv_max_files() + 2; i++) { char n[16]; snprintf(n, sizeof n, "f%d", i); (void)call("quota", FS_OP_WRITE, n, "x", 1, 0); }
    CHECK(call("quota", FS_OP_WRITE, "extra", "x", 1, 0) == FS_E_QUOTA);
    CHECK(call("quota", FS_OP_WRITE, "f0", "yy", 2, 0) == 0 && call("quota", FS_OP_STAT, "f0", 0, 0, 0) == 2);   /* перезапись существующего квотой не блокируется */
    CHECK(call("quota", FS_OP_REMOVE, "f1", 0, 0, 0) == 0 && call("quota", FS_OP_WRITE, "extra", "x", 1, 0) == 0);

    /* неверная операция */
    CHECK(call("paint", 99, "x", 0, 0, 0) == FS_E_ARG && call("paint", 0, "x", 0, 0, 0) == FS_E_ARG);

    /* данные переживают перемонтирование */
    CHECK(kfs_mount(&dev, 16) == 0 && call("plasma", FS_OP_READ, "pic", 0, 10, 0) == 4 && memcmp(R.data, "mine", 4) == 0);

    /* имя процесса для собранных пользователем программ (runlocal) */
    char ln[16];
    CHECK(fsrv_local_name("hello", ln) && !strcmp(ln, "local-hello"));
    CHECK(fsrv_local_name("Hello.elf", ln) && !strcmp(ln, "local-hello"));
    CHECK(fsrv_local_name("a-b_9", ln) && !strcmp(ln, "local-a-b_9"));
    CHECK(fsrv_local_name("123456789", ln) && !strcmp(ln, "local-123456789"));
    CHECK(!fsrv_local_name("1234567890", ln));                    /* больше 15 знаков вместе с приставкой */
    CHECK(!fsrv_local_name("", ln) && !fsrv_local_name(".hid", ln) && !fsrv_local_name("a/b", ln) && !fsrv_local_name("a b", ln));
    CHECK(caps_for_program("kcc") == CAP_FS);                     /* компилятору - файлы и больше ничего */
    CHECK(caps_for_program("local-hello") == 0 && caps_for_program("local-") == 0 && caps_for_program("local-plasma") == 0);
    CHECK(caps_for_program("nonexistent") == 0);

    printf("fsrv: sandbox names/paths, isolation between programs, size and quota limits, atomic replace, persistence passed\n");
    return 0;
}
