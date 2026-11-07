import tempfile
import unittest
from pathlib import Path

from ci.release_gate import check


def fixture_text(root: Path, evidence: str = "tests/area_{i}.log") -> str:
    for i in range(1, 8):
        path = root / "tests" / f"area_{i}.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"verified evidence {i}\n", encoding="utf-8")
    return "\n".join(
        f"## {i}. Area {i}\n\n**Gate:** CLOSED\n**Evidence:** {evidence.format(i=i)}"
        for i in range(1, 8)
    )


class ReleaseGateTests(unittest.TestCase):
    def test_current_gap_inventory_blocks_release(self):
        errors = check(Path("docs/RELEASE_GAPS.md"))
        self.assertTrue(errors)
        self.assertTrue(any("Gate" in error for error in errors))

    def test_all_sections_need_closed_and_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "gaps.md"
            path.write_text(fixture_text(root), encoding="utf-8")
            self.assertEqual(check(path), [])

    def test_missing_evidence_file_blocks_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "gaps.md"
            path.write_text(fixture_text(root, "tests/missing_{i}.log"), encoding="utf-8")
            errors = check(path)
            self.assertEqual(sum("does not exist" in error for error in errors), 7)

    def test_path_traversal_blocks_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "gaps.md"
            path.write_text(fixture_text(root, "../outside_{i}.log"), encoding="utf-8")
            errors = check(path)
            self.assertEqual(sum("unsafe evidence path" in error for error in errors), 7)

    def test_empty_evidence_file_blocks_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "gaps.md"
            path.write_text(fixture_text(root), encoding="utf-8")
            (root / "tests" / "area_4.log").write_text("", encoding="utf-8")
            self.assertTrue(any("empty: tests/area_4.log" in error for error in check(path)))

    def test_duplicate_required_section_blocks_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "gaps.md"
            text = fixture_text(root) + "\n\n## 1. Duplicate"
            path.write_text(text, encoding="utf-8")
            self.assertTrue(any("section 1 is duplicated" in error for error in check(path)))


if __name__ == "__main__":
    unittest.main()
