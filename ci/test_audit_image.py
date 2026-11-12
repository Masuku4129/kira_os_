import struct
import unittest

from ci.audit_image import elf_dependencies


def elf_header(elf_type, phoff=0, phentsize=0, phnum=0, size=64):
    data = bytearray(size)
    data[:4] = b"\x7fELF"
    data[4] = 2
    data[5] = 1
    struct.pack_into("<H", data, 16, elf_type)
    struct.pack_into("<Q", data, 32, phoff)
    struct.pack_into("<HH", data, 54, phentsize, phnum)
    return data


class ElfAuditTests(unittest.TestCase):
    def test_non_elf_is_ignored(self):
        self.assertEqual(elf_dependencies(b"not an ELF", "test"), [])

    def test_relocatable_grub_module_has_no_runtime_program_headers(self):
        data = elf_header(1)
        self.assertEqual(elf_dependencies(data, "grub-module"), [])

    def test_truncated_executable_program_header_table_is_rejected(self):
        data = elf_header(2, phoff=64, phentsize=56, phnum=1)
        with self.assertRaises(ValueError):
            elf_dependencies(data, "truncated")

    def test_dynamic_loader_segment_is_detected(self):
        data = elf_header(2, phoff=64, phentsize=56, phnum=1, size=121)
        struct.pack_into("<IIQQQQQQ", data, 64, 3, 4, 120, 0, 0, 1, 1, 1)
        self.assertIn("PT_INTERP (dynamic loader)", elf_dependencies(data, "interp"))

    def test_shared_library_needed_tag_is_detected(self):
        data = elf_header(3, phoff=64, phentsize=56, phnum=1, size=152)
        struct.pack_into("<IIQQQQQQ", data, 64, 2, 4, 120, 0, 0, 32, 32, 8)
        struct.pack_into("<qQqQ", data, 120, 1, 0, 0, 0)
        self.assertIn("DT_NEEDED (shared library)", elf_dependencies(data, "needed"))


if __name__ == "__main__":
    unittest.main()
