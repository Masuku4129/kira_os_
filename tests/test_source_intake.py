import hashlib
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.kira_source_intake import IntakeError, build_package, read_archive


class SourceIntakeTests(unittest.TestCase):
    def make_zip(self, root, members):
        path = os.path.join(root, "input.zip")
        with zipfile.ZipFile(path, "w") as z:
            for name, data in members.items():
                z.writestr(name, data)
        return path

    def test_preserves_linux_gpl_source_without_claiming_it_is_runnable(self):
        with tempfile.TemporaryDirectory() as td:
            src = self.make_zip(td, {
                "driver/main.c": b"// SPDX-License-Identifier: GPL-2.0-only\nint probe(void){return 0;}\n",
                "driver/Makefile": b"obj-m += main.o\n",
                "driver/COPYING": b"GNU GENERAL PUBLIC LICENSE\n"
            })
            out = os.path.join(td, "linux-driver.ksrc")
            m = build_package(src, out, "https://example.invalid/linux-driver")
            self.assertEqual(m["license"]["spdx_identifiers_found"], ["GPL-2.0-only"])
            self.assertEqual(m["compatibility"]["status"], "NOT_ASSESSED")
            self.assertEqual(m["build"]["status"], "NOT_ATTEMPTED")
            self.assertFalse(m["runtime"]["installable"])
            with zipfile.ZipFile(out) as z:
                self.assertEqual(z.read("source/driver/main.c"), b"// SPDX-License-Identifier: GPL-2.0-only\nint probe(void){return 0;}\n")
                self.assertIn("GPL-2.0-only", z.read("manifest.json").decode())

    def test_unknown_license_is_preserved_but_flagged(self):
        with tempfile.TemporaryDirectory() as td:
            src = self.make_zip(td, {"x.c": b"int x;\n"})
            m = build_package(src, os.path.join(td, "x.ksrc"))
            self.assertEqual(m["license"]["status"], "UNKNOWN_REQUIRES_REVIEW")
            self.assertEqual(m["file_count"], 1)

    def test_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as td:
            src = self.make_zip(td, {"../escape.c": b"int x;\n"})
            with self.assertRaises(IntakeError):
                read_archive(src)

    def test_rejects_absolute_paths(self):
        with tempfile.TemporaryDirectory() as td:
            src = self.make_zip(td, {"/tmp/escape.c": b"int x;\n"})
            with self.assertRaises(IntakeError):
                read_archive(src)

    def test_rejects_duplicate_members(self):
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, "dupe.zip")
            with zipfile.ZipFile(src, "w") as z:
                z.writestr("a.c", b"one")
                z.writestr("a.c", b"two")
            with self.assertRaises(IntakeError):
                read_archive(src)

    def test_hash_is_stable_for_same_tree_order(self):
        with tempfile.TemporaryDirectory() as td:
            a = self.make_zip(td, {"b.h": b"int b;", "a.c": b"int a;"})
            m = build_package(a, os.path.join(td, "a.ksrc"))
            b = self.make_zip(td, {"a.c": b"int a;", "b.h": b"int b;"})
            m2 = build_package(b, os.path.join(td, "b.ksrc"))
            self.assertEqual(m["source_sha256"], m2["source_sha256"])

    def test_license_unknown_does_not_mean_license_approved(self):
        with tempfile.TemporaryDirectory() as td:
            src = self.make_zip(td, {"drv.c": b"/* no license header */\n"})
            m = build_package(src, os.path.join(td, "x.ksrc"))
            self.assertNotEqual(m["license"]["status"], "IDENTIFIED")
            self.assertIn("does not grant redistribution", m["license"]["policy"])


if __name__ == "__main__":
    unittest.main()
