import tempfile
import unittest
from pathlib import Path

from ci.release_gate import check


class ReleaseGateTests(unittest.TestCase):
    def test_current_gap_inventory_blocks_release(self):
        errors = check(Path("docs/RELEASE_GAPS.md"))
        self.assertTrue(errors)
        self.assertTrue(any("Gate" in error for error in errors))

    def test_all_sections_need_closed_and_evidence(self):
        text = "\n".join(
            f"## {i}. Area {i}\n\n**Gate:** CLOSED\n**Evidence:** tests/area_{i}.log"
            for i in range(1, 8)
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gaps.md"
            path.write_text(text, encoding="utf-8")
            self.assertEqual(check(path), [])

    def test_missing_evidence_blocks_release(self):
        text = "\n".join(
            f"## {i}. Area {i}\n\n**Gate:** CLOSED"
            for i in range(1, 8)
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gaps.md"
            path.write_text(text, encoding="utf-8")
            self.assertTrue(any("Evidence" in error for error in check(path)))


if __name__ == "__main__":
    unittest.main()
