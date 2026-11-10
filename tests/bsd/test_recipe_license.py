#!/usr/bin/env python3
import importlib.util
import os
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = importlib.util.spec_from_file_location("bsd_recipe", os.path.join(ROOT, "tools", "bsd_recipe.py"))
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

BSD2 = """Copyright (c) Example.
All rights reserved.
Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS AS IS.
"""
BSD3 = BSD2 + "Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.\n"
MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, to deal in the Software without restriction.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
"""
ISC = """Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
"""


class LicenseClassificationTests(unittest.TestCase):
    def test_spdx_single_license(self):
        self.assertEqual(MOD.classify_license(b"/* SPDX-License-Identifier: BSD-2-Clause */"), "BSD-2-Clause")

    def test_spdx_expression_is_rejected(self):
        with self.assertRaises(ValueError):
            MOD.classify_license(b"/* SPDX-License-Identifier: BSD-2-Clause OR MIT */")

    def test_bsd_two_clause_is_not_mislabeled_three_clause(self):
        self.assertEqual(MOD.classify_license(BSD2.encode()), "BSD-2-Clause")

    def test_bsd_three_clause(self):
        self.assertEqual(MOD.classify_license(BSD3.encode()), "BSD-3-Clause")

    def test_mit_and_isc(self):
        self.assertEqual(MOD.classify_license(MIT.encode()), "MIT")
        self.assertEqual(MOD.classify_license(ISC.encode()), "ISC")

    def test_unknown_license_fails_closed(self):
        with self.assertRaises(ValueError):
            MOD.classify_license(b"Copyright. All rights reserved.")

    def test_bsd_advertising_clause_is_rejected(self):
        text = BSD2 + "3. All advertising materials mentioning features or use of this software must display this acknowledgement.\\n"
        with self.assertRaisesRegex(ValueError, "advertising clause"):
            MOD.classify_license(text.encode())

    def test_custom_greenman_notice_is_not_mislabeled_as_bsd(self):
        text = """Copyright (C) 1993, David Greenman. This software may be used, modified,
        copied, distributed, and sold, in both source and binary form provided that
        the above copyright and these terms are retained. Under no circumstances is
        the author responsible for the proper functioning of this software."""
        with self.assertRaisesRegex(ValueError, "Greenman"):
            MOD.classify_license(text.encode())

    def test_mixed_source_licenses_fail_closed(self):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "dev"))
            with open(os.path.join(root, "dev", "a.c"), "wb") as f:
                f.write(BSD2.encode())
            with open(os.path.join(root, "dev", "b.h"), "wb") as f:
                f.write(BSD3.encode())
            with self.assertRaisesRegex(ValueError, "mixed licenses"):
                MOD.recipe_license(root, ["dev/a.c", "dev/b.h"])


if __name__ == "__main__":
    unittest.main()
