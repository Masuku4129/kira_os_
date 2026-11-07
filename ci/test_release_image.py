import unittest

from ci.release_image import validate


class ReleaseImageTests(unittest.TestCase):
    def report(self):
        return {
            "iso_files": ["boot/kira.bin"],
            "kfs_files": ["/bin/shell"],
            "runtime_library_candidates": 0,
            "dynamic_dependency_count": 0,
        }

    def test_clean_release_image(self):
        self.assertEqual(validate(self.report()), [])

    def test_unsigned_edu_package_is_rejected(self):
        report = self.report()
        report["kfs_files"].append("/drv/edu.kdrv")
        self.assertTrue(any("unsigned EDU" in e for e in validate(report)))

    def test_runtime_library_is_rejected(self):
        report = self.report()
        report["runtime_library_candidates"] = 1
        self.assertTrue(any("runtime library" in e for e in validate(report)))

    def test_dynamic_dependency_is_rejected(self):
        report = self.report()
        report["dynamic_dependency_count"] = 1
        self.assertTrue(any("dynamic dependencies" in e for e in validate(report)))

    def test_missing_inventory_is_rejected(self):
        self.assertTrue(validate({"iso_files": [], "kfs_files": []}))


if __name__ == "__main__":
    unittest.main()
