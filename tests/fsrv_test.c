// Песочницы программ: имена, права, квота, размеры, изоляция между программами (на KiraFS в памяти).
#include "fsrv.h"
#include "caps.h"
#include "kfs.h"
#include "kdrv_format.h"
#include "kdrv_sig.h"
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
    if ((op == FS_OP_LIST || op == FS_OP_DRV_LIST) && !len) len = 65536;
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

    /* пакеты драйверов в /drv (FS_OP_DRV_*): только drvget, проверка заголовка и хешей ядром */
    {
        void sha256(const void*, size_t, uint8_t*);
        uint8_t pk[sizeof(kdrv_header_t) + 600]; kdrv_header_t h; memset(&h, 0, sizeof h);
        h.magic = KDRV_MAGIC; h.format = KDRV_FORMAT; h.hdr_size = sizeof h; strcpy(h.name, "demo"); h.version = 3;
        h.caps = CAP_MMIO | CAP_DMA; h.elf_size = 600; h.nmatch = 1; h.match[0].vendor = 0x10ec; h.match[0].device = 0x8139;
        memcpy(pk, &h, sizeof h); for (int i = 0; i < 600; i++) pk[sizeof h + i] = (uint8_t)(i * 5 + 1);
        kdrv_rec_t rec; memset(&rec, 0, sizeof rec);
        rec.magic = KREC_MAGIC; rec.format = KDRV_FORMAT; rec.rec_size = sizeof rec; rec.confirmed = KREC_CONFIRMED; rec.caps_ok = CAP_MMIO;
        sha256(pk + sizeof h, 600, rec.elf_sha); sha256(pk, sizeof pk, rec.pkg_sha); strcpy(rec.source, "https://example.test/demo");
        CHECK(call("paint", FS_OP_DRV_WRITE, "demo.kdrv", pk, sizeof pk, 0) == FS_E_DENIED);          /* не drvget */
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.rec", &rec, sizeof rec, 0) == FS_E_DENIED);       /* подтверждение без пакета */
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.kdrv", pk, sizeof pk, 0) == 0);
        CHECK(call("drvget", FS_OP_DRV_STAT, "demo.kdrv", 0, 0, 0) == (int64_t)sizeof pk);
        CHECK(call("drvget", FS_OP_DRV_READ, "demo.kdrv", 0, sizeof pk, 0) == (int64_t)sizeof pk && !memcmp(R.data, pk, sizeof pk));
        kdrv_rec_t bad = rec; bad.elf_sha[0] ^= 1; CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.rec", &bad, sizeof bad, 0) == FS_E_DENIED);   /* хеш не тот */
        bad = rec; bad.pkg_sha[5] ^= 1; CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.rec", &bad, sizeof bad, 0) == FS_E_DENIED);
        bad = rec; bad.confirmed = 0; CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.rec", &bad, sizeof bad, 0) == FS_E_ARG);                    /* не подтверждено */
        bad = rec; bad.caps_ok = CAP_NETDEV; CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.rec", &bad, sizeof bad, 0) == FS_E_DENIED);          /* подтверждено больше, чем просит пакет */
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.rec", &rec, sizeof rec, 0) == 0);
        kdrv_sig_t sig; memset(&sig, 0, sizeof sig);
        sig.magic = KDRV_SIG_MAGIC; sig.format = KDRV_SIG_FORMAT; sig.size = sizeof sig;
        memcpy(sig.key_id, "TEST-ED25519-001", 16); memcpy(sig.package_sha256, rec.pkg_sha, 32);
        sig.package_size = sizeof pk; sig.not_before = 1; sig.not_after = 4102444800ULL;
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.sig", &sig, sizeof sig, 0) == 0); /* формат/хеш привязаны; Ed25519 проверяет ядро */
        CHECK(call("drvget", FS_OP_DRV_STAT, "demo.sig", 0, 0, 0) == (int64_t)sizeof sig);
        CHECK(call("drvget", FS_OP_DRV_READ, "demo.sig", 0, sizeof sig, 0) == (int64_t)sizeof sig && !memcmp(R.data, &sig, sizeof sig));
        kdrv_sig_t badsig = sig; badsig.package_sha256[0] ^= 1;
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.sig", &badsig, sizeof badsig, 0) == FS_E_DENIED);
        int64_t ll = call("drvget", FS_OP_DRV_LIST, 0, 0, 0, 0);
        CHECK(ll > 0 && ll < 4096); R.data[ll] = 0;
        CHECK(strstr((char*)R.data, "demo.kdrv") && strstr((char*)R.data, "demo.rec"));
        uint8_t pk2[sizeof pk]; memcpy(pk2, pk, sizeof pk); memcpy(pk2, &h, sizeof h); strcpy(((kdrv_header_t*)pk2)->name, "other");
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.kdrv", pk2, sizeof pk2, 0) == FS_E_ARG);         /* имя файла и имя в заголовке разные */
        CHECK(call("drvget", FS_OP_DRV_WRITE, "demo.kdrv", pk, sizeof pk - 1, 0) == FS_E_ARG);       /* размер не по заголовку */
        const char* evil[] = { "../x.kdrv", "demo", "DEMO.kdrv", "a/b.kdrv", ".kdrv", "demo.elf", "waytoolongname1.kdrv", "demo.kdrv.rec" };
        for (unsigned i = 0; i < sizeof evil / sizeof *evil; i++) CHECK(call("drvget", FS_OP_DRV_STAT, evil[i], 0, 0, 0) == FS_E_DENIED);
        /* окно «Драйверы» (drvman): те же права на /drv, что у drvget; остальные программы - отказ. START/STOP только для .kdrv установленного пакета */
        CHECK(call("drvman", FS_OP_DRV_STAT, "demo.kdrv", 0, 0, 0) == (int64_t)sizeof pk && call("drvman", FS_OP_DRV_LIST, 0, 0, 0, 0) > 0);
        CHECK(call("paint", FS_OP_DRV_START, "demo.kdrv", 0, 0, 0) == FS_E_DENIED && call("paint", FS_OP_DRV_STOP, "demo.kdrv", 0, 0, 0) == FS_E_DENIED);
        CHECK(call("drvman", FS_OP_DRV_START, "demo.kdrv", 0, 0, 0) == 0 && call("drvget", FS_OP_DRV_STOP, "demo.kdrv", 0, 0, 0) == 0);
        CHECK(call("drvman", FS_OP_DRV_START, "nope.kdrv", 0, 0, 0) == KFS_ERR_NOENT);              /* запускать можно только установленный пакет */
        CHECK(call("drvman", FS_OP_DRV_START, "demo.rec", 0, 0, 0) == FS_E_ARG && call("drvman", FS_OP_DRV_STOP, "../demo.kdrv", 0, 0, 0) == FS_E_DENIED);
        CHECK(call("drvman", FS_OP_DRV_WRITE, "demo2.kdrv", pk, sizeof pk, 0) == FS_E_ARG);          /* имя в заголовке demo, файл demo2 */
        CHECK(call("drvget", FS_OP_DRV_REMOVE, "demo.kdrv", 0, 0, 0) == 0);                          /* пакет уходит вместе с подтверждением */
        CHECK(call("drvget", FS_OP_DRV_STAT, "demo.kdrv", 0, 0, 0) == KFS_ERR_NOENT && call("drvget", FS_OP_DRV_STAT, "demo.rec", 0, 0, 0) == KFS_ERR_NOENT && call("drvget", FS_OP_DRV_STAT, "demo.sig", 0, 0, 0) == KFS_ERR_NOENT);
        CHECK(call("drvget", FS_OP_STAT, "demo.kdrv", 0, 0, 0) == KFS_ERR_NOENT);                    /* обычная песочница drvget - отдельно от /drv */
        char st[16]; int kind;
        CHECK(fsrv_drv_name("rl.kdrv", st, &kind) && kind == 0 && !strcmp(st, "rl") && fsrv_drv_name("a-b_9.rec", st, &kind) && kind == 1 && fsrv_drv_name("a-b_9.sig", st, &kind) && kind == 2);
        CHECK(caps_for_program("drvget") == (CAP_NET | CAP_FS | CAP_SYSFILES));
        /* право CAP_PCILIST (список PCI только для чтения) есть у одного drvman; нет ни у драйверов, ни у остальных программ */
        CHECK(caps_for_program("drvman") == (CAP_GFX | CAP_NET | CAP_FS | CAP_SYSFILES | CAP_THREAD | CAP_PCILIST));
        CHECK(!(caps_for_program("drvman") & CAP_DRIVER_));
        const char* others[] = { "plasma", "cube3d", "paint", "edit", "view", "wasm", "fetch", "serve", "udpecho", "net6", "melody", "rec", "ipc", "mt", "kcc", "drvget", "burn", "fpu", "demo", "atk-cap", "drv-edu", "local-x" };
        for (unsigned i = 0; i < sizeof others / sizeof *others; i++) CHECK(!(caps_for_program(others[i]) & CAP_PCILIST));
        CHECK(!strcmp(caps_bit_name(CAP_PCILIST), "CAP_PCILIST") && (CAP_PCILIST & CAP_ALL_) && !(CAP_PCILIST & CAP_DRIVER_));
    }

    printf("fsrv: sandbox names/paths, isolation between programs, size and quota limits, atomic replace, persistence passed\n");
    return 0;
}
