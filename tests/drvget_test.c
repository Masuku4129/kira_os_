// drvget_core: адреса (только https), рецепт, проверка исходников (лицензия, GPL, бинарники), SHA-256 набора, подтверждение по началу хеша.
#include "../userland/drvget_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static int scan(const char* txt, char* lic, const char** err) { return dg_scan_source("x.c", (const uint8_t*)txt, (uint32_t)strlen(txt), lic, err); }
static int rec(const char* txt, dg_recipe_t* r, const char** err) { return dg_parse_recipe(txt, (uint32_t)strlen(txt), r, err); }

int main(void) {
    const char* err = "";
    dg_url_t u;
    CHECK(dg_url_parse("https://example.com/a/b.c", &u, &err) == 0 && !strcmp(u.host, "example.com") && u.port == 443 && !strcmp(u.path, "/a/b.c"));
    CHECK(dg_url_parse("https://10.0.2.100:8443/x", &u, &err) == 0 && u.port == 8443 && !strcmp(u.host, "10.0.2.100"));
    CHECK(dg_url_parse("https://h", &u, &err) == 0 && !strcmp(u.path, "/"));
    CHECK(dg_url_parse("http://example.com/a", &u, &err) != 0 && strstr(err, "https"));
    CHECK(dg_url_parse("ftp://example.com/a", &u, &err) != 0 && dg_url_parse("file:///etc/passwd", &u, &err) != 0 && dg_url_parse("example.com/a", &u, &err) != 0);
    CHECK(dg_url_parse("https://ex ample.com/", &u, &err) != 0 && dg_url_parse("https://h:99999/", &u, &err) != 0 && dg_url_parse("https://h:/", &u, &err) != 0);
    CHECK(dg_url_parse("https://h/a b", &u, &err) != 0 && dg_url_parse("https://h/a\\b", &u, &err) != 0);
    char o[300];
    dg_url_parse("https://h.example/drv/rl/drvget.rec", &u, &err);
    CHECK(dg_url_join(&u, "rl.c", o, sizeof o, &err) == 0 && !strcmp(o, "https://h.example/drv/rl/rl.c"));
    CHECK(dg_url_join(&u, "/x/y.h", o, sizeof o, &err) == 0 && !strcmp(o, "https://h.example/x/y.h"));
    CHECK(dg_url_join(&u, "https://other.example/q.c", o, sizeof o, &err) == 0 && !strcmp(o, "https://other.example/q.c"));
    CHECK(dg_url_join(&u, "../etc.c", o, sizeof o, &err) != 0);
    dg_url_parse("https://h:8443/a/b.rec", &u, &err);
    CHECK(dg_url_join(&u, "c.c", o, sizeof o, &err) == 0 && !strcmp(o, "https://h:8443/a/c.c"));

    CHECK(dg_license_ok("BSD-2-Clause") && dg_license_ok("BSD-3-Clause") && dg_license_ok("MIT") && dg_license_ok("ISC") && dg_license_ok("LicenseRef-Greenman"));
    CHECK(dg_license_matches("BSD-2-Clause", "BSD-2-Clause") && !dg_license_matches("BSD-3-Clause", "BSD-2-Clause") && !dg_license_matches(0, "MIT"));
    CHECK(!dg_license_ok("GPL-2.0") && !dg_license_ok("GPL-2.0-or-later") && !dg_license_ok("LGPL-2.1") && !dg_license_ok("BSD-4-Clause") && !dg_license_ok("") && !dg_license_ok("mit"));
    CHECK(dg_file_name_ok("rl.c") && dg_file_name_ok("if_rlreg.h") && !dg_file_name_ok("net.sys") && !dg_file_name_ok("rtl8139.inf") && !dg_file_name_ok("a.ko")
          && !dg_file_name_ok("x.exe") && !dg_file_name_ok("../a.c") && !dg_file_name_ok(".c") && !dg_file_name_ok("a/b.c") && !dg_file_name_ok("a b.c")
          && !dg_file_name_ok("averyveryverylongfilename_toolong.c"));

    /* рецепт */
    const char* good = "# комментарий\ndrvget-recipe 1\nname rl\nversion 2\nlicense BSD-3-Clause\npci 10ec:8139\nclass 020000/ff0000\ncaps mmio dma pcidev netdev\nmain rl.c\n"
        "file rl.c rl.c\nfile h.h https://x.test/h.h sha256 0000000000000000000000000000000000000000000000000000000000000001\n";
    dg_recipe_t r;
    CHECK(rec(good, &r, &err) == 0);
    CHECK(!strcmp(r.name, "rl") && r.version == 2 && !strcmp(r.license, "BSD-3-Clause") && r.nmatch == 2 && r.match[0].vendor == 0x10ec && r.match[0].device == 0x8139);
    CHECK(r.match[1].vendor == KDRV_ANY && r.match[1].cls == 0x020000 && r.match[1].cls_mask == 0xff0000);
    CHECK(r.caps == (CAP_MMIO | CAP_DMA | CAP_PCIDEV | CAP_NETDEV) && r.nfile == 2 && !strcmp(r.main_file, "rl.c") && !strcmp(r.file[1].url, "https://x.test/h.h") && strlen(r.file[1].sha) == 64);
    const char* base = "drvget-recipe 1\nname rl\nversion 1\nlicense MIT\npci 10ec:8139\ncaps mmio\nmain a.c\nfile a.c a.c\n";
    CHECK(rec(base, &r, &err) == 0);
    const char* greenman_recipe = "drvget-recipe 1\nname gm\nversion 1\nlicense LicenseRef-Greenman\npci 10ec:8139\ncaps mmio\nmain a.c\nfile a.c a.c\n";
    CHECK(rec(greenman_recipe, &r, &err) == 0 && !strcmp(r.license, "LicenseRef-Greenman"));
    struct { const char* from; const char* to; } mut[] = {
        { "license MIT", "license GPL-2.0" }, { "license MIT", "license Proprietary" }, { "name rl", "name RL" }, { "name rl", "name toolongnamexyz" }, { "caps mmio", "caps mmio fs" },
        { "caps mmio", "caps gfx" }, { "pci 10ec:8139", "pci ffff:ffff" }, { "pci 10ec:8139", "pci 10ec" }, { "file a.c a.c", "file a.sys a.sys" }, { "file a.c a.c", "file a.c a.c\nfile a.c b.c" },
        { "main a.c\n", "main b.c\n" }, { "main a.c\n", "main a.h\n" }, { "version 1", "version x" }, { "drvget-recipe 1", "drvget-recipe 2" }, { "main a.c\n", "main a.c\nexec rm\n" },
        { "version 1\n", "" }, { "caps mmio\n", "" }, { "pci 10ec:8139\n", "" }, { "file a.c a.c", "file a.c a.c sha256 xyz" },
    };
    for (unsigned i = 0; i < sizeof mut / sizeof *mut; i++) {
        char t[600]; const char* p = strstr(base, mut[i].from); CHECK(p);
        snprintf(t, sizeof t, "%.*s%s%s", (int)(p - base), base, mut[i].to, p + strlen(mut[i].from));
        if (rec(t, &r, &err) == 0) { printf("мутация %u принята: %s\n", i, mut[i].to); exit(1); }
    }
    CHECK(rec("", &r, &err) != 0 && rec("name rl\n", &r, &err) != 0);
    char big[DG_RECIPE_MAX + 10]; memset(big, '#', sizeof big); CHECK(dg_parse_recipe(big, sizeof big, &r, &err) != 0);
    CHECK(dg_parse_recipe("drvget-recipe 1\nname r\0l\n", 25, &r, &err) != 0);

    /* исходники и лицензия */
    char lic[24];
    CHECK(scan("/* SPDX-License-Identifier: BSD-2-Clause */\nint a;\n", lic, &err) == 0 && !strcmp(lic, "BSD-2-Clause"));
    CHECK(scan("// SPDX-License-Identifier: ISC\nint a;\n", lic, &err) == 0 && !strcmp(lic, "ISC"));
    CHECK(scan("/* SPDX-License-Identifier: MIT*/\nint a;\n", lic, &err) == 0 && !strcmp(lic, "MIT"));
    CHECK(scan("/* SPDX-License-Identifier: GPL-2.0 */\nint a;\n", lic, &err) != 0 && strstr(err, "GPL"));
    CHECK(scan("/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */\n", lic, &err) != 0);
    CHECK(scan("/* SPDX-License-Identifier: BSD-3-Clause OR MIT */\n", lic, &err) != 0 && strstr(err, "составное"));
    CHECK(scan("/* SPDX-License-Identifier: MIT */\n/* SPDX-License-Identifier: BSD-2-Clause */\n", lic, &err) != 0 && strstr(err, "несколько SPDX"));
    CHECK(scan("/* SPDX-License-Identifier: Apache-2.0 */\n", lic, &err) != 0);
    CHECK(scan("/* SPDX-License-Identifier: BSD-2-Clause */\n/* This program is free software; GNU General Public License */\n", lic, &err) != 0);
    CHECK(scan("/* SPDX-License-Identifier: BSD-2-Clause */\n/* licensed GPLv2 */\n", lic, &err) != 0);
    CHECK(scan("/* SPDX-License-Identifier: BSD-2-Clause */\n/* LGPL */\n", lic, &err) != 0);
    CHECK(scan("/* SPDX-License-Identifier: BSD-2-Clause */\nint xgplot;\n", lic, &err) == 0);       /* «gpl» внутри слова - не метка */
    CHECK(scan("int a;\n", lic, &err) != 0 && strstr(err, "лицензия"));
    const char* bsd2 = "Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer. THIS SOFTWARE IS PROVIDED BY THE AUTHOR AS IS.";
    CHECK(scan(bsd2, lic, &err) == 0 && !strcmp(lic, "BSD-2-Clause"));
    const char* bsd2_wrapped = "Redistribution and\nuse in source and\nbinary forms, with or without modification, are permitted provided that the following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer. THIS SOFTWARE IS PROVIDED BY THE AUTHOR AS IS.";
    CHECK(scan(bsd2_wrapped, lic, &err) == 0 && !strcmp(lic, "BSD-2-Clause"));
    const char* bsd3 = "Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met: 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer. 3. Neither the name of the author nor contributors may be used to endorse or promote products derived from this software without specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE AUTHOR AS IS.";
    CHECK(scan(bsd3, lic, &err) == 0 && !strcmp(lic, "BSD-3-Clause"));
    CHECK(scan("/* Redistribution and use in source and binary forms. All advertising materials mentioning features. THIS SOFTWARE IS PROVIDED */\n", lic, &err) != 0);
    CHECK(scan("/* All advertising materials\n * mentioning features or use of this software must display the acknowledgement. */\n", lic, &err) != 0);
    CHECK(scan("/* Permission is hereby granted, free of charge, to any person. THE SOFTWARE IS PROVIDED \"AS IS\" */\n", lic, &err) == 0 && strstr(lic, "MIT"));
    CHECK(scan("/* Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted */\n", lic, &err) == 0 && strstr(lic, "ISC"));
    CHECK(scan("/* Copyright (C) 1993, David Greenman. This software may be used, modified, copied, distributed, and sold, in both source and binary form provided that the above copyright and these terms are retained. Under no circumstances is the author responsible for the proper functioning of this software, nor does the author assume any responsibility */\n", lic, &err) == 0 && !strcmp(lic, "LicenseRef-Greenman"));
    CHECK(scan("/* This software may be used, modified, copied, distributed, and sold, in both source and binary form provided that the above copyright and these terms are retained. */\n", lic, &err) != 0);   /* только обе фразы вместе */
    uint8_t mz[200]; memset(mz, 0, sizeof mz); mz[0] = 'M'; mz[1] = 'Z';
    CHECK(dg_scan_source("net.c", mz, sizeof mz, lic, &err) != 0 && strstr(err, "бинарник"));
    uint8_t elf[64] = { 0x7f, 'E', 'L', 'F' }; CHECK(dg_scan_source("a.c", elf, sizeof elf, lic, &err) != 0);
    uint8_t nul[64]; memset(nul, 'a', sizeof nul); nul[10] = 0; CHECK(dg_scan_source("a.c", nul, sizeof nul, lic, &err) != 0);
    uint8_t gz[16] = { 0x1f, 0x8b, 8 }; CHECK(dg_scan_source("a.c", gz, sizeof gz, lic, &err) != 0);
    CHECK(dg_scan_source("a.c", (const uint8_t*)"", 0, lic, &err) != 0);
    static uint8_t huge[DG_SRC_MAX + 1]; memset(huge, 'a', sizeof huge); CHECK(dg_scan_source("a.c", huge, sizeof huge, lic, &err) != 0);

    /* хеш набора: порядок и границы файлов влияют; префикс подтверждения */
    dg_hash_t h; uint8_t s1[32], s2[32], s3[32];
    dg_set_begin(&h); dg_set_add(&h, "a.c", (const uint8_t*)"xy", 2); dg_set_add(&h, "b.h", (const uint8_t*)"z", 1); dg_set_end(&h, s1);
    dg_set_begin(&h); dg_set_add(&h, "a.c", (const uint8_t*)"x", 1); dg_set_add(&h, "b.h", (const uint8_t*)"yz", 2); dg_set_end(&h, s2);
    dg_set_begin(&h); dg_set_add(&h, "a.c", (const uint8_t*)"xy", 2); dg_set_add(&h, "b.h", (const uint8_t*)"z", 1); dg_set_end(&h, s3);
    CHECK(memcmp(s1, s2, 32) && !memcmp(s1, s3, 32));
    char hx[65]; dg_hex(s1, 32, hx); CHECK(strlen(hx) == 64);
    uint8_t back[32]; CHECK(dg_hex_parse(hx, back, 32) && !memcmp(back, s1, 32) && !dg_hex_parse("zz", back, 1) && !dg_hex_parse("abc", back, 1));
    char pre[20]; memcpy(pre, hx, 8); pre[8] = 0;
    CHECK(dg_hex_prefix(pre, s1) && dg_hex_prefix(hx, s1) && !dg_hex_prefix(pre, s2));
    memcpy(pre, hx, 7); pre[7] = 0; CHECK(!dg_hex_prefix(pre, s1));                      /* короче 8 знаков не принимается */
    memcpy(pre, hx, 8); pre[8] = 0; pre[0] = pre[0] == 'a' ? 'b' : 'a'; CHECK(!dg_hex_prefix(pre, s1));

    /* Strict system driver catalogue parser and exact PCI matching. */
    const char *catalog =
        "kira-driver-index 1\n"
        "# pci VVVV:DDDD name url sha256\n"
        "pci 10ec:8139 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    dg_index_entry_t ix[DG_INDEX_ENTRIES];
    uint32_t ixn = 0;
    CHECK(dg_parse_index(catalog, (uint32_t)strlen(catalog), ix, DG_INDEX_ENTRIES, &ixn, &err) == 0);
    CHECK(ixn == 1 && ix[0].vendor == 0x10ec && ix[0].device == 0x8139);
    CHECK(!strcmp(ix[0].name, "rl") && !strcmp(ix[0].url, "https://example.test/rl/drvget.rec") && strlen(ix[0].recipe_sha) == 64);
    CHECK(ix[0].publisher_required == 0);
    const char *signed_catalog =
        "kira-driver-index 1\n"
        "pci 10ec:8139 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef publisher-required\n";
    CHECK(dg_parse_index(signed_catalog, (uint32_t)strlen(signed_catalog), ix, DG_INDEX_ENTRIES, &ixn, &err) == 0);
    CHECK(ixn == 1 && ix[0].publisher_required == 1);
    CHECK(dg_index_find_pci(ix, ixn, 0x10ec, 0x8139) == 0);
    CHECK(dg_index_find_pci(ix, ixn, 0xffff, 0xffff) == -1);
    CHECK(dg_index_find_name(ix, ixn, "rl") == 0 && dg_index_find_name(ix, ixn, "missing") == -1);
    const char *bad_catalog[] = {
        "kira-driver-index 2\npci 10ec:8139 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
        "kira-driver-index 1\npci 10ec:8139 rl http://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
        "kira-driver-index 1\npci 10ec:813 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
        "kira-driver-index 1\npci 10ec:8139 rl https://example.test/rl/drvget.rec xyz\n",
        "kira-driver-index 1\npci 10ec:8139 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\npci 1234:5678 rl https://example.test/other/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n",
        "kira-driver-index 1\npci 10ec:8139 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\npci 10ec:8139 rl-alt https://example.test/other/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    };
    const char *bad_policy =
        "kira-driver-index 1\n"
        "pci 10ec:8139 rl https://example.test/rl/drvget.rec 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef unknown-policy\n";
    CHECK(dg_parse_index(bad_policy, (uint32_t)strlen(bad_policy), ix, DG_INDEX_ENTRIES, &ixn, &err) != 0);
    for (unsigned i = 0; i < sizeof bad_catalog / sizeof *bad_catalog; i++)
        CHECK(dg_parse_index(bad_catalog[i], (uint32_t)strlen(bad_catalog[i]), ix, DG_INDEX_ENTRIES, &ixn, &err) != 0);

    printf("drvget: https-only URLs, recipe parsing and mutations, license/GPL/binary rejection, set hash, confirmation prefix passed\n");
    return 0;
}
