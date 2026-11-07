#!/usr/bin/env python3
"""Host-side contract tests for the offline KDRV public trust-table generator."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "kdrvtrust.py"


class TrustManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "keys.json"
        self.output = self.root / "kdrv_trust.h"

    def write(self, keys):
        self.manifest.write_text(json.dumps({"format": 1, "keys": keys}), encoding="utf-8")

    def run_tool(self, *args):
        return subprocess.run(
            [sys.executable, str(TOOL), str(self.manifest), "--output", str(self.output), *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        )

    def test_generates_public_keys_and_revocation_flags(self):
        self.write([
            {"id": "owner-key-000001", "public_key": "11" * 32},
            {"id": "old-key-0000001", "public_key": "22" * 32, "revoked": True},
        ])
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        header = self.output.read_text(encoding="utf-8")
        self.assertIn("KDRV_SIG_KEY_REVOKED", header)
        self.assertIn("owner-key-000001", header)
        self.assertNotIn("PRIVATE", header)
        self.assertEqual(self.run_tool("--check").returncode, 0)

    def test_check_does_not_rewrite_stale_header(self):
        self.write([{"id": "owner-key-000001", "public_key": "11" * 32}])
        self.output.write_text("stale", encoding="utf-8")
        result = self.run_tool("--check")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.output.read_text(encoding="utf-8"), "stale")

    def test_duplicate_ids_rejected(self):
        row = {"id": "owner-key-000001", "public_key": "11" * 32}
        self.write([row, row])
        self.assertEqual(self.run_tool().returncode, 2)

    def test_bad_key_size_rejected(self):
        self.write([{"id": "owner-key-000001", "public_key": "11" * 31}])
        self.assertEqual(self.run_tool().returncode, 2)

    def test_all_zero_key_rejected(self):
        self.write([{"id": "owner-key-000001", "public_key": "00" * 32}])
        self.assertEqual(self.run_tool().returncode, 2)

    def test_all_revoked_rejected(self):
        self.write([{"id": "owner-key-000001", "public_key": "11" * 32, "revoked": True}])
        self.assertEqual(self.run_tool().returncode, 2)

    def test_more_than_limit_rejected(self):
        self.write([
            {"id": f"key-{i:012d}", "public_key": f"{i + 1:064x}"}
            for i in range(33)
        ])
        self.assertEqual(self.run_tool().returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
