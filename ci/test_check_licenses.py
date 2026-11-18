#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kira-Proprietary
"""Самопроверка ci/check_licenses.py: на чистом репозитории замечаний нет; каждая мутация обязана ронять проверку."""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_licenses as C

DOC = C.read("docs/THIRD_PARTY.md")
FILES = C.tracked()


class T(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(C.check(), [])

    def test_removed_files_entry_fails(self):
        doc = DOC.replace("prebuilt/BOOTX64.EFI | third | BSD-3-Clause AND BSD-2-Clause | gnu-efi\n", "")
        errs = C.check(doc=doc)
        self.assertTrue(any("prebuilt/BOOTX64.EFI" in e for e in errs), errs)

    def test_removed_component_fails(self):
        doc = re.sub(r"^micropython \|.*\n", "", DOC, flags=re.M)
        self.assertTrue(C.check(doc=doc))

    def test_unlisted_pin_fails(self):
        doc = DOC.replace("ecfdd5d6f9be971852003c2049600dc7b3e2a838", "0" * 40)
        self.assertTrue(any("ecfdd5d6" in e for e in C.check(doc=doc)))

    def test_new_third_party_file_fails(self):
        errs = C.check(files=FILES + ["vendor/new.c"], extra_heads={"vendor/new.c": "/* Copyright (c) 2020 Somebody */\n"})
        self.assertTrue(any("vendor/new.c" in e for e in errs), errs)

    def test_new_file_without_header_fails(self):
        errs = C.check(files=FILES + ["new_ours.c"], extra_heads={"new_ours.c": "int x;\n"})
        self.assertTrue(any("new_ours.c" in e for e in errs), errs)

    def test_new_file_with_kira_spdx_ok(self):
        errs = C.check(files=FILES + ["new_ours.c"], extra_heads={"new_ours.c": "/* SPDX-License-Identifier: LicenseRef-Kira-Proprietary */\n"})
        self.assertEqual(errs, [])

    def test_foreign_spdx_unlisted_fails(self):
        errs = C.check(files=FILES + ["new_mit.c"], extra_heads={"new_mit.c": "// SPDX-License-Identifier: MIT\n"})
        self.assertTrue(any("new_mit.c" in e for e in errs), errs)

    def test_new_binary_fails(self):
        errs = C.check(files=FILES + ["prebuilt/new.bin"])
        self.assertTrue(any("prebuilt/new.bin" in e for e in errs), errs)

    def test_stale_entry_fails(self):
        errs = C.check(doc=DOC.replace("# licenses:files\n", "# licenses:files\nnonexistent/* | own | X | kira-own\n"))
        self.assertTrue(any("nonexistent" in e for e in errs), errs)

    def test_missing_notice_fails(self):
        self.assertTrue(C.check(notice="пусто"))


if __name__ == "__main__":
    unittest.main(verbosity=1)
